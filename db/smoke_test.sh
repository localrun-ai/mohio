#!/usr/bin/env bash
# db/smoke_test.sh - Schema smoke test suite
#
# Spins up a throwaway Postgres 17 container, loads all migrations, runs 15
# behavioral checks, then tears down.  Exit code 0 = all passed.
#
# Usage: ./db/smoke_test.sh

set -euo pipefail
cd "$(dirname "$0")/.."

CONTAINER=wikore_smoke_$$
PG_IMAGE=postgres:17
PASS=0
FAIL=0

pass() { echo "PASS $1: $2"; PASS=$((PASS+1)); }
fail() { echo "FAIL $1: $2"; FAIL=$((FAIL+1)); }

# --------------------------------------------------------------------------
# UUIDs (all chars must be 0-9 or a-f)
# --------------------------------------------------------------------------
CO_ACME='ac000001-0000-0000-0000-000000000000'  # company Acme
CO_BETA='be000001-0000-0000-0000-000000000000'  # company Beta
CO_TEMP='ca110000-0000-0000-0000-000000000000'  # throwaway company
U_ALICE='a1100001-0000-0000-0000-000000000000'  # user Alice (Acme)
U_BOB='b0b00001-0000-0000-0000-000000000000'    # user Bob (Beta)
OU_HR='0a100001-0000-0000-0000-000000000000'    # HR department (Acme child)
DOC='d0c00001-0000-0000-0000-000000000000'      # document
VER1='7e100001-0000-0000-0000-000000000000'     # version 1 (active)
VER2='7e100002-0000-0000-0000-000000000000'     # version 2 (second active - should fail)
VER3='7e100003-0000-0000-0000-000000000000'     # version 3 (different version for FK test)
CHK1='c0100001-0000-0000-0000-000000000000'     # chunk in VER3
WIKI='b100a001-0000-0000-0000-000000000000'     # wiki page
WVER='b100a002-0000-0000-0000-000000000000'     # wiki page version 1
VER4='7e100004-0000-0000-0000-000000000000'     # version 4 (for promotion tests)

# --------------------------------------------------------------------------
# Start container and load schema
# --------------------------------------------------------------------------
echo "-- Starting $PG_IMAGE container ($CONTAINER)..."
docker run --rm -e POSTGRES_PASSWORD=test -d --name "$CONTAINER" "$PG_IMAGE" > /dev/null
until docker exec "$CONTAINER" psql -U postgres -c '\q' 2>/dev/null; do sleep 1; done

psql() { docker exec "$CONTAINER" psql -U postgres -At "$@"; }
sql()  { psql -c "$1"; }

echo "-- Loading migrations..."
cat db/migrations/V001__orgs.sql \
    db/migrations/V002__users_and_auth.sql \
    db/migrations/V003__documents.sql \
    db/migrations/V004__wiki.sql \
    db/migrations/V005__chat.sql \
    db/migrations/V006__integrations.sql \
    db/migrations/V007__audit.sql \
    db/migrations/V009__grant_validation.sql \
    db/migrations/V010__promotion_functions.sql \
  | docker exec -i "$CONTAINER" psql -U postgres -v ON_ERROR_STOP=1 -q
echo "-- Migrations loaded."
echo ""

# --------------------------------------------------------------------------
# Fixtures shared across tests
# --------------------------------------------------------------------------
sql "INSERT INTO companies (id, name, slug) VALUES
    ('$CO_ACME', 'Acme', 'acme'),
    ('$CO_BETA', 'Beta', 'beta');" > /dev/null

sql "INSERT INTO users (id, company_id, external_sub, email) VALUES
    ('$U_ALICE', '$CO_ACME', 'sub1', 'alice@acme.com'),
    ('$U_BOB',   '$CO_BETA', 'sub2', 'bob@beta.com');" > /dev/null

ACME_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='$CO_ACME' AND type='root';")
BETA_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='$CO_BETA' AND type='root';")

# --------------------------------------------------------------------------
# 1. Insert company -> root org_unit auto-created
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM org_units WHERE company_id='$CO_ACME' AND type='root';")
[ "$COUNT" -eq 1 ] \
  && pass 1 "root org_unit auto-created on company insert" \
  || fail 1 "root org_unit missing (got $COUNT)"

