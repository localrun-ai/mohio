# Wikore DB schema review - enterprise RAG + wiki platform

You are reviewing the complete PostgreSQL schema (V001-V009) and Redis key
contract (V008) for Wikore, a self-hosted enterprise RAG + wiki platform.
The backend will be C++23/Drogon. PostgreSQL 17, Qdrant, Redis.

## What Wikore does

- Companies are hard tenant boundaries. Each company has an org hierarchy
  (Company -> Division -> Department -> Team, etc.).
- Documents are ingested, chunked, embedded into Qdrant, and retrieved for
  LLM-powered RAG queries.
- Wiki pages are LLM-synthesized knowledge surfaces derived from approved
  documents.
- Access control: users and groups belong to org_units via memberships.
  Resource grants give org_units explicit access to documents or wiki pages
  outside their hierarchy. The Qdrant chunk payload stores `access_scope_ids`
  (org_unit UUIDs) and retrieval filters by intersection with the user's
  resolved scope set.
- External integrations (Slack, Jira, Confluence, GitHub, etc.) are callable
  as MCP tools from the LLM.
- SSO via external OIDC providers (Keycloak, Authentik, Azure AD). JWT
  validated by C++, not stored.

## Design decisions already finalized - do not re-litigate

- Composite FK pattern: every tenant-owned table has UNIQUE(company_id, id)
  and all FK references include company_id to enforce same-company membership
  at the DB level.
- Root org_unit per company (type='root', slug='root', name=company.name) as
  a mandatory anchor. Access semantics are uniform: "company-wide" is just
  membership on root with applies_to=self_and_descendants.
- org_unit_closure precomputes the transitive ancestor/descendant graph.
  Direct parent_id updates are blocked; a future move_org_unit() function
  will maintain the closure atomically.
- Document versioning: each content change creates a new document_versions
  row. Chunks reference document_version_id, not document_id. Point-in-time
  retrieval uses activated_at/superseded_at in both Postgres and Qdrant
  payload.
- access_scope_ids model: Qdrant chunk payloads store org_unit UUID arrays.
  resource_grants.principal_type is restricted to 'org_unit' for MVP because
  user/group-specific grants cannot be safely represented in the UUID
  intersection model without overgrant. User/group grants require a future
  access_tokens TEXT[] typed-prefix model.
- Audit log: no FKs (lock overhead + UUID preservation after deletion),
  append-only via triggers + DB role, quarterly partitions.
- Redis invalidation: membership/group changes invalidate lr:eff only
  (user.resolved_scopes changed); grant/document/org-structure changes
  require Qdrant payload resync (access_scope_ids changed).
- authority_level lives on documents, not document_versions, for MVP.
  Acknowledged as a V2 migration.
- wiki_page_sources uses a 3-level FK chain: document -> version (with
  document match) -> chunk (with version match). All FKs are ON DELETE
  RESTRICT. Two partial unique indexes instead of a COALESCE sentinel.
- credentials_key_id on integrations for key rotation without full
  re-encryption.
- external_issuer on users for multi-IdP SSO (sub claim is unique per
  issuer, not globally).

## Full schema (V001-V009)

Paste the output of the following command here before sending:

```
cat db/migrations/V001__orgs.sql \
    db/migrations/V002__users_and_auth.sql \
    db/migrations/V003__documents.sql \
    db/migrations/V004__wiki.sql \
    db/migrations/V005__chat.sql \
    db/migrations/V006__integrations.sql \
    db/migrations/V007__audit.sql \
    db/migrations/V008__redis_keys.md \
    db/migrations/V009__grant_validation.sql
```

## Review questions

Please review the schema across these dimensions. Flag findings as BLOCKER /
RECOMMENDED / OPTIONAL where you have a strong opinion. If a concern is
already handled by a finalized design decision listed above, skip it.

### 1. Correctness and gaps

- Are there any constraint gaps, trigger edge cases, or state machine holes
  that could allow bad data even with correct C++ code?
- Is the `lr:eff:{company_id}:{user_id}:{org_unit_id}` Redis key structure
  correct? The org_unit_id here is the scope being queried from (e.g. "what
  can Alice see from within HR?"), not Alice's org_unit membership. Is this
  the right key structure for the access resolver, or does it lead to key
  explosion?
- The `memberships` table has no `expires_at`. Resource grants have one.
  Should memberships also support time-limited access?

### 2. Hard-to-reverse decisions

- What design choices in the current schema will be painful to change once
  C++ code exists and data is in production? Focus on things that require a
  data migration or a change to the Qdrant payload contract, not just a
  schema column add.
- `access_scope_ids UUID[]` in document_chunks is the source of truth that
  feeds Qdrant. What is the migration cost if this model needs to change
  (e.g. to access_tokens TEXT[]) after production data exists?

### 3. Missing tables or concerns for the features described

- No soft delete anywhere. `companies`, `org_units`, `users` are all
  hard-deleted with CASCADE. What is the impact on audit log integrity, and
  should at least users have a `deactivated_at`?
- Wiki pages have content as a single mutable TEXT column with no version
  history. Documents have full versioning; wiki pages do not. Is this
  intentional, and what is the consequence for citation reproducibility when
  a wiki page is edited?
- No storage quota or byte tracking per company or org_unit. The schema
  records `size_bytes` per document_version but there is no aggregate
  enforcement mechanism. Should there be?
- The `move_org_unit()` function is referenced in comments but not
  implemented. What does a correct closure-table update look like for a
  subtree move, and are there any edge cases the current schema makes
  difficult (e.g. cross-company move, moving root)?

### 4. Future extensibility

- The plan is to add user/group-specific grants post-MVP via an
  access_tokens model. What would need to change in the schema, the Qdrant
  payload, and the Redis key structure to support this? Is the current
  schema a clean foundation for that migration?
- Group hierarchy: groups are currently flat (no group-of-groups). Does the
  current schema make nested groups painful to add later?
- When a new embedding model is added, all existing chunks need to be
  re-embedded. The schema tracks `document_chunk_vectors` per
  (chunk_id, embedding_model_id) but has no migration_status or
  re-embedding progress field. Is this a gap, or is it out of scope for
  the DB?
- The `companies.settings JSONB` column is unconstrained. What should go in
  there vs typed columns? Rate limits? Feature flags? LLM model selection?

### 5. Operational concerns

- The audit_log has explicit quarterly partitions through 2027-Q2 plus a
  DEFAULT catch-all. What is the recommended partition management story
  beyond that?
- PostgreSQL Row Level Security (RLS) was not used; the schema relies
  entirely on application-level company_id filtering. Is that acceptable
  for a self-hosted enterprise product, or should RLS be considered as
  defense-in-depth?
- The `resource_grants` validation trigger (V009) does a live SELECT against
  org_units/documents/wiki_pages on every INSERT/UPDATE. At high ingest
  rates this could be a hot path. Is there a better approach?

### 6. Anything else

- Anything not covered above: naming inconsistencies, index coverage holes,
  trigger interaction risks, or schema patterns that look non-standard for
  PostgreSQL 17.
