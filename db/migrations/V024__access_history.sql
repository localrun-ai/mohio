-- V024: Temporal access policy history (memberships + resource_grants)
--
-- Adds shadow history tables capturing every state transition of
-- memberships and resource_grants, with [valid_from, valid_to) ranges.
-- Lets the system answer "what was Alice's scope on 2026-08-14?" with a
-- single indexed query rather than an audit_log replay.
--
-- Why now: SOC2 / ISO27001 auditors ask exactly this question during
-- the sales cycle for regulated buyers. Without temporal columns, the
-- only path is to replay audit_log rows in order - slow, error-prone,
-- and dependent on every grant/revoke having been audited at write time.
-- Once a year of grant/revoke events accumulates without temporal
-- columns, retrofitting means walking the audit log and best-effort
-- reconstructing the state. The schema needs to exist now so history
-- starts continuous.
--
-- Design:
--   * Two history tables, mechanically symmetric to the live tables
--     they shadow. Same columns + valid_from / valid_to.
--   * AFTER INSERT trigger on the live table: writes a history row
--     with valid_from = NEW row's granted_at (or now() if absent),
--     valid_to = NULL.
--   * AFTER UPDATE trigger: closes the open history row
--     (valid_to = clock_timestamp()), opens a new one for the new state.
--   * AFTER DELETE trigger: closes the open history row.
--   * No FK from history to live row: hard-delete on the live side
--     must not orphan history. The whole point is durable record.
--   * Tracks the "live_row_id" so a single ALIVE membership/grant can
--     be followed across renames or expiry refreshes if the application
--     ever UPDATEs in place.
--
-- Operational cost:
--   * Two extra rows on every membership/grant write (one closing the
--     previous open interval, one opening the new). Write rate for
--     these tables is admin-frequency (per-minute at worst), not
--     per-request, so the trigger cost is invisible in practice.
--
-- Query pattern (time travel):
--   SELECT * FROM memberships_history
--   WHERE company_id = $1
--     AND user_id    = $2
--     AND valid_from <= $when
--     AND (valid_to IS NULL OR valid_to > $when);

-- ===========================================================================
-- memberships_history
-- ===========================================================================

CREATE TABLE memberships_history (
    history_id    BIGSERIAL   PRIMARY KEY,
    -- live_row_id stays stable across the lifetime of a membership row:
    -- inserts, updates, and the final delete all share the same value.
    live_row_id   UUID        NOT NULL,
    company_id    UUID        NOT NULL,
    user_id       UUID,
    group_id      UUID,
    org_unit_id   UUID        NOT NULL,
    role          TEXT        NOT NULL,
    applies_to    TEXT        NOT NULL,
    granted_by    UUID,
    granted_at    TIMESTAMPTZ NOT NULL,
    expires_at    TIMESTAMPTZ,
    -- Temporal interval: [valid_from, valid_to). NULL valid_to = currently open.
    valid_from    TIMESTAMPTZ NOT NULL,
    valid_to      TIMESTAMPTZ,
    -- 'insert' / 'update' / 'delete' - which trigger emitted this row.
    change_kind   TEXT        NOT NULL CHECK (change_kind IN ('insert','update','delete')),

    -- An open interval must be the latest row for a given live_row_id.
    UNIQUE (live_row_id, valid_from)
);

-- Hot-path time-travel queries: scope as-of a given timestamp for a user.
CREATE INDEX memberships_history_user_asof_idx
    ON memberships_history (company_id, user_id, valid_from)
    WHERE user_id IS NOT NULL;

CREATE INDEX memberships_history_group_asof_idx
    ON memberships_history (company_id, group_id, valid_from)
    WHERE group_id IS NOT NULL;

-- At most one open interval per live row.
CREATE UNIQUE INDEX memberships_history_open_uidx
    ON memberships_history (live_row_id)
    WHERE valid_to IS NULL;

-- ===========================================================================
-- resource_grants_history
-- ===========================================================================

CREATE TABLE resource_grants_history (
    history_id           BIGSERIAL   PRIMARY KEY,
    live_row_id          UUID        NOT NULL,
    company_id           UUID        NOT NULL,
    resource_type        TEXT        NOT NULL,
    resource_id          UUID        NOT NULL,
    principal_type       TEXT        NOT NULL,
    principal_id         UUID        NOT NULL,
    permission           TEXT        NOT NULL,
    resource_applies_to  TEXT        NOT NULL,
    principal_applies_to TEXT        NOT NULL,
    granted_by           UUID,
    granted_at           TIMESTAMPTZ NOT NULL,
    expires_at           TIMESTAMPTZ,
    valid_from           TIMESTAMPTZ NOT NULL,
    valid_to             TIMESTAMPTZ,
    change_kind          TEXT        NOT NULL CHECK (change_kind IN ('insert','update','delete')),

    UNIQUE (live_row_id, valid_from)
);

CREATE INDEX resource_grants_history_principal_asof_idx
    ON resource_grants_history (company_id, principal_type, principal_id, valid_from);

CREATE INDEX resource_grants_history_resource_asof_idx
    ON resource_grants_history (company_id, resource_type, resource_id, valid_from);

CREATE UNIQUE INDEX resource_grants_history_open_uidx
    ON resource_grants_history (live_row_id)
    WHERE valid_to IS NULL;

