# Driver/Server Contract and Implementation Reconciliation Closure Execution_Plan

Status: active
Created: 2026-05-10
Owner: ScratchBird driver/server reconciliation coordinator
Search key: `DRIVER-SERVER-SPEC-IMPLEMENTATION-RECONCILIATION-CLOSURE`

## Purpose

Correct the two driver release blockers identified by:

- `public_audit_summary`
- `public_audit_summary`
- `public_audit_summary`

The normalized driver/adaptor checklist is now the minimum project driver
requirement set through:

- `public_contract_snapshot`
- `public_contract_snapshot`

This execution_plan closes the remaining gap between that minimum set, the canonical
project contracts, the current implementation, and the CTest regression
suite.

## Non-Negotiable Rules

- Contract authority comes first. Implementation-only behavior cannot be
  treated as a driver contract.
- Every checklist row must have canonical spec authority, implementation status,
  test evidence, and closure state.
- Implementation-ahead features must be classified as `accepted_and_specified`,
  `guarded_until_specified`, or `removed_or_refused`.
- Security-sensitive auth features fail closed until the spec and implementation
  both prove the authority path.
- The engine remains the only authentication, authorization, transaction, and
  finality authority.
- MGA remains the transaction/recovery model. Drivers, listeners, parsers,
  reference tools, retry code, and hidden storage layers must not create alternate
  transaction truth.
- Driver routes must exercise the real route for the lane being claimed:
  client/driver/tool, SBWP/TLS or admitted IPC, listener/manager as configured,
  parser pool, parser, IPC, server, engine security, SBLR execution, MGA
  response return path.
- No placeholder, fake pass, silent skip, or undocumented implementation may
  close a row.

## Target Scope

The target scope is every row in
`artifacts/TARGET_CHECKLIST_ROWS.csv`, currently 331 stable checklist rows:

| Area | Rows | Intent |
| --- | --- | --- |
| Driver discovery/connection/auth/TLS/DSN | `D1.*` through `D5.*` | Driver package registration, DSN/config, connect, auth, TLS, manager-proxy, session identity, and connect-time options. |
| Execution/parameters/results/types/transactions/cancel/diagnostics/metadata | `D6.*` through `D13.*` | Prepare/bind/execute, multiple results, output params, batch, array bind, LOB, type encoding, MGA transactions, cancel, diagnostics, and metadata. |
| Streaming/pooling/async/notifications/telemetry/cursors/routing/extensions/locale/evidence | `D14.*` through `D24.*` | Bulk transfer, LOB streaming, pooling, async, events, OTel, cursor behavior, sharding/routing hints, provider extensions, locale/timezone, release evidence, ScratchBird-specific surfaces. |
| Adapter closure | `A1.*` through `A6.*` | Adapter forwarding, schema, query/DML, diagnostics, lifecycle, packaging, and live-server evidence. |

## Implementation Order

```text
P0  Checklist promotion, drift freeze, security fail-closed hotfix, tracking gates
P1  Canonical contract closure for driver, wire, auth, datatype, metadata, transaction, and diagnostics authority
P2  Implementation reconciliation for implementation-ahead features and missing server-dependent capabilities
P3  Driver/adaptor/tool lane reconciliation against the normalized registry
P4  CTest, lane-native, server-verification, benchmark, and full-route evidence closure
P5  Inventory, gap registry, release declaration, and final audit synchronization
```

## Required Execution_Plan Artifacts

Implementation must not continue beyond P0 until these artifacts exist:

- `artifacts/TARGET_CHECKLIST_ROWS.csv`
- `artifacts/TARGET_EVIDENCE_MANIFEST.csv`
- `artifacts/IMPLEMENTATION_AHEAD_CLASSIFICATION.csv`
- `artifacts/AGENT_WRITE_SCOPE_MATRIX.csv`
- `artifacts/AGENT_STATUS.csv`
- `artifacts/AI_BUDGET_CONTINGENCY.md`
- `artifacts/HARDENING_REQUIREMENTS.md`
- `artifacts/SPEC_AUTHORITY_CLOSURE_MODEL.md`
- `artifacts/WIRE_SESSION_SPEC_CLOSURE_MODEL.md`
- `artifacts/SECURITY_AUTH_RECONCILIATION_MODEL.md`
- `artifacts/DATATYPE_RESULT_METADATA_CLOSURE_MODEL.md`
- `artifacts/DRIVER_LANE_EVIDENCE_MODEL.md`
- `artifacts/FULL_ROUTE_DRIVER_ACCEPTANCE_FIXTURE.md`
- `artifacts/PROTOCOL_VERSION_SKEW_MATRIX.md`
- `artifacts/DETERMINISTIC_REFUSAL_MATRIX.md`
- `artifacts/FUZZ_FAULT_INJECTION_MATRIX.md`
- `artifacts/PACKAGING_DISTRIBUTION_EVIDENCE_MODEL.md`
- `artifacts/PERFORMANCE_BUDGET_GATE.md`
- `artifacts/DOCUMENTATION_SAMPLE_APP_GATE.md`
- `artifacts/REFERENCE_DRIVER_COMPATIBILITY_ROUTE_GATE.md`
- `artifacts/RELEASE_DECLARATION_GENERATOR_MODEL.md`
- `artifacts/P0_EXECUTION_PLAN_CREATION_EVIDENCE.md`

The `driver_server_reconciliation_hardening_gate` CTest gate verifies this
artifact set.

## Required Slice Families

