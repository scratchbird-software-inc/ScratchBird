# Transactions And Recovery

This page is part of the SBsql Language Reference Manual. It explains the ScratchBird transaction and recovery model at a conceptual level. Statement syntax is documented in [../syntax_reference/transaction_control.md](../syntax_reference/transaction_control.md).

Generation task: `core_paradigms_transactions_and_recovery`

## Purpose

ScratchBird transaction authority is MGA-native. The engine owns transaction identity, snapshots, row-version visibility, commit finality, rollback behavior, cleanup horizons, and recovery classification. SBsql can request transaction actions and inspect admitted state, but SQL text and client-side state are not finality authority.

## Core Invariants

| Invariant | Meaning |
| --- | --- |
| Engine-owned identity | Every active transaction has an engine-allocated transaction UUID and local transaction number. |
| Snapshot before work | A transaction snapshot is constructed before user-visible work begins. |
| Versions, not overwrite | Writes create or retire row versions. They do not make older committed versions disappear for existing snapshots. |
| Inventory finality | Commit and rollback state are published through durable MGA transaction inventory. |
| Security recheck | Visibility is the intersection of MGA visibility and security/materialized policy. |
| Indexes are evidence | Index entries accelerate candidates. Final row authority requires MGA, predicate, descriptor, and security recheck. |
| Recovery before work | Recovery-required state fences ordinary work until classification is complete. |

## Transaction Lifecycle

```mermaid
stateDiagram-v2
  [*] --> admitted: begin request
  admitted --> active: identity + snapshot allocated
  active --> committing: commit requested
  active --> rolling_back: rollback requested
  active --> active: savepoint / rollback to savepoint
  committing --> committed: durable finality accepted
  committing --> recovery_required: uncertain evidence
  rolling_back --> rolled_back: rollback evidence accepted
  rolling_back --> recovery_required: uncertain evidence
  committed --> [*]
  rolled_back --> [*]
  recovery_required --> committed: recovery classifies commit
  recovery_required --> rolled_back: recovery classifies rollback
  recovery_required --> fail_closed: operator or policy decision required
```

## Snapshot Contents

A transaction snapshot includes:

- visible-through transaction boundary;
- active transaction exclusions;
- cleanup and retention horizons;
- catalog epoch;
- security epoch;
- policy epoch;
- isolation profile;
- attached database/workarea root;
- effective principal and role context;
- resource and filespace readiness state.

The exact encoded snapshot is engine-owned. The user-facing rule is that visibility must be reproducible for the isolation profile and must fail closed when the required snapshot evidence is missing.

## Commit And Rollback

Commit publishes new visible state only when the durable transaction inventory and required sync/fence policy accept the commit. Rollback makes the transaction's versions non-visible and releases, compensates, or retires transaction-owned resources.

Commit and rollback are not parser decisions. A parser, driver, bridge, or client can request finality, but the engine decides and records it.

## Autocommit

Autocommit is an execution profile:

1. open a transaction;
2. execute one admitted statement or statement group;
3. commit on success;
4. rollback on failure;
5. report finality evidence or a diagnostic.

Autocommit does not weaken transaction guarantees and does not create a second authority path.

## Savepoints

Savepoints are rollback markers inside one transaction. They let a script undo part of a transaction while keeping the transaction active. They do not have independent commit authority, independent snapshots, or independent recovery finality.

## Isolation

ScratchBird public isolation profiles are documented in [Transaction Control](../syntax_reference/transaction_control.md). At a high level:

- `READ COMMITTED` can see newer committed data at statement boundaries.
- `SNAPSHOT` uses a stable transaction snapshot.
- `SNAPSHOT TABLE STABILITY` adds admitted table-stability behavior.
- `SERIALIZABLE` requires conflict detection or prevention for the admitted operation set.

All profiles still require MGA row-version visibility, page/filespace validity, and security policy recheck.

## Recovery Classification

On startup or reopen after interruption, the engine classifies transaction inventory before ordinary work resumes.

| Evidence State | Required Outcome |
| --- | --- |
| Committed evidence complete | Transaction remains committed. |
| Rolled-back evidence complete | Transaction remains rolled back. |
| Active without finality | Transaction is rolled back or classified according to recovery policy. |
| Commit in progress | Recovery completes commit only if durable evidence proves it. |
| Rollback in progress | Recovery completes rollback or fences if uncertain. |
| Prepared or limbo | Recovery waits for a valid local decision, policy decision, or operator action. |
| Inconsistent evidence | Fail closed with recovery-required state. |

Silent inconsistency is not an allowed outcome.

## Bridge And Remote Transactions

Bridge operations may create a local transaction and one or more remote transactions. Each participating database keeps its own transaction authority. A local commit cannot assert remote finality, and a remote commit cannot assert local finality. Cross-node or distributed finality requires explicit policy-owned routes and must preserve local MGA rules.

## Diagnostics And Inspection

Authorized inspection surfaces can expose:

- transaction UUID;
- local transaction number;
- state;
- isolation profile;
- snapshot boundary;
- active savepoints;
- lock/resource wait state;
- recovery-required state;
- commit/rollback evidence status.

Inspection is policy controlled and may redact details.

## Failure Principle

When transaction outcome is uncertain, ScratchBird must return an explicit diagnostic, fence unsafe work, and require recovery or operator/policy decision. It must not infer finality from SQL text, client retry behavior, timestamps, UUID order, parser state, or ordinary log messages.
