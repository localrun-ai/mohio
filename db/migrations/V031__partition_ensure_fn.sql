-- V031: SECURITY DEFINER helpers for runtime partition maintenance.
--
-- PartitionMaintainer (wikore-scheduler) pre-creates quarterly audit_log and
-- monthly usage_events partitions. The runtime role (wikore_app) holds DML
-- on these tables but not CREATE TABLE; these SECURITY DEFINER functions run
-- under the owner's (migration role's) CREATE privilege so the scheduler can
-- call them without schema-creation rights.
--
-- Security properties:
--   1. Each function only touches its named parent table; no generic parent arg.
--   2. Partition name and date bounds are DERIVED from typed (INT) inputs, not
--      caller-supplied strings, so there is no path to arbitrary DDL.
--   3. REVOKE EXECUTE FROM PUBLIC appears immediately after CREATE OR REPLACE
--      so the migration-owner privilege window is closed before the GRANT below.
--   4. GRANT EXECUTE is conditional: runs only when wikore_app already exists
--      so clean CI installs do not fail. Production provisioning must create the
--      role before running migrations; the DO block grants idempotently.

-- ---------------------------------------------------------------------------
-- wikore_ensure_audit_log_partition(year_val INT, quarter_val INT)
--   Returns TRUE if a new quarterly partition was created, FALSE if it
--   already existed. Quarter must be 1-4; year must be 2024-2099.
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
    from_val       TEXT;
    to_val         TEXT;
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
    from_val := year_val::TEXT || '-' || lpad(start_month::TEXT, 2, '0') || '-01';
    to_val   := end_year::TEXT  || '-' || lpad(end_month::TEXT,  2, '0') || '-01';

    SELECT EXISTS (
        SELECT 1 FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relname = partition_name AND n.nspname = 'public'
    ) INTO already_exists;
    IF already_exists THEN RETURN FALSE; END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.audit_log '
        'FOR VALUES FROM (%L::timestamptz) TO (%L::timestamptz)',
        partition_name, from_val, to_val
    );
    RETURN TRUE;
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_ensure_audit_log_partition(INT, INT) FROM PUBLIC;

-- ---------------------------------------------------------------------------
-- wikore_ensure_usage_events_partition(year_val INT, month_val INT)
--   Returns TRUE if a new monthly partition was created, FALSE if it
--   already existed. Month must be 1-12; year must be 2024-2099.
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
    from_val       TEXT;
    to_val         TEXT;
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
    from_val := year_val::TEXT || '-' || lpad(month_val::TEXT,  2, '0') || '-01';
    to_val   := next_year::TEXT || '-' || lpad(next_month::TEXT, 2, '0') || '-01';

    SELECT EXISTS (
        SELECT 1 FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relname = partition_name AND n.nspname = 'public'
    ) INTO already_exists;
    IF already_exists THEN RETURN FALSE; END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.usage_events '
        'FOR VALUES FROM (%L::timestamptz) TO (%L::timestamptz)',
        partition_name, from_val, to_val
    );
    RETURN TRUE;
END;
$$;

REVOKE EXECUTE ON FUNCTION wikore_ensure_usage_events_partition(INT, INT) FROM PUBLIC;

-- ---------------------------------------------------------------------------
-- wikore_check_partition_overflow()
--   Returns one row per monitored default partition indicating whether any
--   rows are present. The runtime role does not need direct SELECT on the
--   child tables; this function provides a tightly bounded read path.
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

-- ---------------------------------------------------------------------------
-- Grants: conditional on wikore_app existing so clean installs do not fail.
-- Production deployments must CREATE ROLE wikore_app before running Flyway.
-- ---------------------------------------------------------------------------

DO $grants$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'wikore_app') THEN
        GRANT EXECUTE ON FUNCTION wikore_ensure_audit_log_partition(INT, INT)    TO wikore_app;
        GRANT EXECUTE ON FUNCTION wikore_ensure_usage_events_partition(INT, INT) TO wikore_app;
        GRANT EXECUTE ON FUNCTION wikore_check_partition_overflow()              TO wikore_app;
    END IF;
END
$grants$;
