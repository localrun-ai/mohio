-- V031: SECURITY DEFINER helpers for runtime partition maintenance.
--
-- PartitionMaintainer (wikore-scheduler) pre-creates quarterly audit_log and
-- monthly usage_events partitions. The runtime role (wikore_app) holds DML
-- on these tables but not CREATE TABLE; these SECURITY DEFINER functions run
-- under the owner's (migration role's) CREATE privilege.
--
-- Security properties:
--   1. Each function only touches its named parent table; no generic parent arg.
--   2. Partition name and date bounds are DERIVED from typed (INT) inputs so
--      there is no path to arbitrary DDL names or ranges.
--   3. Bounds are constructed with make_timestamptz(...,'UTC') to prevent
--      session-timezone drift from creating shifted or overlapping partitions.
--   4. Existence check uses pg_inherits to verify the table is actually an
--      attached partition of the expected parent, not just any same-named table.
--   5. REVOKE EXECUTE FROM PUBLIC appears immediately after each CREATE so the
--      migration-owner privilege is closed before the selective GRANT below.
--
-- Deployment order: wikore_app must be created before this migration runs.
-- In CI this is done by the "Provision runtime roles" workflow step.
-- In production, provision the role before applying migrations:
--   CREATE ROLE wikore_app NOLOGIN;
--   CREATE ROLE wikore_app_login NOSUPERUSER INHERIT LOGIN PASSWORD '...';
--   GRANT wikore_app TO wikore_app_login;

-- ---------------------------------------------------------------------------
-- wikore_ensure_audit_log_partition(year_val INT, quarter_val INT)
--   Returns TRUE if a new quarterly partition was created, FALSE if an
--   attached partition for that period already exists.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION wikore_ensure_audit_log_partition(
    year_val    INT,
    quarter_val INT
) RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    partition_name TEXT;
    start_month    INT;
    end_month      INT;
    end_year       INT;
    from_ts        TIMESTAMPTZ;
    to_ts          TIMESTAMPTZ;
    already_exists BOOLEAN;
BEGIN
    IF quarter_val NOT BETWEEN 1 AND 4 THEN
        RAISE EXCEPTION 'wikore_ensure_audit_log_partition: quarter % not in 1-4', quarter_val;
    END IF;
    IF year_val NOT BETWEEN 2024 AND 2099 THEN
        RAISE EXCEPTION 'wikore_ensure_audit_log_partition: year % out of range', year_val;
    END IF;

    partition_name := 'audit_log_' || year_val::TEXT || '_q' || quarter_val::TEXT;
    start_month    := (quarter_val - 1) * 3 + 1;
    end_month      := start_month + 3;
    end_year       := year_val;
    IF end_month > 12 THEN
        end_month := end_month - 12;
        end_year  := end_year  + 1;
    END IF;

    -- Build UTC-pinned timestamptz bounds so session timezone cannot shift them.
    from_ts := make_timestamptz(year_val, start_month, 1, 0, 0, 0.0, 'UTC');
    to_ts   := make_timestamptz(end_year,  end_month,  1, 0, 0, 0.0, 'UTC');

    -- Verify via pg_inherits that an ATTACHED partition for this period exists,
    -- not just any relation that happens to share the name.
    SELECT EXISTS (
        SELECT 1
        FROM   pg_class      c
        JOIN   pg_namespace  n  ON n.oid  = c.relnamespace
        JOIN   pg_inherits   i  ON i.inhrelid = c.oid
        JOIN   pg_class      p  ON p.oid  = i.inhparent
        JOIN   pg_namespace  pn ON pn.oid = p.relnamespace
        WHERE  c.relname  = partition_name AND n.nspname  = 'public'
          AND  p.relname  = 'audit_log'    AND pn.nspname = 'public'
    ) INTO already_exists;
    IF already_exists THEN RETURN FALSE; END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.audit_log '
        'FOR VALUES FROM (%L) TO (%L)',
        partition_name, from_ts, to_ts
    );
    RETURN TRUE;
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_ensure_audit_log_partition(INT, INT) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION wikore_ensure_audit_log_partition(INT, INT) TO   wikore_app;

-- ---------------------------------------------------------------------------
-- wikore_ensure_usage_events_partition(year_val INT, month_val INT)
--   Returns TRUE if a new monthly partition was created, FALSE if an
--   attached partition for that period already exists.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION wikore_ensure_usage_events_partition(
    year_val  INT,
    month_val INT
) RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    partition_name TEXT;
    next_month     INT;
    next_year      INT;
    from_ts        TIMESTAMPTZ;
    to_ts          TIMESTAMPTZ;
    already_exists BOOLEAN;
BEGIN
    IF month_val NOT BETWEEN 1 AND 12 THEN
        RAISE EXCEPTION 'wikore_ensure_usage_events_partition: month % not in 1-12', month_val;
    END IF;
    IF year_val NOT BETWEEN 2024 AND 2099 THEN
        RAISE EXCEPTION 'wikore_ensure_usage_events_partition: year % out of range', year_val;
    END IF;

    partition_name := 'usage_events_' || year_val::TEXT || '_' || lpad(month_val::TEXT, 2, '0');
    next_month := month_val + 1;
    next_year  := year_val;
    IF next_month > 12 THEN
        next_month := 1;
        next_year  := next_year + 1;
    END IF;

    from_ts := make_timestamptz(year_val, month_val,  1, 0, 0, 0.0, 'UTC');
    to_ts   := make_timestamptz(next_year, next_month, 1, 0, 0, 0.0, 'UTC');

    SELECT EXISTS (
        SELECT 1
        FROM   pg_class      c
        JOIN   pg_namespace  n  ON n.oid  = c.relnamespace
        JOIN   pg_inherits   i  ON i.inhrelid = c.oid
        JOIN   pg_class      p  ON p.oid  = i.inhparent
        JOIN   pg_namespace  pn ON pn.oid = p.relnamespace
        WHERE  c.relname  = partition_name   AND n.nspname  = 'public'
          AND  p.relname  = 'usage_events'   AND pn.nspname = 'public'
    ) INTO already_exists;
    IF already_exists THEN RETURN FALSE; END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.usage_events '
        'FOR VALUES FROM (%L) TO (%L)',
        partition_name, from_ts, to_ts
    );
    RETURN TRUE;
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_ensure_usage_events_partition(INT, INT) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION wikore_ensure_usage_events_partition(INT, INT) TO   wikore_app;

-- ---------------------------------------------------------------------------
-- wikore_check_partition_overflow()
--   Returns one row per monitored default partition with has_rows=TRUE when
--   any rows exist. Eliminates the need for direct SELECT on child tables.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION wikore_check_partition_overflow()
RETURNS TABLE(partition_table TEXT, has_rows BOOLEAN)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    RETURN QUERY VALUES
        ('audit_log_default'::TEXT,
         EXISTS(SELECT 1 FROM public.audit_log_default    LIMIT 1)),
        ('usage_events_default'::TEXT,
         EXISTS(SELECT 1 FROM public.usage_events_default LIMIT 1));
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_check_partition_overflow() FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION wikore_check_partition_overflow() TO   wikore_app;
