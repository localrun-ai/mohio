-- V002: Users, groups, memberships, resource grants, API keys
--
-- Identity lives in the external SSO/OIDC provider (Keycloak, Authentik, etc.).
-- Users and groups are thin local records that map SSO claims to principals.
--
-- All tables carry company_id and use composite FKs (company_id, other_id) to
-- enforce same-company membership at the database level.
--
-- Polymorphic columns (resource_id in resource_grants) reference different
-- tables depending on a discriminator column (resource_type). PostgreSQL cannot
-- express this as a FK constraint; same-company integrity for those columns is
-- enforced by a validation trigger in V009.
--
-- Access model:
--   memberships     - principal (user or group) -> org_unit, with role and
--                     inheritance rule. Split user_id/group_id columns enforce
--                     existence via composite FK; exactly-one CHECK enforces
--                     that exactly one is set.
--   resource_grants - explicit permission override on a specific resource.
--                     Polymorphic resource/principal; V009 adds the trigger.
--   Default is CLOSED: no membership = no access.

-- ---------------------------------------------------------------------------
-- Actor same-company validation helper
--
-- Called from per-table BEFORE INSERT OR UPDATE triggers to validate that
-- actor fields (granted_by, created_by, updated_by, etc.) belong to the
-- same company as the row being written.
--
-- Composite FKs cannot be used for these columns because ON DELETE SET NULL
-- on a composite FK would null out company_id, violating its NOT NULL
-- constraint. Simple FKs only check existence, not company membership.
-- This function fills that gap without storing duplicate company context.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION validate_actor_same_company(
    p_actor_id   UUID,
    p_company_id UUID,
    p_field_name TEXT
)
RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    IF p_actor_id IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM users WHERE id = p_actor_id AND company_id = p_company_id
    ) THEN
        RAISE EXCEPTION 'actor field "%" user % does not belong to company %',
            p_field_name, p_actor_id, p_company_id;
    END IF;
END;
$$;

-- ---------------------------------------------------------------------------
-- Users
-- ---------------------------------------------------------------------------

CREATE TABLE users (
    id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    -- external_issuer + external_sub together identify the SSO principal.
    -- external_issuer: OIDC issuer URL (e.g. 'https://keycloak.acme.com/realms/wikore')
    --                  or 'local' for password/API-key-only accounts.
    -- external_sub:    OIDC sub claim; unique only within an issuer.
    -- Provisioning must always set external_issuer explicitly for OIDC users.
    external_issuer  TEXT        NOT NULL DEFAULT 'local',
    external_sub     TEXT        NOT NULL,
    email            TEXT        NOT NULL,
    display_name     TEXT,
    avatar_url       TEXT,
    is_admin         BOOLEAN     NOT NULL DEFAULT false,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen        TIMESTAMPTZ,
    -- Sub is unique within an issuer, not globally across issuers.
    UNIQUE (company_id, external_issuer, external_sub),
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
-- user belong to the same company. Included in the PK so rows are strictly
-- scoped to one company even if UUIDs are ever imported or replicated.
-- ---------------------------------------------------------------------------

CREATE TABLE group_members (
    company_id UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    group_id   UUID        NOT NULL,
    user_id    UUID        NOT NULL,
    added_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (company_id, group_id, user_id),
    CONSTRAINT group_members_group_same_company_fk
        FOREIGN KEY (company_id, group_id)
        REFERENCES groups(company_id, id) ON DELETE CASCADE,
    CONSTRAINT group_members_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id)  ON DELETE CASCADE
);

CREATE INDEX group_members_user_idx    ON group_members (company_id, user_id);
CREATE INDEX group_members_group_idx   ON group_members (company_id, group_id);

-- ---------------------------------------------------------------------------
-- Memberships: principal (user or group) -> org_unit with role
--
-- Role:
--   viewer  - read documents and wiki, run RAG queries
--   editor  - viewer + ingest documents, edit wiki pages
--   admin   - editor + manage members and sub-unit structure
--
-- applies_to:
--   self_only            - this org_unit only
--   self_and_descendants - this org_unit + all children (via closure table)
--
-- Split user_id / group_id replaces the former polymorphic principal_type +
-- principal_id columns. Composite FKs enforce existence and same-company
-- membership for both. Exactly one must be non-NULL (enforced by CHECK).
--
-- granted_by: simple FK (not composite) because ON DELETE SET NULL on a
-- composite FK would null out company_id, violating its NOT NULL constraint.
-- ---------------------------------------------------------------------------

