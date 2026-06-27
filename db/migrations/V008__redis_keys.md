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

Two distinct change types have different downstream effects. Do not conflate them.

### Membership changes vs group member changes

**Membership changes** (`memberships` table: a principal gains/loses access to an
org_unit) change which org_units' documents a user may retrieve. This alters
`document.access_scope_ids` stored in Qdrant chunk payloads. Both Redis invalidation
AND Qdrant payload resync are required.

**Group member changes** (`group_members` table: a user is added to or removed from
a group) change the user's resolved scope set at query time. They do NOT change
`document.access_scope_ids` directly - document scopes depend on which org_units
have grants/memberships, not on group composition per se. Only Redis invalidation
is required; Qdrant payloads are unchanged.

If a group itself holds a membership (principal_type='group'), then adding a user to
that group expands their effective scopes. The `lr:eff` cache for that user must be
dropped so it is recomputed on next query. No Qdrant resync is needed because the
chunk payloads already include the org_unit in their access_scope_ids.

### Invalidation table

| Event | Redis keys to delete | Qdrant resync? |
|-------|----------------------|----------------|
| Membership add/remove/role-change | `lr:eff:{company_id}:{user_id}:*` for each affected user | Yes - enqueue `lr:resync:q` for affected org_unit |
| Group member add/remove | `lr:eff:{company_id}:{user_id}:*` for the affected user only | No - document scopes unchanged |
| Resource grant create/revoke | `lr:eff:{company_id}:*:{org_unit_id}` for affected principals | Yes - enqueue `lr:resync:q` |
| Org_unit create/move | `lr:tree:{company_id}:{parent_id}` | Only if scope changed |
| Org_unit delete | `lr:tree:*`, `lr:eff:*` for company | Yes - chunks deleted from Qdrant |
| Document lifecycle change | - | Yes - chunk lifecycle_status in Qdrant payload |
| User update | `lr:user:{user_id}` | No |
| API key revoke | `lr:api_key:{key_hash}` | No |
