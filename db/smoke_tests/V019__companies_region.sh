# Per-migration smoke tests for V019 (companies.region).
# Sourced by db/smoke_test.sh after the inline test block.
# Test IDs prefixed "V019." so they cannot collide with other PRs' files.

# V019.1: existing rows backfilled with 'default'
VAL=$(sql "SELECT region FROM companies WHERE id='$CO_ACME';")
[ "$VAL" = "default" ] \
  && pass "V019.1" "existing companies backfilled with region='default'" \
  || fail "V019.1" "expected 'default', got '$VAL'"

# V019.2: new region values accept arbitrary strings
sql "UPDATE companies SET region='eu-west' WHERE id='$CO_ACME';" > /dev/null
VAL=$(sql "SELECT region FROM companies WHERE id='$CO_ACME';")
[ "$VAL" = "eu-west" ] \
  && pass "V019.2" "companies.region updatable to arbitrary string ('eu-west')" \
  || fail "V019.2" "expected 'eu-west', got '$VAL'"

# V019.3: NOT NULL enforced
ERR=$(sql "UPDATE companies SET region=NULL WHERE id='$CO_ACME';" 2>&1 || true)
echo "$ERR" | grep -qi "null" \
  && pass "V019.3" "companies.region rejects NULL" \
  || fail "V019.3" "NULL not rejected: $ERR"
