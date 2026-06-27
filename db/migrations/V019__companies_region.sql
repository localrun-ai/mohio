-- V019: companies.region for multi-region / data residency
--
-- Single nullable-with-default column added to companies. By itself this
-- changes no behavior; the value is recorded so future region-aware
-- adapters (Qdrant collection sharding, Postgres replica routing) can
-- read it without a schema migration when the first regional customer
-- signs.
--
-- Why now: a new customer asking "our data must stay in the EU" cannot
-- be answered honestly if the schema has no concept of region. Adding
-- the column later is an ALTER on a possibly-large table plus backfilling
-- every existing customer with 'default'. Adding it now is free.
--
-- 'default' is the placeholder for single-region deployments; switch the
-- value via UPDATE when a regional adapter is wired in. Possible future
-- values: 'eu-west', 'us-east', 'apac-southeast', etc. Not constrained
-- to an enum so customers / forks can pick their own naming.

ALTER TABLE companies
    ADD COLUMN region TEXT NOT NULL DEFAULT 'default';

-- Tenant-by-region admin queries: "list all EU customers."
CREATE INDEX companies_region_idx
    ON companies (region, id);
