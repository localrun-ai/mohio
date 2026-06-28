-- V031: SECURITY DEFINER helper for runtime partition pre-creation.
--
-- PartitionMaintainer (wikore-scheduler) pre-creates quarterly audit_log
-- and monthly usage_events partitions. The runtime role (wikore_app) holds
-- DML privileges on the partitioned tables but not CREATE TABLE + PARTITION
-- rights on the hierarchy. This function runs under the owner's (migration
-- role's) CREATE privilege, keeping the runtime role's sandbox intact and
-- the append-only controls in V007/V016 unweakened.
--
-- Returns TRUE if a new partition was attached, FALSE if it already existed.
-- from_val and to_val are ISO date strings ('YYYY-MM-DD').
--
-- The function is intentionally narrow: it only accepts known parent tables
-- and only creates range partitions with timestamptz bounds, so it cannot be
-- used to CREATE arbitrary tables under the migration owner's identity.

CREATE OR REPLACE FUNCTION wikore_ensure_partition(
    parent_table   TEXT,
    partition_name TEXT,
    from_val       TEXT,
    to_val         TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    already_exists BOOLEAN;
BEGIN
    -- Guard: only allow known parent tables to limit blast radius.
    IF parent_table NOT IN ('audit_log', 'usage_events') THEN
        RAISE EXCEPTION 'wikore_ensure_partition: unknown parent table %', parent_table;
    END IF;

    -- Check whether the partition already exists in the public schema.
    SELECT EXISTS (
        SELECT 1 FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relname = partition_name
          AND n.nspname = 'public'
    ) INTO already_exists;

    IF already_exists THEN
        RETURN FALSE;
    END IF;

    EXECUTE format(
        'CREATE TABLE public.%I PARTITION OF public.%I '
        'FOR VALUES FROM (%L::timestamptz) TO (%L::timestamptz)',
        partition_name, parent_table, from_val, to_val
    );
    RETURN TRUE;
END;
$$;

-- Grant EXECUTE to the application role so the scheduler can call this
-- without needing schema-creation privileges.
GRANT EXECUTE ON FUNCTION wikore_ensure_partition(TEXT, TEXT, TEXT, TEXT)
    TO wikore_app;
