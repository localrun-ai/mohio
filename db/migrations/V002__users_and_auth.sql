-- V002: Users, groups, memberships, resource grants, API keys
--
-- Identity lives in the external SSO/OIDC provider (Keycloak, Authentik, etc.).
-- Users and groups are thin local records that map SSO claims to principals.
--
-- All tables carry company_id and use composite FKs (company_id, other_id) to
-- enforce same-company membership at the database level.
--
-- Polymorphic columns (principal_id, resource_id) reference different tables
-- depending on a discriminator column (principal_type, resource_type).
-- PostgreSQL cannot express this as a FK constraint; same-company integrity
-- for these columns is enforced by the application layer.  They are clearly
-- marked below.
--
-- Access model:
--   memberships    - principal -> org_unit, with role and inheritance rule
--   resource_grants - explicit permission on a specific resource
--   Default is CLOSED: no membership = no access.

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------

CREATE TABLE users (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id    UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    external_sub  TEXT        NOT NULL,           -- SSO subject (sub claim)
    email         TEXT        NOT NULL,
    display_name  TEXT,
    avatar_url    TEXT,
    is_admin      BOOLEAN     NOT NULL DEFAULT false,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen     TIMESTAMPTZ,
    UNIQUE (company_id, external_sub),
    UNIQUE (company_id, email),
    -- Required target for composite FKs from other tables.
    UNIQUE (company_id, id)
);

CREATE INDEX users_company_id_idx ON users (company_id);

-- ---------------------------------------------------------------------------
-- Groups (IdP group mirror, e.g. Keycloak groups)
--
-- external_provider and external_group_id must both be set or both NULL.
-- ---------------------------------------------------------------------------

CREATE TABLE groups (
    id                UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id        UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    external_provider TEXT,                       -- 'keycloak', 'google', 'azure_ad', ...
    external_group_id TEXT,                       -- provider-assigned group ID
    name              TEXT        NOT NULL,
    description       TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, name),
    -- Required target for composite FKs from other tables.
    UNIQUE (company_id, id),
    -- Both external fields must be present or absent together.
    CONSTRAINT groups_external_both_or_neither
        CHECK ((external_provider IS NULL) = (external_group_id IS NULL))
);

CREATE UNIQUE INDEX groups_external_uidx
    ON groups (company_id, external_provider, external_group_id)
    WHERE external_group_id IS NOT NULL;

CREATE INDEX groups_company_id_idx ON groups (company_id);

-- ---------------------------------------------------------------------------
-- Group members
--
-- company_id is carried here so composite FKs can enforce that group and
-- user belong to the same company.
-- ---------------------------------------------------------------------------

CREATE TABLE group_members (
    company_id UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    group_id   UUID        NOT NULL,
    user_id    UUID        NOT NULL,
    added_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (group_id, user_id),
    CONSTRAINT group_members_group_same_company_fk
        FOREIGN KEY (company_id, group_id)
        REFERENCES groups(company_id, id) ON DELETE CASCADE,
    CONSTRAINT group_members_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id)  ON DELETE CASCADE
);

CREATE INDEX group_members_user_idx    ON group_members (user_id);
CREATE INDEX group_members_company_idx ON group_members (company_id);

-- ---------------------------------------------------------------------------
-- Memberships: principal -> org_unit with role and inheritance scope
--
-- Role:
--   viewer  - read documents and wiki, run RAG queries
--   editor  - viewer + ingest documents, edit wiki pages
--   admin   - editor + manage members and sub-unit structure
--
-- applies_to:
--   self_only              - this org_unit only
--   self_and_descendants   - this org_unit + all children (via closure table)
--
-- principal_id is POLYMORPHIC (user UUID or group UUID depending on
-- principal_type). Same-company integrity is enforced by the application.
--
-- Composite FK on org_unit_id enforces that org_unit belongs to the same company.
-- ---------------------------------------------------------------------------

