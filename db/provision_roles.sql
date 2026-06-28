-- db/provision_roles.sql - idempotent runtime role provisioning.
--
-- Run ONCE as a superuser or a role with CREATEROLE before applying migrations.
-- Migrations themselves require only standard schema DDL; they do not need
-- CREATEROLE.  This separation keeps the migration account narrow.
--
-- Usage:
--   psql -h HOST -U superuser -d DATABASE -f db/provision_roles.sql
--
-- Default password 'wikore_app' is used in development and CI.
-- In production, change it before running this script:
--   sed 's/wikore_app/YOUR_SECURE_PASSWORD/' db/provision_roles.sql | psql ...

DO $$
BEGIN
    CREATE ROLE wikore_app NOLOGIN;
EXCEPTION WHEN duplicate_object THEN NULL;
END
$$;

DO $$
BEGIN
    CREATE ROLE wikore_app_login
        NOSUPERUSER INHERIT LOGIN
        PASSWORD 'wikore_app';
EXCEPTION WHEN duplicate_object THEN NULL;
END
$$;

-- GRANT is idempotent: a no-op if wikore_app_login is already a member.
GRANT wikore_app TO wikore_app_login;
