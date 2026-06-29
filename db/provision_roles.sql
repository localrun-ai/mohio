\set ON_ERROR_STOP on

-- db/provision_roles.sql - idempotent partition-maintenance role provisioning.
--
-- Run ONCE as a superuser or a role with CREATEROLE before applying migrations.
-- Migrations themselves require only standard schema DDL; they do not need
-- CREATEROLE.  This separation keeps the migration account narrow.
--
-- Usage:
--   psql -h HOST -U superuser -d DATABASE -f db/provision_roles.sql
--
-- This script intentionally creates no LOGIN and contains no credentials.
-- The DBA creates a login separately and grants it membership in
-- wikore_partition_maintainer. CI does the same with a test-only login.

DO $$
BEGIN
    CREATE ROLE wikore_partition_maintainer NOLOGIN;
EXCEPTION WHEN duplicate_object THEN NULL;
END
$$;

-- Converge an existing role to the intended non-login, non-privileged shape.
ALTER ROLE wikore_partition_maintainer
    NOLOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;

-- The login inheriting this role must be able to reach this database even when
-- deployments revoke the default PUBLIC CONNECT privilege.
DO $$
BEGIN
    EXECUTE format(
        'GRANT CONNECT ON DATABASE %I TO wikore_partition_maintainer',
        current_database());
END
$$;
