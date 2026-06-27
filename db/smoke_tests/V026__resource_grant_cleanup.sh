# Per-migration smoke tests for V026 (resource_grants zombie cleanup).

OU_A='0a100026-0000-0000-0000-00000000aaaa'
OU_B='0a100026-0000-0000-0000-00000000bbbb'
DOC26='d0c00026-0000-0000-0000-000000000000'
WP26='b100b026-0000-0000-0000-000000000000'
RG_DOC='6ec90026-0000-0000-0000-000000000001'
RG_WP='6ec90026-0000-0000-0000-000000000002'
RG_OU_RES='6ec90026-0000-0000-0000-000000000003'
RG_OU_PRIN='6ec90026-0000-0000-0000-000000000004'

# Fixtures: two org_units, one document owned by OU_A, one wiki_page owned by OU_A.
sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU_A','$CO_ACME','$ACME_ROOT','team','t26a','T26A'),
            ('$OU_B','$CO_ACME','$ACME_ROOT','team','t26b','T26B');" > /dev/null

sql "INSERT INTO documents (id, company_id, owner_org_unit_id, filename)
     VALUES ('$DOC26','$CO_ACME','$OU_A','doc26.pdf');" > /dev/null

sql "INSERT INTO wiki_pages (id, company_id, org_unit_id, slug, title)
     VALUES ('$WP26','$CO_ACME','$OU_A','wp26','WP 26');" > /dev/null

# Four grants: one per resource type, plus one where org_unit is the principal.
sql "INSERT INTO resource_grants
       (id, company_id, resource_type, resource_id, principal_type, principal_id, permission)
     VALUES
       ('$RG_DOC',    '$CO_ACME','document', '$DOC26','org_unit','$OU_B','read'),
       ('$RG_WP',     '$CO_ACME','wiki_page','$WP26', 'org_unit','$OU_B','read'),
       ('$RG_OU_RES', '$CO_ACME','org_unit', '$OU_A', 'org_unit','$OU_B','read'),
       ('$RG_OU_PRIN','$CO_ACME','org_unit', '$ACME_ROOT','org_unit','$OU_A','admin');" > /dev/null

# Sanity: all four grants present.
COUNT=$(sql "SELECT COUNT(*) FROM resource_grants WHERE id IN
             ('$RG_DOC','$RG_WP','$RG_OU_RES','$RG_OU_PRIN');")
[ "$COUNT" = "4" ] \
  && pass "V026.0" "fixtures: 4 resource_grants inserted (sanity)" \
  || fail "V026.0" "expected 4 fixture grants, got $COUNT"

# V026.1: delete document -> grant on it is cleaned
sql "DELETE FROM documents WHERE id='$DOC26';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM resource_grants WHERE id='$RG_DOC';")
[ "$COUNT" = "0" ] \
  && pass "V026.1" "document delete cleans matching resource_grants" \
  || fail "V026.1" "doc grant survived (count=$COUNT)"

# V026.2: delete wiki_page -> grant on it is cleaned
sql "DELETE FROM wiki_pages WHERE id='$WP26';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM resource_grants WHERE id='$RG_WP';")
[ "$COUNT" = "0" ] \
  && pass "V026.2" "wiki_page delete cleans matching resource_grants" \
  || fail "V026.2" "wiki grant survived (count=$COUNT)"

# V026.3: delete org_unit OU_A -> cleans BOTH grants
# (RG_OU_RES has resource_id=OU_A; RG_OU_PRIN has principal_id=OU_A)
sql "DELETE FROM org_units WHERE id='$OU_A';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM resource_grants
             WHERE id IN ('$RG_OU_RES','$RG_OU_PRIN');")
[ "$COUNT" = "0" ] \
  && pass "V026.3" "org_unit delete cleans grants where it is resource OR principal" \
  || fail "V026.3" "org_unit grants survived (count=$COUNT)"

# V026.4: unrelated grant (resource and principal both unrelated to the
# deleted org_unit) survives. Create a fresh team OU_C with a self-grant,
# then delete an UNRELATED team OU_D. RG_KEEP must remain.
OU_C='0a100026-0000-0000-0000-00000000cccc'
OU_D='0a100026-0000-0000-0000-00000000dddd'
RG_KEEP='6ec90026-0000-0000-0000-000000000005'

sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU_C','$CO_ACME','$ACME_ROOT','team','t26c','T26C'),
            ('$OU_D','$CO_ACME','$ACME_ROOT','team','t26d','T26D');" > /dev/null
sql "INSERT INTO resource_grants
       (id, company_id, resource_type, resource_id, principal_type, principal_id, permission)
     VALUES ('$RG_KEEP','$CO_ACME','org_unit','$OU_C','org_unit','$OU_C','read');" > /dev/null

sql "DELETE FROM org_units WHERE id='$OU_D';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM resource_grants WHERE id='$RG_KEEP';")
[ "$COUNT" = "1" ] \
  && pass "V026.4" "trigger leaves unrelated grants alone" \
  || fail "V026.4" "unrelated grant was removed (count=$COUNT)"
