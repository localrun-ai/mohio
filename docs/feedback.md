Yes, I think this is a **very strong idea**, but I would sharpen the framing.

You are not really building "a wiki plus chatbot". You are building:

> **An access-controlled enterprise knowledge RAG platform where every answer is evidence-gated by org structure, permissions, document lifecycle, and retrieval quality.**

That is much more interesting than another AnythingLLM clone.

The Reddit thread is basically a mini customer-discovery signal: they have 3,000+ internal docs, need self-hosting, found AnythingLLM inaccurate, care about different workspaces or tenancies, and specifically say departments like HR, sales, support, IT, and marketing must not see each other's information. They also say SSO/RBAC are crucial, not optional. 

## My honest take

I would build it, but **not as a full enterprise intranet first**.

The valuable part is not the org tree UI. The valuable part is this:

1. Users authenticate through SSO.
2. Their org memberships and grants are resolved.
3. Retrieval is filtered **before** any chunk reaches the LLM.
4. Reranking happens only on allowed evidence.
5. The answer cites only allowed evidence.
6. Wiki pages are generated or edited only from allowed sources.

That is the killer feature.

The admin tree is useful, but it is supporting infrastructure. The core product is **permission-safe RAG**.

## Important architecture correction

I would **not** start with "Qdrant collection per org unit".

That maps nicely from Astraea's jurisdiction model, but enterprise org units are much more dynamic. A company can easily end up with hundreds of departments, projects, working groups, temporary teams, confidential initiatives, and cross-functional squads.

Qdrant's own multitenancy guidance says creating hundreds or thousands of collections is not recommended because it increases overhead and can hurt performance or stability. They recommend, for most cases, using a **single collection per embedding model** with payload-based tenant partitioning and filters. ([Qdrant][1])

So instead of this:

```text
collection: ou_hr
collection: ou_payroll
collection: ou_rnd
collection: ou_marketing
```

I would prefer this:

```text
collection: enterprise_docs_bge_m3

payload:
  company_id
  org_unit_ids
  owner_org_unit_id
  access_scope_ids
  doc_id
  version_id
  lifecycle_status
  authority_level
  created_at
  updated_at
```

Then every retrieval query includes a hard Qdrant filter:

```text
company_id == user's company
AND access_scope_ids intersects user's resolved access scopes
AND lifecycle_status in ["active", "approved"]
```

Qdrant supports payload metadata and filtering, including boolean-style filter clauses and payload indexes, so this fits the system very naturally. ([Qdrant][2])

Use separate collections only when you need **hard isolation**, for example different customers, different embedding models, legal separation, or very large tenants.

## Better mental model

I would split the framework into four layers:

```text
1. Identity layer
   SSO, JWT validation, user identity, groups, external IdP mapping

2. Authorization layer
   Org tree, memberships, explicit grants, inherited grants, resource ACLs

3. Evidence layer
   Documents, chunks, versions, lifecycle, authority, ownership, source traceability

4. Reasoning layer
   Retrieval, reranking, answer generation, wiki generation, citations
```

Astraea already helps most with layers 3 and 4.

Your new product idea adds layers 1 and 2.

That is the actual architectural jump.

## SSO/RBAC: use external IdP, do not build it

I agree with the Reddit answer: do not implement SAML/OIDC yourself.

Use Keycloak, Authentik, Entra ID, Okta, Google Workspace, etc. Your app should validate JWTs and map claims to internal users, groups, and org units.

Keycloak supports OpenID Connect and SAML identity providers, and can broker identity from external providers, so it is a reasonable self-hosted choice. ([Keycloak][3])

The product should own this part:

```text
external_group -> internal_org_unit
external_role  -> internal_role
user           -> explicit grants
```

Not this:

```text
password storage
MFA implementation
SAML parser
OIDC protocol implementation
```

## Access model

I would keep the ACL model simple and strict.

```text
org_units
  id
  parent_id
  company_id
  type: company | subsidiary | division | department | team | project
  name

users
  id
  external_subject
  email

groups
  id
  external_group_id
  name

memberships
  principal_type: user | group
  principal_id
  org_unit_id
  role: viewer | editor | admin

resource_grants
  resource_type: org_unit | document | wiki_page | collection
  resource_id
  principal_type: user | group | org_unit
  principal_id
  permission: read | write | admin
  inherited: true | false
```

Default should be **closed**, not open.

So HR docs are visible to HR because HR has read access. R&D does not see them unless there is an explicit grant.

For inheritance, I would avoid vague rules. Make it explicit:

```text
grant applies_to:
  self_only
  self_and_descendants
```

That avoids surprises like "Company X read access means everyone can read everything".

## Query flow

Your query pipeline should be something like this:

