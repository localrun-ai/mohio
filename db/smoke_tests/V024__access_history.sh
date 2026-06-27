# Per-migration smoke tests for V024 (access policy temporal history).

OU='0a100024-0000-0000-0000-000000000000'
M1='6e4b0024-0000-0000-0000-000000000001'

# Need an org_unit under ACME_ROOT for the membership; create it.
sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU','$CO_ACME','$ACME_ROOT','team','t24','Team V024');" > /dev/null

# V024.1: insert membership -> open history row appears
sql "INSERT INTO memberships (id, company_id, user_id, org_unit_id, role)
     VALUES ('$M1','$CO_ACME','$U_ALICE','$OU','viewer');" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM memberships_history
             WHERE live_row_id='$M1' AND valid_to IS NULL AND change_kind='insert';")
[ "$COUNT" = "1" ] \
  && pass "V024.1" "INSERT on memberships opens one history interval" \
  || fail "V024.1" "expected 1 open history row, got $COUNT"

# V024.2: update membership -> previous interval closed, new interval opened
sql "UPDATE memberships SET role='editor' WHERE id='$M1';" > /dev/null
CLOSED=$(sql "SELECT COUNT(*) FROM memberships_history
              WHERE live_row_id='$M1' AND valid_to IS NOT NULL AND change_kind='insert';")
OPEN=$(sql "SELECT COUNT(*) FROM memberships_history
            WHERE live_row_id='$M1' AND valid_to IS NULL AND change_kind='update';")
[ "$CLOSED" = "1" ] && [ "$OPEN" = "1" ] \
  && pass "V024.2" "UPDATE closes prior insert interval, opens new update interval" \
  || fail "V024.2" "expected 1 closed insert + 1 open update, got closed=$CLOSED open=$OPEN"

# V024.3: time-travel - role at as-of just-before-update is 'viewer'
PIT=$(sql "SELECT valid_from - interval '1 ms' FROM memberships_history
           WHERE live_row_id='$M1' AND change_kind='update';")
ROLE_AT=$(sql "SELECT role FROM memberships_history
               WHERE live_row_id='$M1'
                 AND valid_from <= '$PIT'::timestamptz
                 AND (valid_to IS NULL OR valid_to > '$PIT'::timestamptz);")
[ "$ROLE_AT" = "viewer" ] \
  && pass "V024.3" "time-travel query returns role='viewer' before the update" \
  || fail "V024.3" "expected 'viewer' at PIT, got '$ROLE_AT'"

# V024.4: DELETE writes a tombstone row, no open interval remains
sql "DELETE FROM memberships WHERE id='$M1';" > /dev/null
OPEN=$(sql "SELECT COUNT(*) FROM memberships_history
            WHERE live_row_id='$M1' AND valid_to IS NULL;")
TOMB=$(sql "SELECT COUNT(*) FROM memberships_history
            WHERE live_row_id='$M1' AND change_kind='delete';")
[ "$OPEN" = "0" ] && [ "$TOMB" = "1" ] \
  && pass "V024.4" "DELETE closes open interval and writes one tombstone row" \
  || fail "V024.4" "expected 0 open + 1 tombstone, got open=$OPEN tomb=$TOMB"

# V024.5: same flow for resource_grants
RG='6ec90024-0000-0000-0000-000000000001'
sql "INSERT INTO resource_grants
       (id, company_id, resource_type, resource_id, principal_type, principal_id, permission)
     VALUES ('$RG','$CO_ACME','org_unit','$ACME_ROOT','org_unit','$OU','read');" > /dev/null
sql "UPDATE resource_grants SET permission='write' WHERE id='$RG';" > /dev/null
INTERVALS=$(sql "SELECT COUNT(*) FROM resource_grants_history WHERE live_row_id='$RG';")
OPEN=$(sql "SELECT COUNT(*) FROM resource_grants_history WHERE live_row_id='$RG' AND valid_to IS NULL;")
[ "$INTERVALS" = "2" ] && [ "$OPEN" = "1" ] \
  && pass "V024.5" "resource_grants history: 2 intervals total, 1 open after update" \
  || fail "V024.5" "expected 2 intervals + 1 open, got total=$INTERVALS open=$OPEN"
