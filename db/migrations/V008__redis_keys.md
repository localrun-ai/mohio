# Redis Key Reference

All keys are prefixed with `lr:` (localrun namespace).

## Auth

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:jwt:{jti}` | `"1"` | until JWT exp | Revoked JTIs (logout / forced revoke). Presence = revoked. |
| `lr:api_key:{key_hash}` | JSON Identity | 5 min | API key fast-path; invalidate on revoke |

## Access resolution

The critical path for RAG: resolving `access_scope_ids` (which org_unit_ids a user can
read when querying a given org_unit) must be fast and consistent.

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:eff:{company_id}:{user_id}:{org_unit_id}` | JSON array of UUID strings | see below | Resolved access_scope_ids for this user+scope; drives Qdrant filter |
| `lr:user:{user_id}` | JSON Identity record | 5 min | Profile + is_admin; invalidate on user update |
| `lr:tree:{company_id}:{org_unit_id}` | JSON subtree | 2 min | Admin UI tree; invalidate on any mutation under this node |

### lr:eff TTL policy (grant expiry awareness)

`resource_grants.expires_at` creates a correctness hazard: if an effective scope entry
is cached with a fixed 5-minute TTL but a contributing grant expires within that window,
the user retains access after the grant has lapsed.

**Resolution: Option B - clamp TTL to the earliest grant expiry.**

When building and caching an `lr:eff` entry, the access resolver must:

1. Collect `MIN(g.expires_at) FILTER (WHERE g.expires_at IS NOT NULL)` from all
   `resource_grants` rows that contribute to this user's effective scope.
2. If `min_expiry IS NULL` (no time-limited grants in scope): use default TTL = 5 min.
3. If `min_expiry - now() < 0`: the grant has already lapsed; do not cache (or set TTL = 1s).
4. If `min_expiry - now() < 5 min`: set TTL = `CEIL(min_expiry - now())`.
5. Otherwise: use default TTL = 5 min.

`memberships` rows have no `expires_at` - indefinite memberships always contribute
the full 5-minute window. Only `resource_grants` time-limits are involved here.

Worst-case stale window with this policy: 1 clock-tick between the resolver reading
`MIN(expires_at)` and writing the Redis TTL. Acceptable for enterprise use.

## Rate limiting

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:rl:chat:{user_id}` | counter | 60 s | Sliding window; max N chat requests/min per user |
| `lr:rl:ingest:{org_unit_id}` | counter | 60 s | Ingest rate per org_unit |

## LLM concurrency (Lua semaphore, same pattern as Astraea)

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:llm:sem` | integer | - | Global in-flight LLM requests; capped by LLM_CONCURRENCY env var |
| `lr:llm:sem:{company_id}` | integer | - | Per-company cap (optional; prevents one tenant starving others) |

## Ingest queue

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:ingest:q:{org_unit_id}` | List of doc_id | - | Serializes ingests per org_unit; LPOP = claim work |
| `lr:ingest:lock:{doc_id}` | worker_id | 10 min | Distributed lock; prevents duplicate chunk processing |

## Qdrant payload resync queue

When memberships or resource_grants change, affected chunk payloads (access_scope_ids)
must be updated in Qdrant. Changes are enqueued here for background processing.

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:resync:q` | List of JSON {company_id, org_unit_id, reason} | - | Background worker pops and recomputes chunk scopes |
| `lr:resync:lock:{company_id}:{org_unit_id}` | worker_id | 5 min | Prevent parallel resync for same scope |

## MCP tool cache (read-only tools only)

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:tool:{tool_name}:{sha256(args)}` | JSON result | 5 min | Cache Jira search, Confluence page, etc. Write tools NEVER cached. |
| `lr:oauth:{integration_id}` | access_token | until exp | OAuth token cache; refresh before expiry |

## Invalidation rules

### Mental model: two independent axes

`access_scope_ids` in a Qdrant chunk payload answers: **"which org_unit IDs grant
visibility to this chunk?"** This is a property of the document/grant configuration,
not of the user population.

`lr:eff:{company_id}:{user_id}:{org_unit_id}` answers: **"which org_unit IDs does
this user belong to when querying from this scope?"** This is a property of membership
and group composition.

At query time the filter is: `access_scope_ids intersects user.resolved_scopes`.

The two axes change independently:

- **Membership/group changes** shift user.resolved_scopes. They never alter
  access_scope_ids stored in Qdrant. Only `lr:eff` keys need invalidation; Qdrant
  payloads are untouched.

- **Grant/document/org-structure changes** shift which org_units appear in
  access_scope_ids. Qdrant payload resync is required; `lr:eff` invalidation
  is also needed for any user whose scope resolution depended on those grants.

Treating membership changes as triggers for Qdrant resync is wrong: adding a single
user to HR would queue a rewrite of every HR document payload, making routine admin
operations O(chunks_in_scope) expensive. The correct cost is O(1) Redis key deletes.

### Invalidation table

| Event | Redis keys to delete | Qdrant resync? |
|-------|----------------------|----------------|
| Membership add/remove (principal=user) | `lr:eff:{company_id}:{user_id}:*` | No - access_scope_ids unchanged |
| Membership add/remove (principal=group) | `lr:eff:{company_id}:{user_id}:*` for every member of that group | No - access_scope_ids unchanged |
| Group member add/remove | `lr:eff:{company_id}:{user_id}:*` for the affected user | No - access_scope_ids unchanged |
| Resource grant create/revoke | `lr:eff:{company_id}:*` for affected principals | Yes - access_scope_ids of in-scope chunks change; enqueue `lr:resync:q` |
| Document owner_org_unit change | `lr:eff:{company_id}:*` for old and new org_unit | Yes - access_scope_ids recomputed for all chunks of this document |
| Document lifecycle change (activate/archive) | none | Yes - lifecycle_status field in Qdrant payload |
| Org_unit create | `lr:tree:{company_id}:{parent_id}` | No - no chunks yet |
| Org_unit move | `lr:tree:{company_id}:{old_parent}`, `lr:tree:{company_id}:{new_parent}`, `lr:eff:{company_id}:*` for affected scopes | Yes - descendants-grants now expand over a different subtree; enqueue `lr:resync:q` for moved subtree |
| Org_unit delete | `lr:tree:*`, `lr:eff:*` for company | Yes - chunks owned by deleted unit removed from Qdrant |
| User update | `lr:user:{user_id}` | No |
| API key revoke | `lr:api_key:{key_hash}` | No |
