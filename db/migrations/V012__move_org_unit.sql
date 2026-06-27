-- V012: move_org_unit() and session-flag-aware parent-guard trigger
--
-- F6 (Opus RECOMMENDED): Enables safe subtree moves without globally disabling
-- the parent-update trigger.
--
-- Pattern:
--   1. prevent_org_unit_parent_update() is updated (CREATE OR REPLACE) to
--      allow parent updates when the session-local GUC
--      'wikore.allow_org_unit_move' is set to 'on'.
--   2. move_org_unit() is the ONLY function that sets this flag via
--      set_config(..., true) which is transaction-local and auto-clears.
--   3. No SECURITY DEFINER, no ALTER TABLE DISABLE TRIGGER. The guard is
--      narrow and safe under concurrency.
--
-- Closure surgery correctness:
--   The org_unit_closure table encodes all (ancestor, descendant, depth) pairs.
--   When node N moves from old_parent P to new_parent Q:
--     Step 1: Delete rows where ancestor is outside N's subtree but descendant
--             is inside N's subtree (removes all old ancestor-to-subtree links).
--     Step 2: Cross-join new ancestors of Q with all members of N's subtree to
--             produce the new ancestor-to-subtree links.
--     Step 3: Update parent_id under the session flag.
--   Internal links (N -> N's descendants) are untouched in both steps.

-- ---------------------------------------------------------------------------
-- Updated parent-guard trigger
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_org_unit_parent_update()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.parent_id IS DISTINCT FROM NEW.parent_id
       AND current_setting('wikore.allow_org_unit_move', true) IS DISTINCT FROM 'on'
    THEN
        RAISE EXCEPTION 'org_unit parent_id cannot be updated directly; use move_org_unit()';
    END IF;
    RETURN NEW;
END;
$$;
-- The trigger org_units_prevent_parent_update was created in V001 and remains;
-- CREATE OR REPLACE FUNCTION above updates the body in place.

-- ---------------------------------------------------------------------------
-- move_org_unit(p_company_id, p_node_id, p_new_parent_id)
--
-- Atomically moves an org unit to a new parent within the same company and
-- repairs the closure table. Parent_id update is gated by a transaction-local
-- session flag that only this function sets.
--
-- After this function returns successfully, the caller (C++) must:
--   - SMEMBERS+DEL lr:eff:keys:user:{cid}:{uid} for each user in the moved subtree
--   - DEL lr:tree:{cid}:{old_parent} and lr:tree:{cid}:{new_parent}
--   - Enqueue lr:resync:q for chunks in the subtree if inherited grants changed
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION move_org_unit(
    p_company_id    UUID,
    p_node_id       UUID,
    p_new_parent_id UUID
)
RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    v_type TEXT;
BEGIN
    -- Lock and validate the node being moved.
    SELECT type INTO v_type
    FROM org_units
    WHERE company_id = p_company_id AND id = p_node_id
    FOR UPDATE;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'org_unit % not found in company %', p_node_id, p_company_id;
    END IF;

    IF v_type = 'root' THEN
        RAISE EXCEPTION 'cannot move root org_unit (company_id=%)', p_company_id;
    END IF;

    -- Lock and validate the new parent. Same company_id enforces no cross-company move.
    PERFORM 1
    FROM org_units
    WHERE company_id = p_company_id AND id = p_new_parent_id
    FOR UPDATE;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'new parent % not found in company % '
            '(cross-company moves are not allowed)', p_new_parent_id, p_company_id;
    END IF;

    -- Reject moving under self.
    IF p_node_id = p_new_parent_id THEN
        RAISE EXCEPTION 'org_unit cannot be moved under itself (%)', p_node_id;
    END IF;

    -- Reject cycle: new parent must not be a descendant of the node.
    IF EXISTS (
        SELECT 1 FROM org_unit_closure
        WHERE company_id    = p_company_id
          AND ancestor_id   = p_node_id
          AND descendant_id = p_new_parent_id
          AND depth > 0
    ) THEN
        RAISE EXCEPTION 'cycle: % is a descendant of % and cannot become its parent',
            p_new_parent_id, p_node_id;
    END IF;

    -- ------------------------------------------------------------------
    -- Step 1: Delete old ancestor-to-subtree links.
    --
    -- Remove all rows where:
    --   descendant_id is inside the moved subtree (descendants of p_node_id)
    --   ancestor_id   is outside the moved subtree (not a descendant of p_node_id)
    --
    -- This preserves internal links (node to its own descendants) unchanged.
    -- ------------------------------------------------------------------
    DELETE FROM org_unit_closure
    WHERE company_id = p_company_id
      AND descendant_id IN (
          SELECT descendant_id
          FROM org_unit_closure
          WHERE company_id  = p_company_id
            AND ancestor_id = p_node_id
      )
      AND ancestor_id NOT IN (
          SELECT descendant_id
          FROM org_unit_closure
          WHERE company_id  = p_company_id
            AND ancestor_id = p_node_id
      );

    -- ------------------------------------------------------------------
    -- Step 2: Insert new ancestor-to-subtree links.
    --
    -- For every ancestor A of p_new_parent_id (including itself at depth 0)
    -- and every descendant D of p_node_id (including itself at depth 0):
    --
    --   depth(A -> D) = depth(A -> new_parent) + depth(node -> D) + 1
    --
    -- The +1 accounts for the edge from new_parent to p_node_id.
    -- ------------------------------------------------------------------
    INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth)
    SELECT
        p_company_id,
        a.ancestor_id,
        d.descendant_id,
        a.depth + d.depth + 1
    FROM org_unit_closure a
    CROSS JOIN org_unit_closure d
    WHERE a.company_id    = p_company_id
      AND a.descendant_id = p_new_parent_id
      AND d.company_id    = p_company_id
      AND d.ancestor_id   = p_node_id;

    -- ------------------------------------------------------------------
    -- Step 3: Update parent_id under the session-local flag.
    --
    -- set_config with is_local=true scopes the flag to the current
    -- transaction; it reverts automatically on commit or rollback.
    -- The trigger prevent_org_unit_parent_update checks this flag.
    -- ------------------------------------------------------------------
    PERFORM set_config('wikore.allow_org_unit_move', 'on', true);
    UPDATE org_units
    SET parent_id = p_new_parent_id
    WHERE company_id = p_company_id AND id = p_node_id;
END;
$$;
