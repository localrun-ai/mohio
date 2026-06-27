-- V011: User deactivation, membership expiry, audit index fix, chat constraints
--
-- F3: users.deactivated_at        (Opus RECOMMENDED)
-- F5: memberships.expires_at      (Opus RECOMMENDED)
-- F8: company-first audit index   (Opus RECOMMENDED)
-- F9: chat_turns JSONB array guards (Opus OPTIONAL)

-- ---------------------------------------------------------------------------
-- F3: users.deactivated_at
--
-- Soft deactivation replaces hard-delete for most workflows. The row stays,
-- so every created_by / granted_by / wiki attribution reference remains valid
-- and audit log rows stay attributable by email (denormalized at write time).
--
-- For GDPR erasure: null out PII (email, external_sub, avatar_url, display_name)
-- and set deactivated_at. The row is kept as an FK anchor.
--
-- The UNIQUE(company_id, external_issuer, external_sub) constraint intentionally
-- stays in place so a deactivated identity cannot be silently recycled as a new
-- user row unless the constraint is explicitly dropped first.
-- ---------------------------------------------------------------------------

ALTER TABLE users
    ADD COLUMN deactivated_at TIMESTAMPTZ;

-- Hot-path active-user lookups: login validation, access resolution, member lists.
CREATE INDEX users_active_company_idx
    ON users (company_id, id)
    WHERE deactivated_at IS NULL;

-- Login-by-email fast path for active users only.
CREATE INDEX users_active_email_idx
    ON users (company_id, lower(email))
    WHERE deactivated_at IS NULL;

-- ---------------------------------------------------------------------------
-- F5: memberships.expires_at
--
-- Time-limited access for contractors, secondments, and break-glass incidents.
-- The access resolver already clamps lr:eff TTL to MIN(resource_grants.expires_at);
-- this extends that logic to MIN(memberships.expires_at) as well.
--
-- C++ resolver rule:
--   effective cache TTL = min(default 5 min,
--                             next contributing resource_grant expiry,
--                             next contributing membership expiry)
--
-- Re-granting expired memberships:
--   The unique index memberships_user_org_uidx (created in V002) enforces
--   one row per (company, user, org_unit) regardless of expiry. So re-granting
--   an expired membership is NOT a plain INSERT - it would fail with unique
--   violation against the still-present expired row. The supported flow:
--
--     UPDATE memberships
--     SET    expires_at = $new_expiry,
--            granted_at = now(),
--            granted_by = $admin_id
--     WHERE  company_id  = $cid
--       AND  user_id     = $uid
--       AND  org_unit_id = $ou_id
--       AND  (expires_at IS NULL OR expires_at <= now());
--
--   Application access-resolution queries that want only currently-valid
--   memberships must filter:  (expires_at IS NULL OR expires_at > now()).
--   We do not add a partial unique index gated on expires_at > now() because
--   now() is not immutable and PostgreSQL rejects it in index predicates.
-- ---------------------------------------------------------------------------

ALTER TABLE memberships
    ADD COLUMN expires_at TIMESTAMPTZ;

ALTER TABLE memberships
    ADD CONSTRAINT memberships_expires_after_granted_chk
    CHECK (expires_at IS NULL OR expires_at > granted_at);

-- Active-membership check for users: expiry-aware hot path for access resolver.
CREATE INDEX memberships_user_expiry_idx
    ON memberships (company_id, user_id, expires_at)
    WHERE user_id IS NOT NULL;

-- Active-membership check for groups.
CREATE INDEX memberships_group_expiry_idx
    ON memberships (company_id, group_id, expires_at)
    WHERE group_id IS NOT NULL;

-- ---------------------------------------------------------------------------
-- F8: Rebuild audit_log action index with company_id prefix
--
-- The previous index (action, created_at DESC) caused cross-tenant scans for
-- tenant-scoped audit queries like "all logins in company X this week".
-- Company_id must be the leading column so the planner can seek directly.
-- ---------------------------------------------------------------------------

DROP INDEX IF EXISTS audit_log_action_idx;

CREATE INDEX audit_log_company_action_idx
    ON audit_log (company_id, action, created_at DESC);

-- ---------------------------------------------------------------------------
-- F9: JSONB array-shape guards on chat_turns
--
-- rag_sources is the audit trail of LLM evidence; silent type regressions
-- (object instead of array, scalar, etc.) corrupt citation reproducibility.
-- Both columns are NOT NULL DEFAULT '[]' (V005), so no IS NULL branch needed.
-- The constraint is intentionally lightweight (typeof check only) so it
-- does not prevent valid array shapes from evolving over time.
-- ---------------------------------------------------------------------------

ALTER TABLE chat_turns
    ADD CONSTRAINT chat_turns_rag_sources_array_chk
    CHECK (jsonb_typeof(rag_sources) = 'array');

ALTER TABLE chat_turns
    ADD CONSTRAINT chat_turns_tool_calls_array_chk
    CHECK (jsonb_typeof(tool_calls) = 'array');
