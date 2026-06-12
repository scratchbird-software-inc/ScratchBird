# Firebird Agent Implementation Orchestration Policy

Status: pending
Search key: `FIREBIRD_AGENT_IMPLEMENTATION_ORCHESTRATION_POLICY`

## Purpose

Define how the FirebirdSQL parser, parser-support UDR, emulation, reference-native regression, and hardening work is executed by agents under a single agent manager.

The policy is an execution-control requirement. It does not change Firebird product behavior, ScratchBird core authority, parser/UDR trust boundaries, or CTest acceptance criteria.

## Manager Responsibility

The agent manager owns:

- Maintaining `AGENT_EXECUTION_STATUS.csv`.
- Assigning each active slice to one agent role with explicit file/module ownership.
- Keeping write scopes disjoint when parallel agents are active.
- Reviewing completed agent patches before integration.
- Running or scheduling the narrowest relevant tests after each integrated patch.
- Starting the next ready slice when no active blocker prevents progress.
- Escalating only when a human decision is required.

## Five-Minute Refresh Rule

While implementation agents are active, the manager must perform a status refresh at least every five minutes.

Each refresh records:

- Active agent ids or role labels.
- Slice id and owned file/module scope.
- Current work state.
- Last agent output or patch evidence.
- Last integrated commit/diff evidence when available.
- Last test gate and result.
- Next test gate.
- Blocker state.
- Next action.

If the manager cannot refresh within the five-minute interval due to a long-running command or test, the command/test log path becomes the refresh evidence and the next refresh happens immediately after control returns.

## Agent Assignment Rules

- Assign concrete, bounded slices only.
- Give every agent explicit ownership of files, modules, generated artifacts, or test lanes.
- Tell every implementation agent it is not alone in the codebase and must not revert unrelated edits.
- Assign disjoint write scopes to parallel agents.
- Prefer code-change workers for implementation slices and explorer agents for bounded codebase questions.
- Keep critical-path blocking work local when waiting on an agent would stall progress.
- Integrate completed work before assigning dependent slices.

## Test Cadence

After each integrated code or generated-artifact patch, run the narrowest relevant gate from `FIREBIRD_CTEST_REQUIRED_GATES.csv`.

Minimum cadence:

- Parser grammar/lexer changes run lexer and CST/AST gates.
- Binder/descriptor changes run binder and relevant datatype or DML gates.
- UDR ABI changes run dynamic SQL or environment installer gates.
- Bridge/wire changes run bridge and wire API gates.
- Reference-native harness changes run reference tool build, sandbox, normalization, or replay gates.
- Diagnostics changes run status-vector diagnostic gates.
- Hardening changes run package boundary, runtime absence, malicious input fuzz, or tool isolation gates.
- Registry or matrix changes run surface registry lint and final affected matrix lint.

Broad CTest lanes run at milestone boundaries and before final closure.

## Continuation Rule

Once implementation starts, the manager continues assigning, integrating, refreshing, and testing until:

- Every slice reaches its acceptance gate and the final zero-open audit passes.
- A human decision is required.

Human decision cases are limited to:

- Authority conflict between canonical contracts.
- Missing reference regression source that cannot be acquired from local evidence.
- Destructive operation outside approved boundaries.
- Security or trust-boundary contradiction.
- Contract requirement that cannot be implemented without changing ScratchBird architecture.
- External dependency decision that affects product packaging or licensing.

## Escalation Packet

Every escalation must include:

- Slice id.
- Exact decision requested.
- Evidence path.
- Current status row from `AGENT_EXECUTION_STATUS.csv`.
- Tests already run.
- Alternatives attempted.
- Risk of continuing without human input.

## Required Closure

`firebird_agent_orchestration_gate` passes only when:

- Every active or completed slice has a current status row.
- No active slice has a stale refresh timestamp.
- Every completed implementation slice has test evidence.
- Every integrated patch has review evidence.
- Every blocker row has an escalation packet or next autonomous action.
- Final closure has no active blockers, no unintegrated patches, and no untested completed slices.
