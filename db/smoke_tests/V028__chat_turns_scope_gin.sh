# Per-migration smoke tests for V026 (GIN index on chat_turns.access_scope_ids).

# V028.1: index exists with the right method
HAS=$(sql "SELECT indexdef FROM pg_indexes
           WHERE schemaname='public' AND indexname='chat_turns_access_scope_idx';")
echo "$HAS" | grep -qi "USING gin (access_scope_ids)" \
  && pass "V028.1" "chat_turns_access_scope_idx exists as GIN(access_scope_ids)" \
  || fail "V028.1" "index missing or wrong method: '$HAS'"

# V028.2: array-containment query returns hits + planner accepts it
SES='5e550026-0000-0000-0000-000000000000'
TRN1='7e550026-0000-0000-0000-000000000001'
TRN2='7e550026-0000-0000-0000-000000000002'
OU_OTHER='0a100026-0000-0000-0000-000000000000'

sql "INSERT INTO chat_sessions (id, company_id, org_unit_id, user_id)
     VALUES ('$SES','$CO_ACME','$ACME_ROOT','$U_ALICE');" > /dev/null
sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU_OTHER','$CO_ACME','$ACME_ROOT','team','t26','T26');" > /dev/null

sql "INSERT INTO chat_turns (id, company_id, session_id, question, access_scope_ids)
     VALUES ('$TRN1','$CO_ACME','$SES','q1', ARRAY['$ACME_ROOT'::uuid, '$OU_OTHER'::uuid]);" > /dev/null
sql "INSERT INTO chat_turns (id, company_id, session_id, question, access_scope_ids)
     VALUES ('$TRN2','$CO_ACME','$SES','q2', ARRAY['$ACME_ROOT'::uuid]);" > /dev/null

COUNT=$(sql "SELECT COUNT(*) FROM chat_turns
             WHERE access_scope_ids @> ARRAY['$OU_OTHER'::uuid];")
[ "$COUNT" = "1" ] \
  && pass "V028.2" "@> containment returns turns whose scope touched a given org (count=$COUNT)" \
  || fail "V028.2" "expected 1 turn touching OU_OTHER, got $COUNT"

# V028.3: empty array doesn't match a non-empty filter
COUNT=$(sql "SELECT COUNT(*) FROM chat_turns
             WHERE access_scope_ids @> ARRAY['dead0026-0000-0000-0000-000000000000'::uuid];")
[ "$COUNT" = "0" ] \
  && pass "V028.3" "non-matching containment returns 0 turns" \
  || fail "V028.3" "expected 0 for non-matching scope, got $COUNT"
