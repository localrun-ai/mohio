#include "wikore/ingest/worker.hpp"
#include "wikore/redis.hpp"
#include <drogon/drogon.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <cstring>
#include <format>
#include <random>
#include <string_view>
#include <unistd.h>

namespace wikore::ingest {

namespace {

// Worker_id splitter is the LAST ':' (per-tenant proc-key shape). The
// hostname-included worker_id format chosen by IngestWorker::ctor uses
// '-' separators and never embeds ':', so the proc-key parser's
// rfind(':') in PollingFallback is unambiguous.

// Per V008 the per-tenant queue is `lr:ingest:q:{company_id}`.
constexpr std::string_view kQueuePrefix = "lr:ingest:q:";

// Per-worker processing list. A job moves atomically from the source
// queue to this list when the worker claims it (LMOVE). On clean
// completion the worker LREMs it; on crash, the sweep reaper finds the
// list whose heartbeat key has expired and re-enqueues its contents.
constexpr std::string_view kProcPrefix  = "lr:ingest:proc:";

// Per-worker heartbeat. The sweep reaper considers the worker dead
// when this key is absent.
constexpr std::string_view kHeartbeatPrefix = "lr:ingest:hb:";

// UUID format check (8-4-4-4-12 hex with hyphens). Used to reject
// scanned queue/proc keys whose tenant suffix isn't a valid UUID,
// which prevents a malformed Redis key (e.g. left by an old test or a
// misconfigured producer) from being treated as a real tenant and
// looping in the dispatch path with continually-failing UUID casts.
constexpr bool is_uuid_v4_ish(std::string_view s) noexcept
{
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else {
            const char c = s[i];
            const bool hex = (c >= '0' && c <= '9')
                          || (c >= 'a' && c <= 'f')
                          || (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
    }
    return true;
}

// Extract the company_id portion of `lr:ingest:q:<uuid>` IF the suffix
// is a well-formed UUID. Returns empty for malformed keys.
std::string_view tenant_from_key(std::string_view key)
{
    if (key.size() > kQueuePrefix.size()
        && key.starts_with(kQueuePrefix))
    {
        const auto tenant = key.substr(kQueuePrefix.size());
        if (is_uuid_v4_ish(tenant))
            return tenant;
    }
    return {};
}

// Sleep on the Drogon event loop without blocking a thread.
// Direct awaiter (no IIFE coroutine lambda) -- a capturing coroutine
// lambda would dangle its captures past the await-suspension point.
drogon::Task<void> co_sleep(std::chrono::milliseconds d)
{
    struct Awaiter {
        trantor::EventLoop*           loop;
        std::chrono::milliseconds     delay;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            loop->runAfter(static_cast<double>(delay.count()) / 1000.0,
                           [h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{drogon::app().getLoop(), d};
}

} // namespace

IngestWorker::IngestWorker(application::IngestDocumentVersionUseCase use_case,
                             ShutdownPredicate                          shutdown_requested,
                             Options                                    opts)
    : use_case_(std::move(use_case))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    if (opts_.worker_id.empty()) {
        // Collision-resistant ID: hostname + pid + 64-bit random.
        // The previous time-truncated ID collided across two processes
        // that started within the same 16ms window, and across a single
        // host restart with the same pid recycling. Hostname disambiguates
        // multi-host deployments; pid disambiguates per-host; 64-bit
        // random covers same-host same-pid replay (process restart).
        char host[256] = {};
        if (::gethostname(host, sizeof(host) - 1) != 0)
            std::strcpy(host, "unknown-host");
        std::random_device rd;
        std::uniform_int_distribution<std::uint64_t> dist;
        const auto suffix = dist(rd);

        opts_.worker_id = std::string("wikore-ingest-")
            + host
            + "-" + std::to_string(static_cast<long long>(::getpid()))
            + "-" + std::format("{:016x}", suffix);
    }
}

drogon::Task<void> IngestWorker::rescan_tenants_locked()
{
    auto keys = wikore::Redis::scan_keys(std::string(kQueuePrefix) + "*", /*limit=*/4096);
    tenants_.clear();
    tenants_.reserve(keys.size());
    for (auto& k : keys) {
        auto t = tenant_from_key(k);
        if (!t.empty())
            tenants_.emplace_back(t);
    }
    // Deterministic rotation order so a long-lived worker doesn't reshuffle
    // priorities unpredictably between scans.
    std::sort(tenants_.begin(), tenants_.end());
    cursor_ %= std::max<std::size_t>(tenants_.size(), 1);
    last_scan_ = std::chrono::steady_clock::now();
    co_return;
}

drogon::Task<bool> IngestWorker::try_one_rotation()
{
    if (tenants_.empty()) co_return false;

    const std::size_t start = cursor_;
    do {
        if (shutdown_()) co_return false;

        const auto& company_id = tenants_[cursor_];
        cursor_ = (cursor_ + 1) % tenants_.size();

        const auto key = std::string(kQueuePrefix) + company_id;
        // Per-tenant proc key so the reaper can recover the source-
        // tenant from the key itself rather than trusting an
        // untrusted payload field. Format:
        //   lr:ingest:proc:{worker_id}:{tenant_uuid}
        // Worker_id has no ':'; tenant is a UUID (no ':' either), so
        // splitting on the LAST ':' is unambiguous.
        const std::string proc_key =
            std::string(kProcPrefix) + opts_.worker_id + ":" + company_id;
        // Reliable claim: atomically move the tail of the source queue
        // to the head of this worker's per-tenant processing list. If
        // the worker dies before the use case's CAS-claim, the sweep
        // reaper transfers the entry back to the source queue using
        // the tenant encoded in proc_key.
        auto payload = wikore::Redis::lmove_right_left(key, proc_key);
        if (!payload) continue;

        IngestJob job;
        if (auto err = glz::read_json(job, *payload); err) {
            spdlog::warn("[ingest-worker] discarding malformed job on {}: {}",
                         key, glz::format_error(err, *payload));
            // Drop the malformed payload from our processing list so it
            // doesn't get requeued forever by the sweep reaper.
            wikore::Redis::lrem(proc_key, 1, *payload);
            jobs_failed_.fetch_add(1);
            continue;
        }
        // Defence in depth: the queue key authoritatively names the tenant.
        if (job.company_id != company_id) {
            spdlog::warn("[ingest-worker] tenant mismatch: queue={} job.company_id={}; "
                         "overriding to queue tenant",
                         company_id, job.company_id);
            job.company_id = company_id;
        }
        co_await dispatch(std::move(job), *payload, proc_key);
        co_return true;
    } while (cursor_ != start);

    co_return false;
}

drogon::Task<void> IngestWorker::dispatch(IngestJob   job,
                                          std::string payload,
                                          std::string proc_key)
{
    application::IngestDocumentVersionCmd cmd{
        .company_id          = job.company_id,
        .document_id         = job.document_id,
        .document_version_id = job.document_version_id,
        .file_path           = job.file_path,
        .embed_model_id      = job.embed_model_id,
        // Pass the raw JSON payload so the use case can CAS-claim the
        // row (pending -> processing AND persist payload atomically).
        // The CAS absorbs duplicate delivery from the orphan reaper
        // and from concurrent worker races without resurrecting
        // terminal rows.
        .ingest_job_payload  = payload,
    };

    // RequestContext for the dispatched job:
    //   * tenant = the company that owns the queue
    //   * principal = the service identity of this worker; audit_log rows
    //     will record actor_type='service'
    //   * trace_id = derived from the document_version_id so a re-enqueued
    //     job under the polling fallback hits ON CONFLICT DO NOTHING on the
    //     outbox INSERT (idempotency_key includes trace_id)
    RequestContext ctx{
        .tenant    = {.company_id = job.company_id},
        .principal = {.user_id            = uuid_generate(),
                      .email              = opts_.worker_id,
                      .display_name       = opts_.worker_id,
                      .is_admin           = false,
                      .is_service_account = true},
        .span      = {.trace_id = "ingest:" + job.document_version_id,
                      .span_id  = uuid_generate()},
        .deadline  = std::chrono::steady_clock::now() + opts_.deadline_per_job,
    };

    spdlog::info("[ingest-worker] dispatch company={} doc={} version={}",
                 job.company_id, job.document_id, job.document_version_id);

    auto result = co_await use_case_.execute(std::move(ctx), std::move(cmd));
    if (!result) {
        // INFRA error (DB unavailable, deadline exceeded, etc.). The
        // pipeline never reached a CAS-claim or a CAS-claim failed
        // mid-flight in a non-deterministic way. The proc entry may
        // be the sole copy of a still-recoverable job; we cannot just
        // LREM. We can't wait for heartbeat expiry either: this
        // worker is alive and refreshing its heartbeat.
        //
        // Atomically transfer the proc entry back to its source queue.
        // The Lua LREM-then-conditional-LPUSH primitive guarantees
        // only one publisher per payload. The claim_for_processing
        // CAS at the next pickup safely absorbs any case where the
        // row reached a terminal state in the meantime.
        //
        // If the transfer fails (Redis unavailable), we retry with
        // backoff. As a last resort, we DELETE our own heartbeat key
        // so the orphan reaper can take over; this opens the proc
        // list to the reaper across all our tenants, but in a Redis-
        // unavailable scenario that's an acceptable trade-off (the
        // alternative is permanently losing the job).
        jobs_failed_.fetch_add(1);
        const auto src_key = std::string(kQueuePrefix) + job.company_id;
        constexpr int  kXferAttempts   = 4;
        constexpr auto kXferRetryDelay = std::chrono::milliseconds(250);
        int xfer = -1;
        for (int attempt = 0; attempt < kXferAttempts && xfer < 0; ++attempt) {
            if (attempt > 0)
                co_await co_sleep(kXferRetryDelay);
            xfer = wikore::Redis::transfer_proc_to_source(
                proc_key, src_key, payload);
        }
        if (xfer == 1) {
            spdlog::warn("[ingest-worker] dispatch failed: kind={} msg={}; "
                         "transferred proc entry back to {} for retry",
                         static_cast<int>(result.error().kind),
                         result.error().message, src_key);
        } else if (xfer == 0) {
            spdlog::warn("[ingest-worker] dispatch failed: kind={} msg={}; "
                         "proc entry was already gone (concurrent reaper?)",
                         static_cast<int>(result.error().kind),
                         result.error().message);
        } else {
            // All retries failed. Mark this worker as fatal so the
            // heartbeat timer stops refreshing and run() exits at the
            // next iteration boundary. The supervisor (systemd /
            // orchestrator) will restart the process; in the meantime
            // the orphan reaper (in the scheduler, a SEPARATE process
            // with its own Redis pool) takes over the proc list.
            //
            // DEL'ing the heartbeat here alone is not enough: the
            // periodic timer would just recreate it. Setting the flag
            // makes the timer a no-op going forward.
            spdlog::error("[ingest-worker] dispatch failed: kind={} msg={}; "
                          "AND transfer-back to {} failed {} times -- "
                          "entering fatal state; supervisor should restart",
                          static_cast<int>(result.error().kind),
                          result.error().message, src_key, kXferAttempts);
            fatal_failure_.store(true);
            // Best-effort heartbeat delete (may also fail in the same
            // Redis outage; the TTL will handle it eventually).
            wikore::Redis::del(std::string(kHeartbeatPrefix) + opts_.worker_id);
        }
        co_return;
    }

    switch (*result) {
    case application::IngestDispatchOutcome::Processed:
    case application::IngestDispatchOutcome::TerminalError:
        // Terminal outcome: row is 'done' (Processed) or 'error'
        // (TerminalError). The use case's terminal UPDATEs already
        // cleared ingest_job_payload AND ingest_claim_token inside
        // the token-gated CAS, so there is NOTHING for the worker to
        // mutate on the row here. Doing a separate post-commit
        // UPDATE would be a race: if the version is re-triggered and
        // a new worker claims it between our terminal UPDATE and our
        // proc cleanup, our unconditional UPDATE would erase the new
        // owner's freshly-persisted recovery state.
        //
        // We DO NOT transfer back the proc entry on TerminalError:
        // the row is owned-and-marked-error by us, so re-LMOVE would
        // just CAS-skip as DuplicateSkipped and waste a rotation.
        if (*result == application::IngestDispatchOutcome::Processed)
            jobs_processed_.fetch_add(1);
        else
            jobs_failed_.fetch_add(1);
        wikore::Redis::lrem(proc_key, 1, payload);
        break;
    case application::IngestDispatchOutcome::DuplicateSkipped:
    case application::IngestDispatchOutcome::OwnershipLost:
        // Another worker is the current owner of the row (either won
        // the CAS at claim time, or took over after the polling
        // fallback reset our claim). LREM our own proc entry but do
        // NOT modify document_versions -- doing so would destroy the
        // active owner's recovery state.
        jobs_duplicates_.fetch_add(1);
        wikore::Redis::lrem(proc_key, 1, payload);
        break;
    }
}

void IngestWorker::refresh_heartbeat()
{
    const auto key = std::string(kHeartbeatPrefix) + opts_.worker_id;
    wikore::Redis::set(key, "1", opts_.heartbeat_ttl);
}

drogon::Task<void> IngestWorker::run()
{
    spdlog::info("[ingest-worker] {} starting; idle_sleep={}ms rescan={}s "
                 "heartbeat_ttl={}s heartbeat_period={}s",
                 opts_.worker_id,
                 opts_.idle_sleep.count(),
                 opts_.rescan_interval.count(),
                 opts_.heartbeat_ttl.count(),
                 opts_.heartbeat_period.count());

    // Schedule heartbeat refresh on a Drogon timer so it fires
    // INDEPENDENT of dispatch. A single dispatch can take up to
    // deadline_per_job (default 600s); if we only refreshed between
    // rotations the heartbeat would expire mid-dispatch and the orphan
    // reaper would steal our in-flight job.
    //
    // The timer also checks fatal_failure_ and becomes a no-op once
    // set, so a transfer-back-permanent-failure cleanly stops
    // recreating the heartbeat key while the supervisor restarts us.
    refresh_heartbeat();
    auto* hb_loop = drogon::app().getLoop();
    const auto hb_timer = hb_loop->runEvery(
        static_cast<double>(opts_.heartbeat_period.count()),
        [worker_id = opts_.worker_id, ttl = opts_.heartbeat_ttl,
         fatal = &fatal_failure_] {
            if (fatal->load()) return;
            wikore::Redis::set(
                std::string(kHeartbeatPrefix) + worker_id, "1", ttl);
        });

    co_await rescan_tenants_locked();

    while (!shutdown_() && !fatal_failure_.load()) {
        // Rescan if the cached tenant list is stale.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_scan_ >= opts_.rescan_interval) {
            co_await rescan_tenants_locked();
        }

        bool dispatched = co_await try_one_rotation();
        if (!dispatched) {
            // Empty rotation; back off briefly. Use a short cancellable
            // sleep so SIGTERM doesn't wait the full interval.
            co_await co_sleep(opts_.idle_sleep);
        }
    }

    // Stop the heartbeat timer FIRST so no stray refresh fires after we
    // delete the key below.
    hb_loop->invalidateTimer(hb_timer);

    // Clean shutdown: drop the heartbeat so a restart with the same
    // worker_id doesn't see itself as a ghost (and so an integration
    // test can assert the key disappeared).
    wikore::Redis::del(std::string(kHeartbeatPrefix) + opts_.worker_id);
    if (fatal_failure_.load()) {
        spdlog::error("[ingest-worker] {} exiting in FATAL state; "
                      "supervisor should restart", opts_.worker_id);
    } else {
        spdlog::info("[ingest-worker] {} drained; exiting", opts_.worker_id);
    }
    co_return;
}

} // namespace wikore::ingest

// ---------------------------------------------------------------------------
// glaze meta for IngestJob so the worker can deserialize Redis payloads
// pushed by the API as JSON. The 'priority' field is optional (defaults to 0).
// ---------------------------------------------------------------------------

template <>
struct glz::meta<wikore::ingest::IngestJob> {
    using T = wikore::ingest::IngestJob;
    static constexpr auto value = glz::object(
        "company_id",          &T::company_id,
        "document_id",         &T::document_id,
        "document_version_id", &T::document_version_id,
        "file_path",           &T::file_path,
        "embed_model_id",      &T::embed_model_id,
        "priority",            &T::priority);
};