# --------------------------------------------------------------------------
# 2. Root closure self-entry exists at depth 0
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM org_unit_closure
             WHERE company_id='$CO_ACME'
               AND ancestor_id='$ACME_ROOT'
               AND descendant_id='$ACME_ROOT'
               AND depth=0;")
[ "$COUNT" -eq 1 ] \
  && pass 2 "root closure self-entry at depth 0 exists" \
  || fail 2 "root closure self-entry missing (got $COUNT)"

# --------------------------------------------------------------------------
# 3. Direct root delete is blocked by trigger
# --------------------------------------------------------------------------
ERR=$(sql "DELETE FROM org_units WHERE id='$ACME_ROOT';" 2>&1 || true)
echo "$ERR" | grep -q "Cannot delete root org_unit" \
  && pass 3 "direct root delete blocked by trigger" \
  || fail 3 "direct root delete was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 4. Company delete cascades (root trigger allows when company row is gone)
# --------------------------------------------------------------------------
sql "INSERT INTO companies (id, name, slug)
     VALUES ('$CO_TEMP', 'Temp', 'temp');" > /dev/null
ERR=$(sql "DELETE FROM companies WHERE id='$CO_TEMP';" 2>&1 || true)
echo "$ERR" | grep -qi "error\|exception" \
  && fail 4 "company cascade delete failed: $ERR" \
  || pass 4 "company delete cascades (root trigger allows when company is gone)"

# --------------------------------------------------------------------------
# 5. Non-root child insert creates full closure chain
# --------------------------------------------------------------------------
sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU_HR', '$CO_ACME', '$ACME_ROOT', 'department', 'hr', 'HR');" > /dev/null