| Family | Required closure |
| --- | --- |
| Driver core spec | Checklist registry, lane status YAML contract, baseline union override, evidence contract, release readiness, BIC comparison. |
| Native wire/session | Startup key/value registry, server info, parameter status, ping/pong, disconnect, reset session, reauth/token refresh, cancel request, state notifications, timeouts, keepalive, failover hints, trace context. |
| Manager/listener | MCP_HELLO extensions, DB_CONNECT constants, direct-native bypass behavior, pool drain/kill/refusal, parser pool and client route state. |
| Auth/security | Auth method/provider registries for all implemented and required families, production status flags, verifier-boundary status, PEER/ident OS credential evidence, MFA, token rotation, channel binding, LDAP/Kerberos/RADIUS/OIDC/SAML/WebAuthn/SPIFFE policy. |
| Parameter/result/type | ParameterDataPacket, ParameterDescription, RowDescription metadata bitmap, CanonicalTypeId wire byte layout stitching, null vs empty, precision/scale, generated keys discriminator, OUT/INOUT return path. |
| Execution/cursor/stream | MultiResultEnvelope, batch/pipeline, array bind, LOB locator/chunk protocol, cursor holdability/sensitivity/concurrency, copy/bulk per-row reject events, bounded streaming/backpressure. |
| Diagnostics/cancel/observability | CancelOutcomeToSqlstate, timeout/cancel/network distinctions, MessageVector mapping, W3C traceparent/tracestate, parameter redaction map, localized messages. |
| MGA/transactions | Autocommit mapping, immediate transaction boundary reopen, no silent retry, savepoints, 2PC/XA, limbo/prepared transaction recovery, dormant detach/reattach finality query. |
| Metadata/catalog | `sys.information` projections, ODBC/JDBC/.NET metadata mapping, metadata flags, grant-visible views, monitoring views, type/function/reserved-word lists. |
| Driver/adaptor implementation | Every lane and adapter has source status, package manifest, status YAML, CTest labels, native route tests, server verification packet, and release evidence. |
| Protocol version/skew | Old client/new server, new client/old server, unknown feature bits, extension negotiation, downgrade refusal, and exact version diagnostics. |
| Deterministic refusal | Unsupported, guarded, conditional, or not-applicable rows have exact SQLSTATE, message-vector, retryability, and finality behavior. |
| Fuzz/fault injection | Malformed frames, duplicate keys, oversized payloads, auth replay, TLS downgrade, stream corruption, cancel/reset races, and pool-return races fail closed. |
| Packaging/release evidence | Driver/adaptor/tool packages prove metadata, versioning, signing/SBOM where applicable, install smoke tests, and build-artifact isolation. |
| Performance budgets | Real-route latency, throughput, TLS overhead, prepared execution, bulk transfer, and allocation regression budgets are explicit and enforced. |
| Documentation/sample apps | Driver DSN, auth/TLS, diagnostics, transactions, metadata, and adapter sample applications are executable and cite the verified route. |
| Reference compatibility route | Reference compatibility drivers and tools prove behavior through the admitted driver/wire/listener/parser/server/engine path, not parser-only shortcuts. |
| Release declaration | Machine-readable release declaration reports every checklist row and lane as supported, fail-closed, conditional N/A, or not implemented. |

## Agent Execution Model

When implementation is run under agent management, use disjoint write ownership:

| Agent | Ownership |
| --- | --- |
| `spec_registry_agent` | Driver checklist registry, MANIFEST/AUTHORITY, spec indexes, row-status schema. |
| `wire_spec_agent` | Native wire, SBWP, SBPS, local IPC, manager/listener protocol specs. |
| `security_agent` | Auth method/provider registries, PEER fail-closed, token refresh, policy/redaction/channel-binding specs and code. |
| `wire_impl_agent` | Native wire/session/cancel/reset/ping/multi-result/bulk/LOB/batch/array-bind implementation. |
| `datatype_metadata_agent` | Type wire encoding, RowDescription/ParameterDescription, metadata flags, catalog projections. |
| `driver_lane_agent` | Driver/adaptor/tool lane manifests, row status YAML, package/tests, CTest integration. |
| `verification_agent` | CTest, server-verification packets, full-route route tests, benchmark evidence, final inventory sync. |

Agents must update `artifacts/AGENT_STATUS.csv` at least every five minutes
during long-running execution and must not revert unrelated work.

## Definition Of Done

This execution_plan is complete only when:

- Every row in `artifacts/TARGET_CHECKLIST_ROWS.csv` is
  `implemented_and_proven` or, for conditional rows only,
  `not_applicable_with_citation`.
- No row remains `server_unspecified`, `undocumented_implementation`,
  `implemented_without_evidence`, or `not_started`.
- Every implementation-ahead audit item is back-ported to spec or guarded from
  public driver paths.
- PEER/ident authentication is fail-closed unless verified OS peer credentials
  are present.
- The full CTest suite includes the driver/adaptor/tool gates and fails on any
  required row without evidence.
- Server-verification packets and lane-native tests pass for every claimed lane.
- Protocol version skew and unsupported-feature refusal behavior are specified
  and proven by negative route tests.
- Driver/adaptor/tool package and documentation samples are reproducible from a
  clean checkout and do not write artifacts outside the build or package output
  directories.
- Reference compatibility evidence uses the same admitted route as public clients.
- Performance gates have explicit pass/fail budgets instead of descriptive
  benchmark output only.
- `public_audit_summary`, the public
  zero-grey registry, and this execution_plan tracker are synchronized.
