# Per-migration smoke tests for V025 (wiki page tombstones, K2 symmetry).

WP25='b100b025-0000-0000-0000-000000000000'
WV25='b100b025-0000-0000-0000-00000000000a'

sql "INSERT INTO wiki_pages (id, company_id, org_unit_id, slug, title)
     VALUES ('$WP25','$CO_ACME','$ACME_ROOT','v025-policy','Policy 25');" > /dev/null
sql "INSERT INTO wiki_page_versions
       (id, company_id, wiki_page_id, version_no, content, content_hash, lifecycle_status)
     VALUES ('$WV25','$CO_ACME','$WP25',1,'confidential synthesized content','h25_sha','active');" > /dev/null

# V025.1: wiki_pages.deleted_at defaults to NULL on new rows
VAL=$(sql "SELECT deleted_at IS NULL FROM wiki_pages WHERE id='$WP25';")
[ "$VAL" = "t" ] \
  && pass "V025.1" "wiki_pages.deleted_at defaults to NULL" \
  || fail "V025.1" "expected NULL, got '$VAL'"

# V025.2: marking deleted lights up the partial index filter
sql "UPDATE wiki_pages SET deleted_at = now() WHERE id='$WP25';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM wiki_pages WHERE company_id='$CO_ACME' AND deleted_at IS NOT NULL;")
[ "$COUNT" -ge 1 ] \
  && pass "V025.2" "wiki_pages.deleted_at queryable via partial index (count=$COUNT)" \
  || fail "V025.2" "expected >=1 deleted wiki page, got $COUNT"

# V025.3: tombstone insert with content-free record
sql "INSERT INTO wiki_page_version_tombstones
       (company_id, wiki_page_version_id, wiki_page_id, content_hash, reason)
     VALUES ('$CO_ACME','$WV25','$WP25','h25_sha','gdpr_erasure');" > /dev/null
ROW=$(sql "SELECT wiki_page_id || '|' || content_hash || '|' || reason
           FROM wiki_page_version_tombstones
           WHERE company_id='$CO_ACME' AND wiki_page_version_id='$WV25';")
[ "$ROW" = "$WP25|h25_sha|gdpr_erasure" ] \
  && pass "V025.3" "wiki_page_version_tombstones records hash + reason" \
  || fail "V025.3" "tombstone row wrong (got '$ROW')"

# V025.4: tombstone survives hard-delete of underlying wiki page version
# (No FK on wiki_page_version_id; durable audit by design.)
sql "DELETE FROM wiki_page_versions WHERE id='$WV25';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM wiki_page_version_tombstones
             WHERE company_id='$CO_ACME' AND wiki_page_version_id='$WV25';")
[ "$COUNT" = "1" ] \
  && pass "V025.4" "tombstone survives hard-delete of wiki_page_versions row" \
  || fail "V025.4" "tombstone lost after underlying version delete (count=$COUNT)"

# V025.5: duplicate tombstone for same version rejected (PK constraint)
ERR=$(sql "INSERT INTO wiki_page_version_tombstones
             (company_id, wiki_page_version_id, wiki_page_id, content_hash, reason)
           VALUES ('$CO_ACME','$WV25','$WP25','h25_sha','source_deleted');" 2>&1 || true)
echo "$ERR" | grep -qi "duplicate\|unique\|violates" \
  && pass "V025.5" "duplicate tombstone for same version rejected by PK" \
  || fail "V025.5" "duplicate not rejected: $ERR"

# V025.6: company_id cascade purges tombstones when the company is deleted
sql "INSERT INTO companies (id, name, slug) VALUES
     ('cafe0025-0000-0000-0000-000000000000','Cafe 25','cafe25');" > /dev/null
CAFE_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='cafe0025-0000-0000-0000-000000000000' AND type='root';")
sql "INSERT INTO wiki_pages (id, company_id, org_unit_id, slug, title)
     VALUES ('b100c025-0000-0000-0000-000000000000','cafe0025-0000-0000-0000-000000000000','$CAFE_ROOT','x','X');" > /dev/null
sql "INSERT INTO wiki_page_version_tombstones
       (company_id, wiki_page_version_id, wiki_page_id, content_hash, reason)
     VALUES ('cafe0025-0000-0000-0000-000000000000',
             'b100c025-0000-0000-0000-00000000000a','b100c025-0000-0000-0000-000000000000',
             'h_cafe','source_deleted');" > /dev/null
sql "DELETE FROM companies WHERE id='cafe0025-0000-0000-0000-000000000000';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM wiki_page_version_tombstones
             WHERE company_id='cafe0025-0000-0000-0000-000000000000';")
[ "$COUNT" = "0" ] \
  && pass "V025.6" "tombstones cascade-purged on company delete" \
  || fail "V025.6" "tombstones not purged (count=$COUNT)"
