-- Users (thin record - identity lives in the external SSO/OIDC provider)
-- id = SSO subject claim (sub)

CREATE TABLE users (
    id           UUID        PRIMARY KEY,
    email        TEXT        NOT NULL UNIQUE,
    display_name TEXT,
    avatar_url   TEXT,
    is_admin     BOOLEAN     NOT NULL DEFAULT false,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen    TIMESTAMPTZ
);

CREATE INDEX users_email_idx ON users (email);

-- Org memberships: user -> org, with role
-- Role inheritance: a reader on a parent org inherits reader on all children.
-- Writer/admin roles do NOT inherit downward - must be granted explicitly.

CREATE TABLE memberships (
    user_id    UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    org_id     UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    role       TEXT        NOT NULL CHECK (role IN ('reader','writer','admin')),
    granted_by UUID        REFERENCES users(id),
    granted_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, org_id)
);

CREATE INDEX memberships_org_id_idx  ON memberships (org_id);
CREATE INDEX memberships_user_id_idx ON memberships (user_id);

-- Cross-org access grants (e.g. RnD can read HR docs if explicitly granted)
CREATE TABLE access_grants (
    grantee_org_id UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    target_org_id  UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    role           TEXT        NOT NULL CHECK (role IN ('reader','writer')),
    granted_by     UUID        REFERENCES users(id),
    granted_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (grantee_org_id, target_org_id),
    CHECK (grantee_org_id <> target_org_id)
);

-- Service/bot API keys (for CI pipelines, Slack bots, etc. - no SSO)
CREATE TABLE api_keys (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    user_id     UUID        REFERENCES users(id),
    name        TEXT        NOT NULL,
    key_prefix  TEXT        NOT NULL,           -- first 8 chars, shown in UI
    key_hash    TEXT        NOT NULL UNIQUE,     -- SHA-256 of full key
    role        TEXT        NOT NULL CHECK (role IN ('reader','writer')),
    last_used   TIMESTAMPTZ,
    expires_at  TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX api_keys_key_hash_idx ON api_keys (key_hash);
CREATE INDEX api_keys_org_id_idx   ON api_keys (org_id);
