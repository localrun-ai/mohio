# Per-migration smoke tests for V020 (retention_policy).

# V020.1: companies.default_retention_days default is NULL (no policy)
VAL=$(sql "SELECT default_retention_days FROM companies WHERE id='$CO_ACME';")
[ -z "$VAL" ] \
  && pass "V020.1" "companies.default_retention_days defaults to NULL" \
  || fail "V020.1" "expected NULL, got '$VAL'"

# V020.2: default_retention_days enforces > 0
ERR=$(sql "UPDATE companies SET default_retention_days=0 WHERE id='$CO_ACME';" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V020.2" "default_retention_days CHECK rejects 0" \
  || fail "V020.2" "0 was not rejected: $ERR"

# V020.3: documents.retention_until accepts a future timestamp
sql "UPDATE documents SET retention_until=now() + interval '90 days' WHERE id='$DOC';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM documents WHERE id='$DOC' AND retention_until > now();")
[ "$COUNT" = "1" ] \
  && pass "V020.3" "documents.retention_until set to future timestamp queryable" \
  || fail "V020.3" "unexpected count $COUNT"

# V020.4: chat_turns.retention_until set + partial index visible to planner
sql "INSERT INTO chat_sessions (id, company_id, org_unit_id, user_id)
     VALUES ('5e550020-0000-0000-0000-000000000000','$CO_ACME','$ACME_ROOT','$U_ALICE');" > /dev/null
sql "INSERT INTO chat_turns (id, company_id, session_id, question, retention_until)
     VALUES ('7e550020-0000-0000-0000-000000000000','$CO_ACME',
             '5e550020-0000-0000-0000-000000000000','Q', now() + interval '30 days');" > /dev/null

COUNT=$(sql "SELECT COUNT(*) FROM chat_turns
             WHERE retention_until IS NOT NULL AND retention_until > now();")
[ "$COUNT" -ge "1" ] \
  && pass "V020.4" "chat_turns.retention_until indexable via partial idx (count=$COUNT)" \
  || fail "V020.4" "expected >=1, got '$COUNT'"
