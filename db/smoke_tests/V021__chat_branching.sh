# Per-migration smoke tests for V021 (chat_branching).

SES='5e550021-0000-0000-0000-000000000000'
PAR='7e550021-0000-0000-0000-000000000000'
CH1='7e550021-0000-0000-0000-000000000001'

sql "INSERT INTO chat_sessions (id, company_id, org_unit_id, user_id)
     VALUES ('$SES','$CO_ACME','$ACME_ROOT','$U_ALICE');" > /dev/null

# Top-level turn (no parent)
sql "INSERT INTO chat_turns (id, company_id, session_id, question)
     VALUES ('$PAR','$CO_ACME','$SES','original Q');" > /dev/null

# V021.1: branched turn references parent via self-FK
sql "INSERT INTO chat_turns (id, company_id, session_id, question, parent_turn_id)
     VALUES ('$CH1','$CO_ACME','$SES','refined Q','$PAR');" > /dev/null
VAL=$(sql "SELECT parent_turn_id FROM chat_turns WHERE id='$CH1';")
[ "$VAL" = "$PAR" ] \
  && pass "V021.1" "chat_turns.parent_turn_id self-FK references parent" \
  || fail "V021.1" "expected parent=$PAR, got '$VAL'"

# V021.2: unknown parent_turn_id rejected by FK
BAD='7e550021-ffff-0000-0000-000000000000'
ERR=$(sql "INSERT INTO chat_turns (id, company_id, session_id, question, parent_turn_id)
           VALUES ('$BAD','$CO_ACME','$SES','q','dead0021-0000-0000-0000-000000000000');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass "V021.2" "unknown parent_turn_id rejected by self-FK" \
  || fail "V021.2" "unknown parent not rejected: $ERR"

# V021.3: ON DELETE SET NULL on parent deletion
sql "DELETE FROM chat_turns WHERE id='$PAR';" > /dev/null
VAL=$(sql "SELECT parent_turn_id IS NULL FROM chat_turns WHERE id='$CH1';")
[ "$VAL" = "t" ] \
  && pass "V021.3" "parent delete SET NULL on child's parent_turn_id" \
  || fail "V021.3" "expected NULL after parent delete, got '$VAL'"
