-- V017: Prompt template registry for chat answer reproducibility
--
-- chat_turns.rag_sources pins the *evidence* shown to the LLM. The system
-- prompt that wrapped that evidence is the other half of reproducibility.
-- Without a versioned prompt template, "edit the system prompt to be more
-- concise" silently invalidates every past chat answer for audit purposes
-- ("show me the exact prompt and context that produced this answer about
-- leave policy") — table stakes for regulated buyers.
--
-- Designed to land before iteration 3 wires the chat path. The chat write
-- path needs to populate prompt_template_id at turn creation time; if the
-- column is missing, that signal is lost forever for turns that already ran.
--
-- Design choices:
--   * prompt_templates rows are IMMUTABLE on content/name/hash. Editing a
--     template creates a NEW row (new id). chat_turns.prompt_template_id
--     pins each turn to the exact content used at synthesis time.
--   * Single table (no document_versions-style two-level pattern): prompt
--     templates are low-cardinality (dozens per company at most). The
--     (name, created_at DESC) ordering recovers "latest version of this
--     template" without lifecycle complexity.
--   * Composite FK from chat_turns enforces the prompt template belongs
--     to the same company as the turn — defence in depth on the chat path.
--   * ON DELETE RESTRICT on chat_turns.prompt_template_id: a template
--     cited by historical chats cannot be hard-deleted. Soft retire by
--     no longer using it; the row stays.
--
-- chat_turns.prompt_template_id is NULLABLE so:
--   * existing rows pre-V016 don't need backfilling
--   * non-chat use cases (curated answers, tool-only turns) don't need
--     a template

CREATE TABLE prompt_templates (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id    UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    name          TEXT        NOT NULL,     -- stable label across versions of "the same" template
    content       TEXT        NOT NULL,     -- the full system prompt
    content_hash  TEXT        NOT NULL,     -- SHA-256 of content (caller computed)
    description   TEXT,                     -- mutable; not part of reproducibility contract
    created_by    UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Required so composite FKs can reference (company_id, id).
    UNIQUE (company_id, id),
    -- A given exact content within a (company, name) should not be inserted
    -- twice; callers reuse the existing row if the hash matches.
    UNIQUE (company_id, name, content_hash)
);

-- "Latest version of template X for company Y": ORDER BY created_at DESC LIMIT 1.
CREATE INDEX prompt_templates_company_name_idx
    ON prompt_templates (company_id, name, created_at DESC);

-- Validator: company-scoped actor (same pattern as memberships.granted_by, etc.)
CREATE OR REPLACE FUNCTION validate_prompt_templates_actors()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM validate_actor_same_company(NEW.created_by, NEW.company_id, 'created_by');
    RETURN NEW;
END;
$$;

CREATE TRIGGER prompt_templates_validate_actors
    BEFORE INSERT OR UPDATE ON prompt_templates
    FOR EACH ROW EXECUTE FUNCTION validate_prompt_templates_actors();

-- ---------------------------------------------------------------------------
-- Immutability of reproducibility-critical columns.
--
-- description and created_by remain mutable (description is for humans;
-- created_by may need SET NULL on user delete).
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_prompt_template_content_mutation()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.content      IS DISTINCT FROM NEW.content
       OR OLD.content_hash IS DISTINCT FROM NEW.content_hash
       OR OLD.name        IS DISTINCT FROM NEW.name
       OR OLD.company_id  IS DISTINCT FROM NEW.company_id
       OR OLD.created_at  IS DISTINCT FROM NEW.created_at THEN
        RAISE EXCEPTION 'prompt_templates content/name/hash/company are immutable; '
                        'create a new row instead';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER prompt_templates_immutable
    BEFORE UPDATE ON prompt_templates
    FOR EACH ROW EXECUTE FUNCTION prevent_prompt_template_content_mutation();

-- ---------------------------------------------------------------------------
-- chat_turns.prompt_template_id
-- ---------------------------------------------------------------------------

ALTER TABLE chat_turns
    ADD COLUMN prompt_template_id UUID;

ALTER TABLE chat_turns
    ADD CONSTRAINT chat_turns_prompt_template_same_company_fk
    FOREIGN KEY (company_id, prompt_template_id)
    REFERENCES prompt_templates(company_id, id) ON DELETE RESTRICT;

-- Reverse lookup: "which chat turns used template X?" (admin / quality audit)
CREATE INDEX chat_turns_prompt_template_idx
    ON chat_turns (company_id, prompt_template_id)
    WHERE prompt_template_id IS NOT NULL;
