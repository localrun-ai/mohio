# Per-migration smoke tests for V023 (shared_chat_threads / K8).

SES='5e550023-0000-0000-0000-000000000000'
SHR='5ea50023-0000-0000-0000-000000000000'

sql "INSERT INTO chat_sessions (id, company_id, org_unit_id, user_id)
     VALUES ('$SES','$CO_ACME','$ACME_ROOT','$U_ALICE');" > /dev/null

# V023.1: share insert with non-empty access_scope_snapshot
sql "INSERT INTO shared_chat_threads
       (id, company_id, chat_session_id, shared_by_user_id, access_scope_snapshot, share_token)
     VALUES ('$SHR','$CO_ACME','$SES','$U_ALICE',
             ARRAY['$ACME_ROOT'::uuid], 'tok-V023-1');" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM shared_chat_threads
             WHERE share_token='tok-V023-1' AND revoked_at IS NULL;")
[ "$COUNT" = "1" ] \
  && pass "V023.1" "shared_chat_threads insert + active filter (count=$COUNT)" \
  || fail "V023.1" "expected 1, got $COUNT"

# V023.2: snapshot is immutable
ERR=$(sql "UPDATE shared_chat_threads
           SET access_scope_snapshot = ARRAY['dead0023-0000-0000-0000-000000000000'::uuid]
           WHERE id='$SHR';" 2>&1 || true)
echo "$ERR" | grep -qi "immutable" \
  && pass "V023.2" "access_scope_snapshot is immutable (snapshot frozen at share time)" \
  || fail "V023.2" "snapshot mutation not blocked (err='$ERR')"

# V023.3: revoke_at and revoked_by_user_id move together
ERR=$(sql "UPDATE shared_chat_threads SET revoked_at=now() WHERE id='$SHR';" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V023.3" "revoke without revoked_by_user_id rejected by CHECK" \
  || fail "V023.3" "asymmetric revoke not blocked (err='$ERR')"

# V023.4: cross-company chat_session_id rejected by composite FK
SES_BETA='5e550023-bbbb-0000-0000-000000000000'
sql "INSERT INTO chat_sessions (id, company_id, org_unit_id, user_id)
     VALUES ('$SES_BETA','$CO_BETA','$BETA_ROOT','$U_BOB');" > /dev/null
ERR=$(sql "INSERT INTO shared_chat_threads
             (company_id, chat_session_id, shared_by_user_id, access_scope_snapshot, share_token)
           VALUES ('$CO_ACME','$SES_BETA','$U_ALICE',
                   ARRAY['$ACME_ROOT'::uuid],'tok-V023-bad');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass "V023.4" "cross-company chat_session_id rejected by composite FK" \
  || fail "V023.4" "cross-company share not rejected (err='$ERR')"

# V023.5: clean revoke (both fields together) succeeds
sql "UPDATE shared_chat_threads
     SET revoked_at = now(), revoked_by_user_id = '$U_ALICE'
     WHERE id='$SHR';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM shared_chat_threads
             WHERE id='$SHR' AND revoked_at IS NOT NULL;")
[ "$COUNT" = "1" ] \
  && pass "V023.5" "clean revoke (both fields together) accepted" \
  || fail "V023.5" "revoke didn't apply (count=$COUNT)"
