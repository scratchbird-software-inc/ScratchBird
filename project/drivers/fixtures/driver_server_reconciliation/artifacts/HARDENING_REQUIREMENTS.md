# Hardening Requirements

Search key: `DRIVER-SERVER-RECONCILIATION-HARDENING-REQUIREMENTS`.

## Authority Hardening

- The checklist registry is minimum driver authority. Every row must trace to
  canonical specs and implementation evidence.
- Execution_Plans and audits are evidence, not product behavior authority.
- Any implementation-visible driver behavior without canonical spec authority is
  drift and must be specified, guarded, or removed/refused.
- The manifest must list any new canonical spec, registry, or trace schema that
  this execution_plan introduces.

## Security Hardening

- Engine security is the only authentication and authorization authority.
- Drivers, listeners, managers, parsers, adapters, and tools may transport
  credentials or assertions but may not accept sessions by themselves.
- PEER/ident requires verified OS peer credentials. Absence of
  `SO_PEERCRED`, `getpeereid`, `ucred`, or equivalent verified evidence fails
  closed.
- Validator-only external auth providers must be marked with verifier boundary,
  support state, and production-admission status before any public driver can
  advertise them.
- Secret and sensitive connect keys must use redaction classes from
  `registries/connect-keys.yaml`.

## MGA Hardening

- Driver autocommit maps to engine-owned transaction boundaries.
- Reset, cancel, reconnect, pool return, dormant reattach, XA recovery, and
  timeout handling must cite MGA evidence before closure.
- A timeout is a client observation until the engine reports operation finality.
- No hidden retry is allowed unless the engine reports idempotency/finality that
  permits retry.
- WAL, donor engines, SQLite, parser state, or driver caches must not become
  recovery/finality authority.

## Wire Hardening

- Every client-visible frame requires a byte-level spec, versioning rule, invalid
  state behavior, and CTest evidence.
- Unknown connect keys are rejected unless registered aliases explicitly admit
  them.
- Multi-result, batch, array-bind, LOB, notification, reset, reauth, cancel, and
  trace context cannot ship as implicit JSON or ad hoc payloads.
- Client-visible drain/shutdown/close behavior must be structured diagnostics,
  not bare strings.
- Protocol version skew is a release gate. Old/new client/server pairings,
  unknown feature bits, extension negotiation, and downgrade refusal must be
  specified and tested.
- Unsupported, guarded, conditional, and not-applicable behavior must produce
  deterministic diagnostics. Silent success, fake support, and descriptive-only
  refusal are blockers.

## Driver And Adapter Hardening

- Every lane has a row-status YAML file using
  `public_contract_snapshot`.
- Every adapter proves it forwards DSN, auth, TLS, metadata, transaction,
  cancellation, telemetry, and lifecycle behavior through the underlying driver.
- `[~]` / implemented-without-evidence is a blocker.
- `[N/A]` is allowed only for conditional rows and only with runtime/API
  citation.
- Server-verification packets must be self-contained and reproducible.
- Driver/adaptor/tool package evidence must include install smoke tests and must
  prove build artifacts stay under the build or package output directories.
- Documentation samples must execute against a real route before release
  evidence can cite them.
- Donor compatibility evidence must use donor tools or compatibility drivers
  through the admitted ScratchBird client-to-engine route.

## Test Hardening

- CTest must include the checklist structure gate.
- Closure CTest must include per-lane driver gates, adapter gates, full-route
  gates, auth gates, MGA transaction gates, metadata gates, type round-trip
  gates, and benchmark route gates.
- Benchmark evidence is invalid if it bypasses the declared client/driver to
  SBWP/TLS or admitted IPC to listener/parser/server/engine route.
- Benchmark output is not sufficient by itself. Each claimed performance profile
  needs an explicit pass/fail budget and current baseline evidence.
- Fuzz and fault-injection tests must cover malformed frames, oversized payloads,
  duplicated connect keys, auth replay, TLS downgrade attempts, cancellation
  races, reset races, and pool-return races.
- Final release evidence must include a machine-readable declaration for every
  checklist row and lane using only supported, fail-closed, conditional N/A, or
  not implemented states.
