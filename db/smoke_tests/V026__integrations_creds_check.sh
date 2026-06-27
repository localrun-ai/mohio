# Per-migration smoke tests for V026 (integrations credentials/key_id consistency).

INT_OK_NN='11700026-0000-0000-0000-000000000000'   # both NULL
INT_OK_BOTH='11700026-0000-0000-0000-000000000001' # both set
INT_BAD1='11700026-0000-0000-0000-00000000bad1'    # creds set, key_id NULL
INT_BAD2='11700026-0000-0000-0000-00000000bad2'    # creds NULL, key_id set

# V026.1: both NULL accepted (integration exists, credentials not yet configured)
sql "INSERT INTO integrations
       (id, company_id, org_unit_id, type, name, credentials, credentials_key_id)
     VALUES ('$INT_OK_NN','$CO_ACME','$ACME_ROOT','slack','slack-empty', NULL, NULL);" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM integrations WHERE id='$INT_OK_NN';")
[ "$COUNT" = "1" ] \
  && pass "V026.1" "both NULL accepted (no credentials yet)" \
  || fail "V026.1" "both-NULL rejected (count=$COUNT)"

# V026.2: both set accepted (credentials encrypted with a known key)
sql "INSERT INTO integrations
       (id, company_id, org_unit_id, type, name, credentials, credentials_key_id)
     VALUES ('$INT_OK_BOTH','$CO_ACME','$ACME_ROOT','slack','slack-ok',
             'enc_blob_b64','key-v1');" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM integrations WHERE id='$INT_OK_BOTH';")
[ "$COUNT" = "1" ] \
  && pass "V026.2" "both-set accepted (credentials+key_id together)" \
  || fail "V026.2" "both-set rejected (count=$COUNT)"

# V026.3: creds set + key_id NULL rejected (orphan ciphertext)
ERR=$(sql "INSERT INTO integrations
             (id, company_id, org_unit_id, type, name, credentials, credentials_key_id)
           VALUES ('$INT_BAD1','$CO_ACME','$ACME_ROOT','slack','slack-bad1',
                   'orphan_cipher', NULL);" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V026.3" "creds set + key_id NULL rejected (orphan ciphertext blocked)" \
  || fail "V026.3" "orphan ciphertext not rejected: $ERR"

# V026.4: creds NULL + key_id set rejected (dangling key reference)
ERR=$(sql "INSERT INTO integrations
             (id, company_id, org_unit_id, type, name, credentials, credentials_key_id)
           VALUES ('$INT_BAD2','$CO_ACME','$ACME_ROOT','slack','slack-bad2',
                   NULL, 'key-v1');" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V026.4" "creds NULL + key_id set rejected (dangling key reference)" \
  || fail "V026.4" "dangling key reference not rejected: $ERR"

# V026.5: UPDATE path also enforced (clearing only one side rejected)
ERR=$(sql "UPDATE integrations SET credentials = NULL WHERE id='$INT_OK_BOTH';" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V026.5" "UPDATE that clears only one side rejected" \
  || fail "V026.5" "asymmetric UPDATE not rejected: $ERR"
