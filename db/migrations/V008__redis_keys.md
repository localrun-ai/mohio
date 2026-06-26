# Redis Key Reference

All keys are prefixed with `lr:` (localrun namespace).

## Auth

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:jwt:{jti}` | `"ok"` | until JWT exp | Valid JWT fast-path; set on first verify |
| `lr:revoked:{jti}` | `"1"` | until JWT exp | Revoked JWTs (logout / forced revoke) |
| `lr:apikey:{key_hash_prefix8}` | JSON {org_id, user_id, role} | 10 min | API key fast-path; invalidate on revoke |

## Access resolution

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:eff:{user_id}:{org_id}` | JSON array of org_id strings | 5 min | Effective org IDs for user querying org; invalidate on membership/grant change |
| `lr:user:{user_id}` | JSON user record | 5 min | Profile + is_admin; invalidate on user update |
| `lr:tree:{org_id}` | JSON subtree | 2 min | For admin UI; invalidate on any org mutation under this node |

## Rate limiting

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:rl:chat:{user_id}` | counter | 60 s | Sliding window; max N chat requests/min |
| `lr:rl:ingest:{org_id}` | counter | 60 s | Ingest rate per org |

## LLM concurrency (Lua semaphore, same pattern as Astraea)

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:llm:sem` | integer | - | Global in-flight LLM requests; capped by config |
| `lr:llm:sem:{org_id}` | integer | - | Per-org cap (optional; prevents one org starving others) |

## Ingest queue

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:ingest:q:{org_id}` | List of doc_id | - | Serializes ingests per org; pop = claim |
| `lr:ingest:lock:{doc_id}` | worker_id | 10 min | Distributed lock; prevents duplicate processing |

## MCP tool cache (read-only tools only)

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:tool:{tool_name}:{sha256(args)}` | JSON result | 5 min | Cache Jira search, Confluence page, etc. Write tools never cached |
| `lr:oauth:{integration_id}` | access_token | until exp | OAuth token cache; refresh before expiry |

## Invalidation rules

- Membership add/remove/change -> delete `lr:eff:{user_id}:*` for affected user
- Org create/move/delete -> delete `lr:tree:{parent_id}`, `lr:eff:*:{org_id}`
- Access grant/revoke -> delete `lr:eff:*` for all members of grantee_org
- User update -> delete `lr:user:{user_id}`
- API key revoke -> delete `lr:apikey:{prefix}`
