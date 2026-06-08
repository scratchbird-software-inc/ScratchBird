# Database Lifecycle Agent Orchestration Policy

Status: pending
Search key: `DATABASE-LIFECYCLE-AGENT-ORCHESTRATION-POLICY`

This policy controls implementation once the full database lifecycle execution_plan is started. The coordinator is responsible for keeping agent-managed implementation moving until the execution_plan is complete or a blocker requires a human decision.

## Coordinator Duties

- Maintain `DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv` as the live queue for every `TRACKER.csv` slice.
- Assign agents by disjoint write scope and record every assignment in `DATABASE_LIFECYCLE_AGENT_STATUS.csv`.
- Register every writable file or file group in `DATABASE_LIFECYCLE_AGENT_WRITE_SCOPE_REGISTER.csv` before an agent starts work.
- Check active agents at least every five minutes and append each check to `DATABASE_LIFECYCLE_AGENT_HEARTBEAT_LOG.csv`.
- Integrate completed agent work, review diffs, run required validation, and update `TRACKER.csv`, `ACCEPTANCE_GATES.csv`, and evidence paths only after validation passes.
- Record failures, failed gates, stale agents, blocked agents, and human-decision blockers in `DATABASE_LIFECYCLE_AGENT_FAILURE_INVENTORY.csv`.

## Agent Assignment Rules

- Agents are the default implementation mechanism for non-overlapping slices and non-overlapping file groups.
- The coordinator may implement or correct work locally only when the work is on the critical path, when assigning it would overlap active write scopes, or when an integration failure requires immediate local repair.
- No two agents may write the same source, spec, registry, generated artifact, build rule, or test file group at the same time.
- Agents must be told they are not alone in the codebase, must not revert unrelated edits, and must adapt to existing or concurrently integrated changes.
- Agents own their assigned write scope until the coordinator marks the scope released.

## Five-Minute Heartbeat

During active implementation or long validation:

- The coordinator checks every active agent at least once every five minutes.
- Each heartbeat records UTC timestamp, active agent id/name, slice id, current state, last observed output, evidence path, blocker state, and next coordinator action.
- If an agent is stale for two heartbeat periods, the coordinator records the stale state, either reassigns the work or continues locally, and preserves the stale evidence.
- Heartbeats continue until no agents and no long validations are active.

## Validation And Correction Loop

Slice closure requires:

- implementation integrated into the main workspace;
- required CTest labels, static gates, generated conformance checks, or audit scripts completed;
- complete failure inventory preserved for full/gate/soak runs;
- targeted correction for failures followed by rerun of the failed gate;
- `TRACKER.csv` status updated;
- `ACCEPTANCE_GATES.csv` status updated when a gate closes;
- execution queue, agent status, write-scope register, heartbeat log, and evidence paths updated.

Full, gate, or soak validation must collect all useful failures before debugging. It must not stop on first failure unless the failure prevents useful evidence collection for the rest of the run.

## Stop Conditions

The coordinator stops unattended execution only when:

- a required architectural or authority decision is missing;
- credentials or external access are required;
- a destructive action requires approval;
- a dependency cannot be acquired or built without human input;
- source changes conflict in a way that risks losing user work;
- a canonical spec contradiction cannot be resolved from existing authority.

Every stop condition must be recorded in the failure inventory and current agent status before escalation.