CREATE TABLE memberships (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    user_id     UUID,                             -- set if principal is a user
    group_id    UUID,                             -- set if principal is a group
    org_unit_id UUID        NOT NULL,
    role        TEXT        NOT NULL CHECK (role IN ('viewer', 'editor', 'admin')),
    applies_to  TEXT        NOT NULL DEFAULT 'self_and_descendants'
                            CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by  UUID        REFERENCES users(id) ON DELETE SET NULL,
    granted_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Exactly one of user_id / group_id must be set.
    CONSTRAINT memberships_exactly_one_principal
        CHECK (
            (user_id IS NOT NULL AND group_id IS NULL)
            OR
            (user_id IS NULL     AND group_id IS NOT NULL)
        ),

    CONSTRAINT memberships_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE,

    CONSTRAINT memberships_group_same_company_fk
        FOREIGN KEY (company_id, group_id)
        REFERENCES groups(company_id, id) ON DELETE CASCADE,

    CONSTRAINT memberships_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

-- One membership per (user|group, org_unit) per company.
CREATE UNIQUE INDEX memberships_user_org_uidx
    ON memberships (company_id, user_id, org_unit_id)
    WHERE user_id IS NOT NULL;

CREATE UNIQUE INDEX memberships_group_org_uidx
    ON memberships (company_id, group_id, org_unit_id)
    WHERE group_id IS NOT NULL;

CREATE INDEX memberships_org_unit_idx ON memberships (company_id, org_unit_id);
CREATE INDEX memberships_user_idx     ON memberships (company_id, user_id)  WHERE user_id  IS NOT NULL;
CREATE INDEX memberships_group_idx    ON memberships (company_id, group_id) WHERE group_id IS NOT NULL;

CREATE TRIGGER memberships_updated_at
    BEFORE UPDATE ON memberships
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE OR REPLACE FUNCTION validate_memberships_actors()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM validate_actor_same_company(NEW.granted_by, NEW.company_id, 'granted_by');
    RETURN NEW;
END;
$$;

CREATE TRIGGER memberships_validate_actors
    BEFORE INSERT OR UPDATE ON memberships
    FOR EACH ROW EXECUTE FUNCTION validate_memberships_actors();

-- ---------------------------------------------------------------------------
-- Resource grants: explicit permission override on a specific resource
--
-- Used for cross-unit sharing and document/wiki ACL overrides.
-- Example: Legal org_unit gets read access to one specific HR document.
--
-- Both principal_id and resource_id are POLYMORPHIC - the type discriminator
-- columns determine which table they reference. PostgreSQL cannot express
-- conditional FKs. Same-company integrity is enforced by the
-- validate_resource_grant_same_company() trigger added in V009.
--
-- applies_to semantics (two independent axes, both default to self_only):
--
--   resource_applies_to: does the grant cover the specified resource only,
--     or the resource org_unit and all its descendants?
--     'self_and_descendants' is only meaningful when resource_type='org_unit';
--     documents and wiki_pages have no descendants (enforced by CHECK).
--
--   principal_applies_to: does the grant cover the specified principal only,
--     or the principal org_unit and all its descendant org_units' members?
--     'self_and_descendants' is only meaningful when principal_type='org_unit';
--     individual users and groups have no org subtree (enforced by CHECK).
--
-- Example: Legal (org_unit, self_and_descendants) granted read on HR (org_unit,
--   self_only) means all Legal sub-teams can read HR documents, but NOT HR
--   sub-team documents.
-- ---------------------------------------------------------------------------

CREATE TABLE resource_grants (
    id                   UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id           UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    resource_type        TEXT        NOT NULL
                                     CHECK (resource_type IN ('org_unit', 'document', 'wiki_page')),
    resource_id          UUID        NOT NULL,      -- POLYMORPHIC: V009 trigger validates
    principal_type       TEXT        NOT NULL CHECK (principal_type IN ('user', 'group', 'org_unit')),
    principal_id         UUID        NOT NULL,      -- POLYMORPHIC: V009 trigger validates
    permission           TEXT        NOT NULL CHECK (permission IN ('read', 'write', 'admin')),

    -- Resource axis: self_and_descendants only valid for org_unit resources.
    resource_applies_to  TEXT        NOT NULL DEFAULT 'self_only'
                                     CHECK (resource_applies_to IN ('self_only', 'self_and_descendants')),
    -- Principal axis: self_and_descendants only valid for org_unit principals.
    principal_applies_to TEXT        NOT NULL DEFAULT 'self_only'
                                     CHECK (principal_applies_to IN ('self_only', 'self_and_descendants')),

    granted_by           UUID        REFERENCES users(id) ON DELETE SET NULL,
    granted_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at           TIMESTAMPTZ,

    -- Prevent nonsensical combinations: only org_units have descendants.
    CONSTRAINT rg_resource_applies_to_valid
        CHECK (resource_applies_to   = 'self_only' OR resource_type   = 'org_unit'),
    CONSTRAINT rg_principal_applies_to_valid
        CHECK (principal_applies_to  = 'self_only' OR principal_type  = 'org_unit'),

    -- company_id included so uniqueness is always tenant-scoped.
    UNIQUE (company_id, resource_type, resource_id, principal_type, principal_id)
);

CREATE INDEX resource_grants_resource_idx  ON resource_grants (company_id, resource_type, resource_id);
CREATE INDEX resource_grants_principal_idx ON resource_grants (company_id, principal_type, principal_id);
CREATE INDEX resource_grants_company_idx   ON resource_grants (company_id);
-- Access resolver hot path: fetch all active grants for a principal and compute
-- MIN(expires_at) to determine lr:eff TTL. expires_at trailing so the planner
-- can filter or sort by it without a separate scan.
CREATE INDEX resource_grants_principal_active_idx
    ON resource_grants (company_id, principal_type, principal_id, expires_at);
-- Resync worker hot path: find all grants on a resource to recompute access_scope_ids.
CREATE INDEX resource_grants_resource_active_idx
    ON resource_grants (company_id, resource_type, resource_id, expires_at);

CREATE OR REPLACE FUNCTION validate_resource_grants_actors()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM validate_actor_same_company(NEW.granted_by, NEW.company_id, 'granted_by');
    RETURN NEW;
END;
$$;

CREATE TRIGGER resource_grants_validate_actors
    BEFORE INSERT OR UPDATE ON resource_grants
    FOR EACH ROW EXECUTE FUNCTION validate_resource_grants_actors();

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
