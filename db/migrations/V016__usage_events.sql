-- V016: Per-tenant usage events (LLM tokens, embedding tokens, cost ledger)
--
-- Append-only operational ledger for billing, cost accounting, and capacity
-- planning. Captures every llama-server call (chat, embed, rerank) so the
-- system can answer "how much did Acme cost us last quarter?" and
-- "which tenants are hitting their LLM budget?".
--
-- This table is foundational specifically because it cannot be reconstructed
-- after the fact: LLM calls that were never logged are lost forever. Adding
-- the schema before the first paying customer means historical reports stay
-- continuous regardless of when the billing/dashboard surfaces ship.
--
-- Design discipline mirrors audit_log (V007):
--   * NO foreign keys. Lock contention on the hot path, plus ON DELETE
--     SET NULL would null out attribution exactly when accounting needs it.
--   * Append-only via trigger.
--   * Partitioned by created_at; monthly buckets (audit_log is quarterly,
--     but usage_events volume is per-LLM-call which is much higher).
--
-- C++ writers MUST populate cost_micros at write time from the
-- model-pricing config rather than computing it later from a price table.
-- That way historical reports are stable even if pricing changes.
--
-- detail JSONB holds call-specific context (request_id, prompt_template_id,
-- chat_turn_id, error_code, etc.) without polluting the typed columns.

CREATE TABLE usage_events (
    id            BIGSERIAL,
    company_id    UUID        NOT NULL,
    user_id       UUID,                       -- NULL for system/scheduled jobs
    event_type    TEXT        NOT NULL
                              CHECK (event_type IN (
                                  'llm_chat',
                                  'llm_embed',
                                  'llm_rerank'
                              )),
    model_name    TEXT        NOT NULL,       -- 'qwen3-8b', 'bge-m3', etc.
    tokens_in     INT         NOT NULL DEFAULT 0 CHECK (tokens_in  >= 0),
    tokens_out   INT          NOT NULL DEFAULT 0 CHECK (tokens_out >= 0),
    latency_ms    INT         CHECK (latency_ms  IS NULL OR latency_ms  >= 0),
    cost_micros   BIGINT      NOT NULL DEFAULT 0 CHECK (cost_micros >= 0),
    detail        JSONB       NOT NULL DEFAULT '{}',
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (id, created_at)
) PARTITION BY RANGE (created_at);

CREATE TABLE usage_events_2026_06 PARTITION OF usage_events
    FOR VALUES FROM ('2026-06-01') TO ('2026-07-01');
CREATE TABLE usage_events_2026_07 PARTITION OF usage_events
    FOR VALUES FROM ('2026-07-01') TO ('2026-08-01');
CREATE TABLE usage_events_2026_08 PARTITION OF usage_events
    FOR VALUES FROM ('2026-08-01') TO ('2026-09-01');
CREATE TABLE usage_events_2026_09 PARTITION OF usage_events
    FOR VALUES FROM ('2026-09-01') TO ('2026-10-01');
CREATE TABLE usage_events_2026_10 PARTITION OF usage_events
    FOR VALUES FROM ('2026-10-01') TO ('2026-11-01');
CREATE TABLE usage_events_2026_11 PARTITION OF usage_events
    FOR VALUES FROM ('2026-11-01') TO ('2026-12-01');
CREATE TABLE usage_events_2026_12 PARTITION OF usage_events
    FOR VALUES FROM ('2026-12-01') TO ('2027-01-01');
CREATE TABLE usage_events_2027_01 PARTITION OF usage_events
    FOR VALUES FROM ('2027-01-01') TO ('2027-02-01');
CREATE TABLE usage_events_2027_02 PARTITION OF usage_events
    FOR VALUES FROM ('2027-02-01') TO ('2027-03-01');
CREATE TABLE usage_events_2027_03 PARTITION OF usage_events
    FOR VALUES FROM ('2027-03-01') TO ('2027-04-01');
CREATE TABLE usage_events_2027_04 PARTITION OF usage_events
    FOR VALUES FROM ('2027-04-01') TO ('2027-05-01');
CREATE TABLE usage_events_2027_05 PARTITION OF usage_events
    FOR VALUES FROM ('2027-05-01') TO ('2027-06-01');
CREATE TABLE usage_events_2027_06 PARTITION OF usage_events
    FOR VALUES FROM ('2027-06-01') TO ('2027-07-01');

-- Catch-all so a missed partition rollover never breaks INSERTs.
-- Rows here should be moved to a named partition promptly (operational).
CREATE TABLE usage_events_default PARTITION OF usage_events DEFAULT;

-- Cost report hot path: SUM(cost_micros) WHERE company_id=$1 AND created_at >= $2
CREATE INDEX usage_events_company_time_idx
    ON usage_events (company_id, created_at DESC);

-- Per-model usage report: GROUP BY model_name within a tenant.
CREATE INDEX usage_events_company_model_idx
    ON usage_events (company_id, model_name, created_at DESC);

-- ---------------------------------------------------------------------------
-- Append-only enforcement (same pattern as audit_log V007).
--
-- Defence-in-depth: also create a dedicated DB role for the application that
-- has INSERT but not UPDATE or DELETE on usage_events. Provisioning step:
--   GRANT INSERT ON usage_events TO wikore_app;
--   REVOKE UPDATE, DELETE ON usage_events FROM wikore_app;
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_usage_events_mutation()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'usage_events is append-only';
END;
$$;

CREATE TRIGGER usage_events_no_update
    BEFORE UPDATE ON usage_events
    FOR EACH ROW EXECUTE FUNCTION prevent_usage_events_mutation();

CREATE TRIGGER usage_events_no_delete
    BEFORE DELETE ON usage_events
    FOR EACH ROW EXECUTE FUNCTION prevent_usage_events_mutation();
