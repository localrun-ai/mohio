-- V013: reactivate_user(company_id, external_issuer, external_sub) function
--
-- Addresses Opus V2-N3 (RECOMMENDED).
--
-- Problem:
--   V011 added users.deactivated_at for soft deactivation. The existing
--   UNIQUE(company_id, external_issuer, external_sub) constraint (V001)
--   intentionally stays in place so a deactivated identity cannot be
--   silently recycled as a new user row.
--
--   Side effect: when a deactivated user signs back in (rehire, reinstate),
--   a plain INSERT into users fails with unique violation. Callers need a
--   single primitive to "find the deactivated row and revive it" without
--   each service inventing its own UPDATE-or-INSERT pattern (which is
--   easy to get wrong: race conditions, partial PII reset, missing
--   audit trail).
--
-- Function behavior:
--   - Lookup the row by (company_id, external_issuer, external_sub).
--   - If the row exists and IS deactivated:
--       Refresh PII fields from the IdP claims (email, display_name, avatar_url),
--       clear deactivated_at, and return the user_id.
--   - If the row exists and is NOT deactivated:
--       Refresh PII fields only (normal sign-in path) and return the user_id.
--   - If the row does not exist:
--       Insert a new user row and return the new id.
--
--   This is the canonical "upsert from IdP claims" primitive. All sign-in
--   code paths (OAuth/SAML/OIDC callbacks) SHOULD call this function rather
--   than open-coding the lookup/insert/update logic.
--
-- Locking:
--   The PERFORM ... FOR UPDATE on the candidate row serializes concurrent
--   sign-ins for the same identity (e.g. two browser tabs racing through
--   the OAuth callback). The unique constraint plus FOR UPDATE means at
--   most one transaction reactivates; the other sees the already-revived
--   row on its second probe.
--
-- Out of scope:
--   - Group/membership restoration: deliberately NOT done here. Reactivation
--     is identity-level only; access has to be re-granted explicitly by an
--     admin so the audit trail records who reinstated which permissions.
--   - GDPR-erased users (email = NULL, external_sub = NULL): cannot be
--     matched by this function (external_sub is NULL on the existing row).
--     That is intentional - an erased identity has no path back.

CREATE OR REPLACE FUNCTION reactivate_user(
    p_company_id       UUID,
    p_external_issuer  TEXT,
    p_external_sub     TEXT,
    p_email            TEXT,
    p_display_name     TEXT,
    p_avatar_url       TEXT
) RETURNS UUID
LANGUAGE plpgsql
AS $$
DECLARE
    v_user_id        UUID;
    v_deactivated_at TIMESTAMPTZ;
BEGIN
    -- Lock the candidate row if it exists. The unique index on
    -- (company_id, external_issuer, external_sub) means at most one row matches.
    SELECT id, deactivated_at
      INTO v_user_id, v_deactivated_at
      FROM users
     WHERE company_id       = p_company_id
       AND external_issuer  = p_external_issuer
       AND external_sub     = p_external_sub
     FOR UPDATE;

    IF FOUND THEN
        UPDATE users
           SET email          = p_email,
               display_name   = p_display_name,
               avatar_url     = p_avatar_url,
               deactivated_at = NULL,
               last_seen      = clock_timestamp()
         WHERE id = v_user_id;

        RETURN v_user_id;
    END IF;

    -- No existing row: create a new user.
    INSERT INTO users (
        company_id,
        external_issuer,
        external_sub,
        email,
        display_name,
        avatar_url,
        last_seen
    ) VALUES (
        p_company_id,
        p_external_issuer,
        p_external_sub,
        p_email,
        p_display_name,
        p_avatar_url,
        clock_timestamp()
    )
    RETURNING id INTO v_user_id;

    RETURN v_user_id;
END;
$$;

COMMENT ON FUNCTION reactivate_user(UUID, TEXT, TEXT, TEXT, TEXT, TEXT) IS
    'Canonical sign-in upsert: revives a soft-deactivated user, refreshes IdP-claim '
    'PII fields, or inserts a new user row. Does NOT restore group memberships or '
    'resource_grants - reactivation is identity-level only and access must be '
    're-granted explicitly by an admin. See V013 header for full contract.';