```text
1. Decode JWT.
2. Resolve user, groups, org memberships.
3. Compute allowed access_scope_ids.
4. Build mandatory retrieval filter.
5. Retrieve candidate chunks from Qdrant.
6. Rerank allowed chunks only.
7. Build prompt from allowed chunks only.
8. Generate answer.
9. Return answer with citations and access-safe source list.
10. Audit log: user, query, chunk IDs, document IDs, permission basis.
```

The non-negotiable part:

```text
Unauthorized chunks must never enter the LLM context.
```

Not hidden in UI. Not removed after generation. Not "the model probably will not mention it".

Never retrieved, never reranked, never prompted.

That is exactly where your tenancy.localrun.ai experience matters.

## Wiki part

The wiki should not be a separate product at first. It should be a **projection of the evidence layer**.

For example:

```text
Policy documents -> generated wiki page
SOP folder -> generated team handbook
Ticket history -> generated troubleshooting guide
Architecture docs -> generated system overview
```

But every generated wiki page needs provenance:

```text
This page was generated from:
- doc A version 3
- doc B version 7
- SOP C approved on date X
```

And the wiki page itself must have ACLs.

The wiki is not just editable Markdown. It is an **LLM-maintained knowledge surface over controlled evidence**.

That is much more powerful.

## Existing tools already cover parts of this

Open WebUI now has RBAC concepts such as roles, permissions, groups, and resource access control for knowledge bases, models, and tools. Its docs describe additive permissions and read/write grants for resources. ([Open WebUI][4])

Open WebUI also supports OIDC SSO through environment configuration, with one OIDC provider configured via `OPENID_PROVIDER_URL`. ([Open WebUI][5])

So your framework should not compete by saying "we also have chat and docs".

It should compete by saying:

> Existing tools give you an AI UI. We give you auditable, permission-safe, metadata-gated enterprise retrieval.

That is a much sharper positioning.

## MVP I would build

Do **not** start with subsidiaries, divisions, SCIM, complex admin UI, wiki generation, and multi-company enterprise SaaS.

Start with this:

```text
MVP 1:
- Single company
- Departments and teams only
- Local users or OIDC/JWT auth
- Admin can create org units
- Admin can assign users/groups to org units
- Documents belong to org units
- Qdrant payload-filtered retrieval
- Reranker
- Chat with citations
- Audit log of retrieved chunks
```

That is already enough to prove the core.

Then:

```text
MVP 2:
- Wiki pages generated from approved docs
- Document lifecycle: draft, active, deprecated, archived
- Authority weighting
- Explicit cross-department sharing
- Access preview: "what can this user see?"
```

Then:

```text
MVP 3:
- Multiple companies
- Subsidiaries/divisions
- SCIM provisioning
- Advanced admin UI
- Approval workflow
- Connectors: Google Drive, SharePoint, Git, Confluence, Jira
```

## The strongest product angle

The strongest pitch is not:

> Self-hosted AI wiki.

It is:

> Self-hosted enterprise RAG with strict pre-retrieval access control, reranked evidence, auditable citations, and org-aware knowledge segmentation.

That is a real pain point. The Reddit thread shows exactly that: self-hosting alone is not enough, and generic RAG accuracy is not enough when departments have different access boundaries. 

## My suggested name for the concept

Maybe:

```text
Astraea Enterprise
```

or more generally:

```text
Astraea Knowledge Fabric
```

The core abstraction:

```text
Jurisdiction -> Knowledge Scope
Legal authority -> Document authority
Court/case metadata -> Enterprise metadata
Citation correctness -> Evidence correctness
Statutory boundary -> Access boundary
```

That mapping is actually very clean.

## Final verdict

Yes, this is worth pursuing.

But the winning version is not a generic intranet. The winning version is:

```text
A self-hosted, org-aware, permission-safe RAG and wiki framework for enterprise knowledge.
```

And the key engineering decision I would make early is:

```text
Use PostgreSQL as the source of truth for org/access/document metadata.
Use Qdrant payload filters for access-safe retrieval.
Use reranking only after permission filtering.
Use LLMs only after evidence gating.
```

That would make it a natural evolution from tenancy.localrun.ai instead of a totally unrelated product.

[1]: https://qdrant.tech/documentation/tutorials/multiple-partitions/?utm_source=chatgpt.com "Multitenancy - Qdrant"
[2]: https://qdrant.tech/documentation/concepts/payload/?utm_source=chatgpt.com "Payload - Qdrant"
[3]: https://www.keycloak.org/docs/latest/server_admin/?utm_source=chatgpt.com "Server Administration Guide"
[4]: https://docs.openwebui.com/features/authentication-access/rbac/?utm_source=chatgpt.com "Role-Based Access Control (RBAC) / Open WebUI"
[5]: https://docs.openwebui.com/features/authentication-access/auth/sso/?utm_source=chatgpt.com "SSO (OAuth, OIDC, Trusted Header) / Open WebUI"
