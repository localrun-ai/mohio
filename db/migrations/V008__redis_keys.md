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
| `lr:eff:{company_id}:{user_id}:{org_unit_id}` | JSON array of UUID strings | 5 min | Resolved access_scope_ids for this user+scope; drives Qdrant filter |
| `lr:user:{user_id}` | JSON Identity record | 5 min | Profile + is_admin; invalidate on user update |
| `lr:tree:{company_id}:{org_unit_id}` | JSON subtree | 2 min | Admin UI tree; invalidate on any mutation under this node |

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

Membership / grant changes have two effects: a fast Redis invalidation and a slower
Qdrant payload resync. Both must happen.

| Event | Redis keys to delete | Qdrant resync? |
|-------|----------------------|----------------|
| Member add/remove/role-change | `lr:eff:{company_id}:{user_id}:*` | Yes - enqueue `lr:resync:q` |
| Resource grant create/revoke | `lr:eff:{company_id}:*:{org_unit_id}` for affected principals | Yes |
| Org_unit create/move | `lr:tree:{company_id}:{parent_id}` | Only if scope changed |
| Org_unit delete | `lr:tree:*`, `lr:eff:*` for company | Yes - chunks deleted from Qdrant |
| Document lifecycle change | - | Yes - chunk lifecycle_status in Qdrant payload |
| User update | `lr:user:{user_id}` | No |
| API key revoke | `lr:api_key:{key_hash}` | No |