CREATE TABLE memberships (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id     UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    principal_type TEXT        NOT NULL CHECK (principal_type IN ('user', 'group')),
    principal_id   UUID        NOT NULL,          -- POLYMORPHIC: app enforces same-company
    org_unit_id    UUID        NOT NULL,
    role           TEXT        NOT NULL CHECK (role IN ('viewer', 'editor', 'admin')),
    applies_to     TEXT        NOT NULL DEFAULT 'self_and_descendants'
                               CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by     UUID        REFERENCES users(id) ON DELETE SET NULL,
    granted_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, principal_type, principal_id, org_unit_id),
    CONSTRAINT memberships_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE INDEX memberships_org_unit_idx  ON memberships (org_unit_id);
CREATE INDEX memberships_principal_idx ON memberships (principal_type, principal_id);
CREATE INDEX memberships_company_idx   ON memberships (company_id);

CREATE TRIGGER memberships_updated_at
    BEFORE UPDATE ON memberships
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Resource grants: explicit permission override on a specific resource
--
-- Used for cross-unit sharing and document/wiki ACL overrides.
-- Example: Legal (org_unit) gets read access to one specific HR document.
--
-- Both principal_id and resource_id are POLYMORPHIC.
-- Same-company integrity is enforced by the application layer.
-- ---------------------------------------------------------------------------

CREATE TABLE resource_grants (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id     UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    resource_type  TEXT        NOT NULL
                               CHECK (resource_type IN ('org_unit', 'document', 'wiki_page')),
    resource_id    UUID        NOT NULL,           -- POLYMORPHIC: app enforces same-company
    principal_type TEXT        NOT NULL CHECK (principal_type IN ('user', 'group', 'org_unit')),
    principal_id   UUID        NOT NULL,           -- POLYMORPHIC: app enforces same-company
    permission     TEXT        NOT NULL CHECK (permission IN ('read', 'write', 'admin')),
    applies_to     TEXT        NOT NULL DEFAULT 'self_only'
                               CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by     UUID        REFERENCES users(id) ON DELETE SET NULL,
    granted_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ,
    UNIQUE (resource_type, resource_id, principal_type, principal_id)
);

CREATE INDEX resource_grants_resource_idx  ON resource_grants (resource_type, resource_id);
CREATE INDEX resource_grants_principal_idx ON resource_grants (principal_type, principal_id);
CREATE INDEX resource_grants_company_idx   ON resource_grants (company_id);

-- ---------------------------------------------------------------------------
-- API keys (service accounts: CI bots, ingestion scripts, Slack bots, etc.)
--
-- key_hash: SHA-256 hex of the raw key. Never store the plaintext.
-- key_prefix: first 8 chars of the raw key, shown in UI for identification.
-- role: permission CEILING for the key, regardless of the user's memberships.
--   A key with role='viewer' cannot perform write operations even if the
--   underlying user is an editor or admin.
-- is_admin: grants system-level admin operations (company management, etc.)
--   Separate from role because role is scoped to org-unit operations.
-- revoked_at: non-null means the key is disabled.
-- ---------------------------------------------------------------------------

CREATE TABLE api_keys (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL,
    user_id     UUID        NOT NULL,
    name        TEXT        NOT NULL,
    key_prefix  TEXT        NOT NULL,
    key_hash    TEXT        NOT NULL UNIQUE,
    role        TEXT        NOT NULL DEFAULT 'viewer'
                            CHECK (role IN ('viewer', 'editor', 'admin')),
    is_admin    BOOLEAN     NOT NULL DEFAULT false,
    last_used   TIMESTAMPTZ,
    expires_at  TIMESTAMPTZ,
    revoked_at  TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT api_keys_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE
);

CREATE INDEX api_keys_key_hash_idx ON api_keys (key_hash);
CREATE INDEX api_keys_company_idx  ON api_keys (company_id);
CREATE INDEX api_keys_user_idx     ON api_keys (user_id);
