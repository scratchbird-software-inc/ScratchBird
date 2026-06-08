# Agent Orchestration Plan

Status: complete
Search key: `FSPE-AGENT-ORCHESTRATION-PLAN`
Updated: 2026-05-07 20:19:47 EDT

## Coordinator Rule

One coordinator manages the execution_plan. The coordinator owns slice ordering, assignment, monitoring, validation, correction loops, tracker updates, gate updates, and move eligibility.

## Execution Rule

Implementation slices execute one at a time unless the coordinator records a disjoint-write exception before work starts. Read-only inventory, audit preparation, and fixture planning may run in parallel only when their write scopes do not overlap implementation files.

## Queue And Write Scope

- `SLICE_EXECUTION_QUEUE.csv` contains one queue row for every tracker slice, including P0B-P0E and all P11-P14 sub-gates.
- `AGENT_WRITE_SCOPE_REGISTER.csv` defines role-level allowed and forbidden write scopes.
- If a file is needed by two roles, the coordinator serializes that file and records the ownership decision before edits begin.
- No worker may rewrite or revert another worker's edits.

## Monitoring Rule

Long-running agent work and validation jobs are checked every five minutes. The coordinator records progress, blocker, or failure state in the queue or failure inventory.

## Correction Rule

Validation failure pauses new slice starts. Corrections are assigned, validation reruns, and tracker/gate status changes only after validation passes.

## Infrastructure Error Rule

Out-of-space, permission denied, endpoint unavailable, stale socket, degraded device, timeout, OOM, corrupt build tree, or stale generated artifact events are recorded in `FAILURE_INVENTORY.csv`. Parser-visible or operator-visible events must also receive a message-vector backlog row before the owning slice can close.

## Status Vocabulary

Allowed slice states: `planned`, `ready_for_assignment`, `assigned`, `implementing`, `implementation_complete_pending_validation`, `validation_failed`, `correcting`, `validation_passed`, `tracker_updated`, `complete`, `blocked_by_predecessor`.

## P0A Result

P0A generated 44 queue rows, 16 write-scope role rows, 8 cadence rows, and an empty failure inventory with required columns.