-- ===========================================================================
-- Triggers: write to history on INSERT / UPDATE / DELETE
-- ===========================================================================

CREATE OR REPLACE FUNCTION memberships_write_history()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_now TIMESTAMPTZ := clock_timestamp();
BEGIN
    IF TG_OP = 'INSERT' THEN
        INSERT INTO memberships_history
            (live_row_id, company_id, user_id, group_id, org_unit_id,
             role, applies_to, granted_by, granted_at, expires_at,
             valid_from, valid_to, change_kind)
        VALUES
            (NEW.id, NEW.company_id, NEW.user_id, NEW.group_id, NEW.org_unit_id,
             NEW.role, NEW.applies_to, NEW.granted_by, NEW.granted_at, NEW.expires_at,
             COALESCE(NEW.granted_at, v_now), NULL, 'insert');
        RETURN NEW;
    END IF;

    IF TG_OP = 'UPDATE' THEN
        UPDATE memberships_history
        SET valid_to = v_now
        WHERE live_row_id = OLD.id AND valid_to IS NULL;
        INSERT INTO memberships_history
            (live_row_id, company_id, user_id, group_id, org_unit_id,
             role, applies_to, granted_by, granted_at, expires_at,
             valid_from, valid_to, change_kind)
        VALUES
            (NEW.id, NEW.company_id, NEW.user_id, NEW.group_id, NEW.org_unit_id,
             NEW.role, NEW.applies_to, NEW.granted_by, NEW.granted_at, NEW.expires_at,
             v_now, NULL, 'update');
        RETURN NEW;
    END IF;

    IF TG_OP = 'DELETE' THEN
        UPDATE memberships_history
        SET valid_to = v_now
        WHERE live_row_id = OLD.id AND valid_to IS NULL;
        INSERT INTO memberships_history
            (live_row_id, company_id, user_id, group_id, org_unit_id,
             role, applies_to, granted_by, granted_at, expires_at,
             valid_from, valid_to, change_kind)
        VALUES
            (OLD.id, OLD.company_id, OLD.user_id, OLD.group_id, OLD.org_unit_id,
             OLD.role, OLD.applies_to, OLD.granted_by, OLD.granted_at, OLD.expires_at,
             v_now, v_now, 'delete');
        RETURN OLD;
    END IF;

    RETURN NULL;
END;
$$;

CREATE TRIGGER memberships_history_trigger
    AFTER INSERT OR UPDATE OR DELETE ON memberships
    FOR EACH ROW EXECUTE FUNCTION memberships_write_history();

CREATE OR REPLACE FUNCTION resource_grants_write_history()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_now TIMESTAMPTZ := clock_timestamp();
BEGIN
    IF TG_OP = 'INSERT' THEN
        INSERT INTO resource_grants_history
            (live_row_id, company_id, resource_type, resource_id,
             principal_type, principal_id, permission,
             resource_applies_to, principal_applies_to,
             granted_by, granted_at, expires_at,
             valid_from, valid_to, change_kind)
        VALUES
            (NEW.id, NEW.company_id, NEW.resource_type, NEW.resource_id,
             NEW.principal_type, NEW.principal_id, NEW.permission,
             NEW.resource_applies_to, NEW.principal_applies_to,
             NEW.granted_by, NEW.granted_at, NEW.expires_at,
             COALESCE(NEW.granted_at, v_now), NULL, 'insert');
        RETURN NEW;
    END IF;

    IF TG_OP = 'UPDATE' THEN
        UPDATE resource_grants_history
        SET valid_to = v_now
        WHERE live_row_id = OLD.id AND valid_to IS NULL;
        INSERT INTO resource_grants_history
            (live_row_id, company_id, resource_type, resource_id,
             principal_type, principal_id, permission,
             resource_applies_to, principal_applies_to,
             granted_by, granted_at, expires_at,
             valid_from, valid_to, change_kind)
        VALUES
            (NEW.id, NEW.company_id, NEW.resource_type, NEW.resource_id,
             NEW.principal_type, NEW.principal_id, NEW.permission,
             NEW.resource_applies_to, NEW.principal_applies_to,
             NEW.granted_by, NEW.granted_at, NEW.expires_at,
             v_now, NULL, 'update');
        RETURN NEW;
    END IF;

    IF TG_OP = 'DELETE' THEN
        UPDATE resource_grants_history
        SET valid_to = v_now
        WHERE live_row_id = OLD.id AND valid_to IS NULL;
        INSERT INTO resource_grants_history
            (live_row_id, company_id, resource_type, resource_id,
             principal_type, principal_id, permission,
             resource_applies_to, principal_applies_to,
             granted_by, granted_at, expires_at,
             valid_from, valid_to, change_kind)
        VALUES
            (OLD.id, OLD.company_id, OLD.resource_type, OLD.resource_id,
             OLD.principal_type, OLD.principal_id, OLD.permission,
             OLD.resource_applies_to, OLD.principal_applies_to,
             OLD.granted_by, OLD.granted_at, OLD.expires_at,
             v_now, v_now, 'delete');
        RETURN OLD;
    END IF;

    RETURN NULL;
END;
$$;

CREATE TRIGGER resource_grants_history_trigger
    AFTER INSERT OR UPDATE OR DELETE ON resource_grants
    FOR EACH ROW EXECUTE FUNCTION resource_grants_write_history();
