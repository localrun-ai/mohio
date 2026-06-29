-- V031: SECURITY DEFINER helpers for runtime partition maintenance.
--
-- PartitionMaintainer (wikore-scheduler) pre-creates quarterly audit_log and
-- monthly usage_events partitions. Its dedicated runtime role has EXECUTE only;
-- these SECURITY DEFINER functions perform the narrowly scoped catalog access
-- and DDL under the migration owner's privilege.
--
-- Security properties:
--   1. Each function only touches its named parent table; no generic parent arg.
--   2. Partition name and date bounds are DERIVED from typed (INT) inputs so
--      there is no path to arbitrary DDL names or ranges.
--   3. Bounds use make_timestamptz(...,'UTC') + SET timezone='UTC' on the
--      function so both creation and existence checks are timezone-independent.
--   4. Existence check uses pg_inherits to verify the table is actually an
--      attached partition of the expected parent, not just any same-named table.
--      If the attached partition's bounds do not match the expected period the
--      function raises immediately rather than silently returning FALSE.
--   5. REVOKE EXECUTE FROM PUBLIC appears immediately after each CREATE so the
--      migration-owner privilege is closed before the selective GRANT below.
--
-- Deployment order: wikore_partition_maintainer must be created before this
-- migration runs.
-- Run db/provision_roles.sql as superuser before applying migrations.

-- Required to resolve the functions when PUBLIC schema access is hardened.
GRANT USAGE ON SCHEMA public TO wikore_partition_maintainer;

-- ---------------------------------------------------------------------------
-- wikore_ensure_audit_log_partition(year_val INT, quarter_val INT)
--   Returns TRUE if a new quarterly partition was created, FALSE if an
--   attached partition for that period already exists with correct bounds.
--   Raises if a relation with the expected name exists but is not an attached
--   partition, or if an attached partition has mismatched bounds.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION wikore_ensure_audit_log_partition(
    year_val    INT,
    quarter_val INT
) RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
SET timezone    = 'UTC'
AS $$
DECLARE
    partition_name  TEXT;
    start_month     INT;
    end_month       INT;
    end_year        INT;
    from_ts         TIMESTAMPTZ;
    to_ts           TIMESTAMPTZ;
    already_exists  BOOLEAN;
    expected_bound  TEXT;
    actual_bound    TEXT;
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

    -- UTC-pinned bounds (SET timezone='UTC' on function ensures ::text renders
    -- identically to pg_get_expr output for the comparison below).
    from_ts := make_timestamptz(year_val, start_month, 1, 0, 0, 0.0, 'UTC');
    to_ts   := make_timestamptz(end_year,  end_month,  1, 0, 0, 0.0, 'UTC');

    -- Verify via pg_inherits that an ATTACHED partition exists.
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

    IF already_exists THEN
        -- Verify the existing partition's bounds match the expected period.
        -- Both pg_get_expr and ::text use the function's SET timezone='UTC',
        -- so the text comparison is stable regardless of caller timezone.
        expected_bound := format(
            'FOR VALUES FROM (''%s'') TO (''%s'')',
            from_ts::text, to_ts::text);

        SELECT pg_get_expr(c.relpartbound, c.oid)
        FROM   pg_class     c
        JOIN   pg_namespace n ON n.oid = c.relnamespace
        WHERE  c.relname = partition_name AND n.nspname = 'public'
        INTO   actual_bound;

        IF actual_bound IS DISTINCT FROM expected_bound THEN
            RAISE EXCEPTION
                'wikore_ensure_audit_log_partition: % is attached but has '
                'wrong bounds (expected %, got %)',
                partition_name, expected_bound, actual_bound;
        END IF;

        RETURN FALSE;
    END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.audit_log '
        'FOR VALUES FROM (%L) TO (%L)',
        partition_name, from_ts, to_ts
    );
    RETURN TRUE;
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_ensure_audit_log_partition(INT, INT) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION wikore_ensure_audit_log_partition(INT, INT)
    TO wikore_partition_maintainer;

-- ---------------------------------------------------------------------------
-- wikore_ensure_usage_events_partition(year_val INT, month_val INT)
--   Returns TRUE if a new monthly partition was created, FALSE if an
--   attached partition for that period already exists with correct bounds.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION wikore_ensure_usage_events_partition(
    year_val  INT,
    month_val INT
) RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
SET timezone    = 'UTC'
AS $$
DECLARE
    partition_name  TEXT;
    next_month      INT;
    next_year       INT;
    from_ts         TIMESTAMPTZ;
    to_ts           TIMESTAMPTZ;
    already_exists  BOOLEAN;
    expected_bound  TEXT;
    actual_bound    TEXT;
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

    IF already_exists THEN
        expected_bound := format(
            'FOR VALUES FROM (''%s'') TO (''%s'')',
            from_ts::text, to_ts::text);

        SELECT pg_get_expr(c.relpartbound, c.oid)
        FROM   pg_class     c
        JOIN   pg_namespace n ON n.oid = c.relnamespace
        WHERE  c.relname = partition_name AND n.nspname = 'public'
        INTO   actual_bound;

        IF actual_bound IS DISTINCT FROM expected_bound THEN
            RAISE EXCEPTION
                'wikore_ensure_usage_events_partition: % is attached but has '
                'wrong bounds (expected %, got %)',
                partition_name, expected_bound, actual_bound;
        END IF;

        RETURN FALSE;
    END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.usage_events '
        'FOR VALUES FROM (%L) TO (%L)',
        partition_name, from_ts, to_ts
    );
    RETURN TRUE;
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_ensure_usage_events_partition(INT, INT) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION wikore_ensure_usage_events_partition(INT, INT)
    TO wikore_partition_maintainer;

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
GRANT  EXECUTE ON FUNCTION wikore_check_partition_overflow()
    TO wikore_partition_maintainer;
