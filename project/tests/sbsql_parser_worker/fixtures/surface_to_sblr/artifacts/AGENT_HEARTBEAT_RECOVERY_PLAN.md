# Agent Heartbeat And Recovery Plan

Search key: `SBSQL-SURFACE-SBLR-AGENT-HEARTBEAT-RECOVERY`

## Purpose

Implementation is coordinated by one manager. Agents may work in parallel only when write scopes are disjoint and the tracker records the assigned slice.

## Heartbeat Rule

During implementation, every active agent must update `artifacts/AGENT_STATUS.csv` at least every five minutes with:

- assigned slice
- owned write scope
- current action
- last validation command
- current blocker or `none`
- next expected artifact

## Stall And Handoff Rule

If an agent misses a heartbeat, the coordinator must:

1. Mark the agent `stalled`.
2. Record incomplete files and last known validation state.
3. Add or update a failure inventory row.
4. Reassign only after confirming write-scope ownership.
5. Prevent the replacement agent from reverting unrelated edits.

## Tracker Rule

A slice cannot move from `pending` to `in_progress` unless owner, outputs, gates, write scope, and validation commands are recorded. A slice cannot move to `complete` unless validation has been rerun after the final edit.

## Five-Minute Refresh Rule

For long-running work, the coordinator refreshes status every five minutes and records one of:

- `active`
- `waiting_on_build`
- `waiting_on_test`
- `blocked`
- `stalled`
- `complete_pending_validation`
- `complete`