# Expect 2 rows: self (depth 0) + root ancestor (depth 1)
ROWS=$(sql "SELECT COUNT(*) FROM org_unit_closure
            WHERE company_id='$CO_ACME' AND descendant_id='$OU_HR';")
[ "$ROWS" -eq 2 ] \
  && pass 5 "child closure chain correct ($ROWS ancestor rows: self + root)" \
  || fail 5 "expected 2 ancestor rows for child, got $ROWS"

# --------------------------------------------------------------------------
# 6. Direct parent_id update is blocked
# --------------------------------------------------------------------------
ERR=$(sql "UPDATE org_units SET parent_id='$OU_HR'
           WHERE id='$OU_HR';" 2>&1 || true)
echo "$ERR" | grep -q "parent_id cannot be updated" \
  && pass 6 "direct parent_id update blocked by trigger" \
  || fail 6 "parent_id update was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 7. Cross-company parent insert fails (composite FK)
# --------------------------------------------------------------------------
ERR=$(sql "INSERT INTO org_units (company_id, parent_id, type, slug, name)
           VALUES ('$CO_ACME', '$BETA_ROOT', 'department', 'cross', 'Cross');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass 7 "cross-company parent insert rejected by composite FK" \
  || fail 7 "cross-company parent insert was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 8. Membership granted_by from wrong company is rejected
# --------------------------------------------------------------------------
ERR=$(sql "INSERT INTO memberships (company_id, user_id, org_unit_id, role, granted_by)
           VALUES ('$CO_ACME', '$U_ALICE', '$ACME_ROOT', 'viewer', '$U_BOB');" 2>&1 || true)
echo "$ERR" | grep -q "does not belong to company" \
  && pass 8 "membership with cross-company granted_by rejected" \
  || fail 8 "cross-company granted_by was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 9. At most one active document version per document
# --------------------------------------------------------------------------
sql "INSERT INTO documents (id, company_id, owner_org_unit_id, filename)
     VALUES ('$DOC', '$CO_ACME', '$ACME_ROOT', 'policy.pdf');" > /dev/null

sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$VER1', '$CO_ACME', '$DOC', 1, 'h1',
             'done', now(), now(), 5, 'active');" > /dev/null

ERR=$(sql "INSERT INTO document_versions
             (id, company_id, document_id, version_no, source_hash,
              ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
           VALUES ('$VER2', '$CO_ACME', '$DOC', 2, 'h2',
                   'done', now(), now(), 3, 'active');" 2>&1 || true)
echo "$ERR" | grep -qi "unique\|duplicate" \
  && pass 9 "second active version rejected by partial unique index" \
  || fail 9 "second active version was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 10. wiki_page_sources chunk must belong to the cited version
# --------------------------------------------------------------------------
sql "UPDATE document_versions
     SET lifecycle_status='deprecated', superseded_at=now()
     WHERE id='$VER1';" > /dev/null

sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$VER3', '$CO_ACME', '$DOC', 3, 'h3',
             'done', now(), now(), 1, 'active');" > /dev/null

sql "INSERT INTO document_chunks
       (id, company_id, document_version_id, chunk_index, content, content_hash)
     VALUES ('$CHK1', '$CO_ACME', '$VER3', 0, 'text', 'ch');" > /dev/null

sql "INSERT INTO wiki_pages (id, company_id, org_unit_id, slug, title)
     VALUES ('$WIKI', '$CO_ACME', '$ACME_ROOT', 'policy', 'Policy');" > /dev/null

sql "INSERT INTO wiki_page_versions
       (id, company_id, wiki_page_id, version_no, content, content_hash, lifecycle_status)
     VALUES ('$WVER', '$CO_ACME', '$WIKI', 1, 'Content v1', 'h_wiki1', 'active');" > /dev/null

# CHK1 belongs to VER3 but we cite it under VER1 - must fail
ERR=$(sql "INSERT INTO wiki_page_sources
             (company_id, wiki_page_version_id, document_id, document_version_id, chunk_id)
           VALUES ('$CO_ACME', '$WVER', '$DOC', '$VER1', '$CHK1');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass 10 "wiki_page_sources chunk from wrong version rejected by FK" \
  || fail 10 "chunk from wrong version was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 11-15. promote_document_version() tests
# --------------------------------------------------------------------------
# Set up a second 'done' document version (VER4) for promotion tests.
# VER3 is currently the active version.
sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, chunk_count, lifecycle_status)
     VALUES ('$VER4', '$CO_ACME', '$DOC', 4, 'h4',
             'done', now(), 2, 'draft');" > /dev/null

# --------------------------------------------------------------------------
# 11. Direct activate-first update fails (unique violation: VER3 already active)
# --------------------------------------------------------------------------
ERR=$(sql "UPDATE document_versions
           SET lifecycle_status='active', activated_at=now(), superseded_at=NULL
           WHERE id='$VER4';" 2>&1 || true)
echo "$ERR" | grep -qi "unique\|duplicate" \
  && pass 11 "direct activate without deprecating old version rejected by partial unique index" \
  || fail 11 "direct activate-first was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 12. promote_document_version() succeeds
# --------------------------------------------------------------------------
ERR=$(sql "SELECT promote_document_version('$CO_ACME', '$DOC', '$VER4');" 2>&1 || true)
echo "$ERR" | grep -qi "error\|exception" \
  && fail 12 "promote_document_version() raised an error: $ERR" \
  || pass 12 "promote_document_version() completed without error"

# --------------------------------------------------------------------------
# 13. Exactly one active version remains after promotion
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM document_versions
             WHERE company_id='$CO_ACME' AND document_id='$DOC'
               AND lifecycle_status='active';")
[ "$COUNT" -eq 1 ] \
  && pass 13 "exactly one active version after promotion (got $COUNT)" \
  || fail 13 "expected 1 active version after promotion, got $COUNT"

# --------------------------------------------------------------------------
# 14. Previously active version (VER3) now has superseded_at set
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM document_versions
             WHERE id='$VER3'
               AND lifecycle_status='deprecated'
               AND superseded_at IS NOT NULL;")
[ "$COUNT" -eq 1 ] \
  && pass 14 "old active version deprecated with superseded_at set" \
  || fail 14 "old active version not correctly deprecated (got $COUNT)"

# --------------------------------------------------------------------------
# 15. New active version (VER4) has activated_at set and superseded_at NULL
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM document_versions
             WHERE id='$VER4'
               AND lifecycle_status='active'
               AND activated_at IS NOT NULL
               AND superseded_at IS NULL;")
[ "$COUNT" -eq 1 ] \
  && pass 15 "new active version has activated_at set and superseded_at NULL" \
  || fail 15 "new active version state incorrect (got $COUNT)"

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo ""
docker rm -f "$CONTAINER" > /dev/null
TOTAL=$((PASS+FAIL))
echo "-- Results: $PASS/$TOTAL passed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
