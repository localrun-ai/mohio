-- V002: Users, groups, memberships, resource grants, and API keys
--
-- Identity lives in the external SSO/OIDC provider (Keycloak, Authentik, etc.).
-- Users and groups are thin local records that map SSO claims to internal principals.
--
-- Authorization model:
--   - memberships: principal (user or group) -> org_unit, with role + inheritance rule
--   - resource_grants: explicit permission on a specific resource for any principal
--
-- Default access is CLOSED: no membership = no access.
-- Inheritance is explicit: applies_to = 'self_and_descendants' must be stated.

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------

CREATE TABLE users (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id   UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    external_sub TEXT        NOT NULL,           -- SSO subject claim (sub)
    email        TEXT        NOT NULL,
    display_name TEXT,
    avatar_url   TEXT,
    is_admin     BOOLEAN     NOT NULL DEFAULT false,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen    TIMESTAMPTZ,
    UNIQUE (company_id, external_sub),
    UNIQUE (company_id, email)
);

CREATE INDEX users_company_id_idx ON users (company_id);

-- ---------------------------------------------------------------------------
-- Groups (mapped from external IdP groups, e.g. Keycloak group IDs)
-- ---------------------------------------------------------------------------

CREATE TABLE groups (
    id                UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id        UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    external_provider TEXT,                      -- 'keycloak', 'google', 'azure_ad', etc.
    external_group_id TEXT,                      -- provider's group ID
    name              TEXT        NOT NULL,
    description       TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, name)
);

CREATE UNIQUE INDEX groups_external_uidx
    ON groups (company_id, external_provider, external_group_id)
    WHERE external_group_id IS NOT NULL;

CREATE INDEX groups_company_id_idx ON groups (company_id);

CREATE TABLE group_members (
    group_id   UUID NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
    user_id    UUID NOT NULL REFERENCES users(id)  ON DELETE CASCADE,
    added_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (group_id, user_id)
);

CREATE INDEX group_members_user_idx ON group_members (user_id);

-- ---------------------------------------------------------------------------
-- Memberships: principal (user or group) -> org_unit, with role
--
-- applies_to controls inheritance:
--   self_only              - this org_unit only
--   self_and_descendants   - this org_unit + all its children (via closure table)
--
-- Role:
--   viewer  - read documents and wiki, run RAG queries
--   editor  - viewer + ingest documents, edit wiki pages
--   admin   - editor + manage members and sub-unit structure
-- ---------------------------------------------------------------------------

CREATE TABLE memberships (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id      UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    principal_type  TEXT        NOT NULL CHECK (principal_type IN ('user', 'group')),
    principal_id    UUID        NOT NULL,
    org_unit_id     UUID        NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    role            TEXT        NOT NULL CHECK (role IN ('viewer', 'editor', 'admin')),
    applies_to      TEXT        NOT NULL DEFAULT 'self_and_descendants'
                                CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by      UUID        REFERENCES users(id),
    granted_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, principal_type, principal_id, org_unit_id)
);

CREATE INDEX memberships_org_unit_idx  ON memberships (org_unit_id);
CREATE INDEX memberships_principal_idx ON memberships (principal_type, principal_id);
CREATE INDEX memberships_company_idx   ON memberships (company_id);

-- ---------------------------------------------------------------------------
-- Resource grants: explicit permission on a specific resource
--
-- Used for cross-unit sharing, document-level overrides, and wiki page ACLs.
-- Examples:
--   Legal can read one specific HR document   -> resource_type='document'
--   RnD team can read all HR org_unit docs    -> resource_type='org_unit', applies_to='self_and_descendants'
-- ---------------------------------------------------------------------------

CREATE TABLE resource_grants (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id      UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    resource_type   TEXT        NOT NULL
                                CHECK (resource_type IN ('org_unit', 'document', 'wiki_page')),
    resource_id     UUID        NOT NULL,
    principal_type  TEXT        NOT NULL CHECK (principal_type IN ('user', 'group', 'org_unit')),
    principal_id    UUID        NOT NULL,
    permission      TEXT        NOT NULL CHECK (permission IN ('read', 'write', 'admin')),
    applies_to      TEXT        NOT NULL DEFAULT 'self_only'
                                CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by      UUID        REFERENCES users(id),
    granted_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at      TIMESTAMPTZ,
    UNIQUE (resource_type, resource_id, principal_type, principal_id)
);

CREATE INDEX resource_grants_resource_idx  ON resource_grants (resource_type, resource_id);
CREATE INDEX resource_grants_principal_idx ON resource_grants (principal_type, principal_id);
CREATE INDEX resource_grants_company_idx   ON resource_grants (company_id);

-- ---------------------------------------------------------------------------
-- API keys (service accounts: CI bots, Slack bots, etc.)
--
-- key_hash: SHA-256 of the raw key (hex). Never store the raw key.
-- key_prefix: first 8 chars shown in UI for identification.
-- is_admin: allows admin-level operations (e.g. for provisioning bots).
-- revoked_at: set when a key is revoked; null means active.
-- ---------------------------------------------------------------------------

CREATE TABLE api_keys (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    user_id     UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name        TEXT        NOT NULL,
    key_prefix  TEXT        NOT NULL,            -- first 8 chars shown in UI
    key_hash    TEXT        NOT NULL UNIQUE,      -- SHA-256 hex of full key
    role        TEXT        NOT NULL DEFAULT 'viewer'
                            CHECK (role IN ('viewer', 'editor', 'admin')),
    is_admin    BOOLEAN     NOT NULL DEFAULT false,
    last_used   TIMESTAMPTZ,
    expires_at  TIMESTAMPTZ,
    revoked_at  TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX api_keys_key_hash_idx  ON api_keys (key_hash);
CREATE INDEX api_keys_company_idx   ON api_keys (company_id);
CREATE INDEX api_keys_user_idx      ON api_keys (user_id);
