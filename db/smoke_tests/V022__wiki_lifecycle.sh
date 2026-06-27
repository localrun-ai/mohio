# Per-migration smoke tests for V022 (wiki_lifecycle).
#
# Existing fixtures (from inline test 10): WIKI page + WVER active version.
# That row was inserted before V022's trigger existed, so it has activated_at
# either NULL (legacy) or set by V022 trigger if INSERT had been replayed.
# We exercise the new CHECK + promote function on a fresh page.

WP2='b100b002-0000-0000-0000-000000000000'
WV2A='b100b002-0000-0000-0000-00000000000a'
WV2B='b100b002-0000-0000-0000-00000000000b'

sql "INSERT INTO wiki_pages (id, company_id, org_unit_id, slug, title)
     VALUES ('$WP2','$CO_ACME','$ACME_ROOT','v022-policy','Policy 2');" > /dev/null

# V022.1: trigger auto-fills activated_at on INSERT with lifecycle_status='active'
sql "INSERT INTO wiki_page_versions (id, company_id, wiki_page_id, version_no, content, content_hash, lifecycle_status)
     VALUES ('$WV2A','$CO_ACME','$WP2',1,'first version','h1','active');" > /dev/null
VAL=$(sql "SELECT activated_at IS NOT NULL FROM wiki_page_versions WHERE id='$WV2A';")
[ "$VAL" = "t" ] \
  && pass "V022.1" "trigger auto-fills wiki_page_versions.activated_at on INSERT active" \
  || fail "V022.1" "activated_at not set (got '$VAL')"

# V022.2: CHECK prevents lifecycle_status='active' with NULL activated_at via direct UPDATE
ERR=$(sql "INSERT INTO wiki_page_versions (id, company_id, wiki_page_id, version_no, content, content_hash, lifecycle_status, activated_at)
           VALUES ('b100b002-0000-0000-0000-0000000000ee','$CO_ACME','$WP2',2,'bad','hbad','active',NULL);" 2>&1 || true)
# trigger will set activated_at to clock_timestamp on INSERT, so this still passes.
# Instead test the CHECK via a deferred-trigger-bypass path is awkward; test the obvious case:
# A direct UPDATE that tries to put a row into 'active' with explicit NULL activated_at and
# explicit superseded_at (trigger only fires BEFORE UPDATE OF lifecycle_status, so changing
# activated_at directly does not trigger reset).
sql "INSERT INTO wiki_page_versions (id, company_id, wiki_page_id, version_no, content, content_hash, lifecycle_status, activated_at)
     VALUES ('$WV2B','$CO_ACME','$WP2',2,'second','h2','draft', NULL);" > /dev/null
ERR=$(sql "UPDATE wiki_page_versions SET activated_at=NULL WHERE id='$WV2A';" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V022.2" "CHECK rejects active row with NULL activated_at" \
  || fail "V022.2" "CHECK did not block (err='$ERR')"

# V022.3: promote_wiki_page_version() deprecates old + activates new atomically
sql "SELECT promote_wiki_page_version('$CO_ACME','$WP2','$WV2B');" > /dev/null
ACTIVE_COUNT=$(sql "SELECT COUNT(*) FROM wiki_page_versions WHERE wiki_page_id='$WP2' AND lifecycle_status='active';")
DEPRECATED_COUNT=$(sql "SELECT COUNT(*) FROM wiki_page_versions WHERE wiki_page_id='$WP2' AND lifecycle_status='deprecated' AND superseded_at IS NOT NULL;")
[ "$ACTIVE_COUNT" = "1" ] && [ "$DEPRECATED_COUNT" = "1" ] \
  && pass "V022.3" "promote_wiki_page_version() swap: 1 active, 1 deprecated+superseded" \
  || fail "V022.3" "expected 1 active + 1 deprecated, got active=$ACTIVE_COUNT dep=$DEPRECATED_COUNT"

# V022.4: promote rejects archived version
sql "UPDATE wiki_page_versions SET lifecycle_status='archived' WHERE id='$WV2A';" > /dev/null
ERR=$(sql "SELECT promote_wiki_page_version('$CO_ACME','$WP2','$WV2A');" 2>&1 || true)
echo "$ERR" | grep -qi "not promotable\|archived" \
  && pass "V022.4" "promote_wiki_page_version() rejects archived version" \
  || fail "V022.4" "archived was promoted (err='$ERR')"
