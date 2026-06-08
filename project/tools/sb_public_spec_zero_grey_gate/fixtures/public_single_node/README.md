# Public Single-Node Parser/Storage/Datatype/Security/Wire/Donor Closure Execution_Plan

Status: completed
Created: 2026-05-10
Owner: ScratchBird public closure coordinator
Search key: `PUBLIC-SINGLE-NODE-PARSER-STORAGE-DATATYPE-SECURITY-WIRE-DONOR-CLOSURE`

## Purpose

Close the next large public zero-grey target set after
`public-release-foundation-closure`. This package combines the remaining
parser/profile drift, parser-to-SBLR closure, storage/page/filespace lifecycle,
datatype and index execution, security/auth/audit, wire/driver operations, and
public donor compatibility rows into one agent-managed implementation plan.

The plan intentionally uses the recommended defaults:

- Target the listed gaps only, not every remaining public gap in the registry.
- Include all open public donor-family rows as dependent later slices.
- Require original donor regression/tool evidence where a donor source tree or
  donor toolchain is available locally.
- Include all driver/adaptor/tool lanes in the wire/driver closure.
- Treat cloud storage as real provider-interface plus local emulator gates until
  real credentials are supplied.
- Use the same autonomous agent-manager execution model as the previous closure
  plan, with five-minute heartbeat updates.

## Target Scope

This execution_plan targets 110 registry rows:

| Group | Gaps | Intent |
| --- | --- | --- |
| Parser V3 and parser profile closure | `SB-PUBLIC-GAP-0040` through `SB-PUBLIC-GAP-0060` | Close SBLR operation/lowering/runtime matrix, native SBSQL dialect, parser AST/CST/bound-AST, profile drift, management syntax, diagnostics, ingress security, UDR bridge, meta-commands, and parser conformance. |
| Storage/page/filespace lifecycle closure | `SB-PUBLIC-GAP-0021`, `0022`, `0023`, `0024`, `0033`, `0034`, `0036`, `0037` | Close storage architecture, page layouts, datatype physical storage, cloud-provider interface/local emulator, database lifecycle, quarantine/fence/release, and storage metrics. |
| Datatype and index execution closure | `SB-PUBLIC-GAP-0145` through `SB-PUBLIC-GAP-0158` | Close commercial-grade datatype behavior, 128-bit numeric backend, casts/comparisons, domain methods, datatype catalog/metrics, index catalog DDL, index family matrix, index diagnostics, insert/update write profiles, and index metrics. |
| Security/auth/audit closure | `SB-PUBLIC-GAP-0061` through `SB-PUBLIC-GAP-0068` | Close local authentication, authorization, audit, encryption at rest, policy enforcement, auth plugin manifests, and regression guard the already-closed TLS row `0066`. |
| Wire/driver operational closure | `SB-PUBLIC-GAP-0069` through `SB-PUBLIC-GAP-0087` | Close server/listener lifecycle, SBCT, parser pool, SBWP, local IPC, driver package manifests, native/ODBC/JDBC/language/tool/adaptor lanes, routing, reconnect finality, and idempotency. |
| Donor compatibility closure | Open public donor rows in `SB-PUBLIC-GAP-0089` through `SB-PUBLIC-GAP-0128` | Close donor catalog seeds, datatype gates, index translation, diagnostics, metadata overlays, migration/CDC, sandbox bridge, wire sessions, parser-family inventory, built-in surfaces, release acquisition, all public donor family profiles, and capability-reference refusal surfaces. |

The exact target rows are recorded in `artifacts/TARGET_GAPS.csv`.

## Explicit Non-Targets

The recommended default is listed-gap scope only. Public rows outside this
execution_plan remain open until a later execution_plan targets them. Examples include
architecture leftovers, memory/cache, operations, agents, local metrics roots,
non-listed cloud operations, and implementation trace/buildability rows not
named by this package.

## Non-Negotiable Rules

- No placeholder, stub, fake pass, or deferral may close a target row.
- The engine remains SBLR-only; parsers lower dialect input to SBLR and UUID
  operations.
- MGA remains transaction/recovery/finality authority. WAL, donor engines,
  hidden SQLite, parser state, CRUD text streams, or driver behavior may not
  become finality authority.
- Donor emulation must not introduce cross-dialect parser dependencies. Code may
  be shared only within the same dialect parser/UDR package.
- Driver/adaptor/tool behavior must route through declared wire/IPC/protocol
  paths and engine-owned authentication/authorization/transaction authority.
- Donor original regression evidence must use donor tools only as compatibility
  inputs/oracles, never as ScratchBird storage/finality backends.
- Cloud-provider closure must provide real provider interfaces and local
  emulator conformance gates; real credentials are optional deployment inputs,
  not a reason to skip API/contract implementation.
- Every target row must have a CTest label and final evidence before its
  registry state changes to `implemented_in_full`.
- Agents must update `artifacts/AGENT_STATUS.csv` at least every five minutes
  during long-running implementation.

## Implementation Order

```text
P0  Target registry, hardening, reusable zero-grey gates, AI-budget resume plan
P1  Parser V3 drift, profile authority, and parser-to-SBLR substrate
P2  Storage/page/filespace lifecycle and cloud-provider/local-emulator substrate
P3  Datatype and index execution substrate
P4  Security/auth/audit substrate
P5  Wire, listener, server, driver, adaptor, and tool operational substrate
P6  Donor core framework and donor-family implementation batches
P7  Full-route regression, donor original regression, target zero-grey closure
```

## Required Hardening Artifacts

Implementation must not start until these artifacts exist:

- `artifacts/TARGET_GAPS.csv`
- `artifacts/TARGET_EVIDENCE_MANIFEST.csv`
- `artifacts/AGENT_WRITE_SCOPE_MATRIX.csv`
- `artifacts/AGENT_STATUS.csv`
- `artifacts/AI_BUDGET_CONTINGENCY.md`
- `artifacts/HARDENING_REQUIREMENTS.md`
- `artifacts/PARSER_PROFILE_CLOSURE_MODEL.md`
- `artifacts/STORAGE_FORMAT_AND_PROVIDER_POLICY.md`
- `artifacts/DATATYPE_INDEX_EXECUTION_CLOSURE_MODEL.md`
- `artifacts/SECURITY_AUTH_AUDIT_CLOSURE_MODEL.md`
- `artifacts/WIRE_DRIVER_OPERATIONAL_CLOSURE_MODEL.md`
- `artifacts/DONOR_REGRESSION_POLICY.md`
- `artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md`

## Agent Execution Protocol

When implementation begins, the coordinator manages agents by disjoint write
scope:

- `release_gate_agent`: target registry, reusable target zero-grey tooling,
  evidence manifests, CTest label gates, final audit tooling.
- `parser_agent`: parser V3 grammar, profile authority, AST/CST/bound-AST,
  management syntax, diagnostics, ingress security, parser conformance.
- `sblr_udr_agent`: SBLR operation/lowering/runtime matrix, SBLR code
  generation, trusted C++ UDR bridge and admission tests.
- `storage_agent`: page layout, filespace lifecycle, cloud provider interface,
  local emulator, quarantine/fence/release, database lifecycle.
- `datatype_index_agent`: datatype closure, 128-bit numeric, casts/comparisons,
  domains, index catalog DDL, index families, insert/update write profiles.
- `security_agent`: local auth, authorization, audit, encryption-at-rest,
  policy, auth plugin manifest.
- `wire_driver_agent`: server/listener lifecycle, SBCT, parser pool, SBWP, local
  IPC, driver/adaptor/tool lanes, route finality.
- `donor_core_agent`: donor catalog seeds, datatype gates, diagnostics,
  metadata overlay, sandbox bridge, donor release/regression framework.
- `donor_family_agents`: family batches after parser/profile and donor-core
  gates pass.
- `verification_agent`: CTest sweeps, donor original regression, benchmark/full
  route, inventory update, registry synchronization.

Agents are not alone in the codebase. They must not revert unrelated edits and
must adapt to changes made by other agents. Each agent must record heartbeat and
evidence updates in `artifacts/AGENT_STATUS.csv`.

## Definition Of Done

This execution_plan is complete only when:

- Every target row in `artifacts/TARGET_GAPS.csv` is implemented in full or, for
  already closed `SB-PUBLIC-GAP-0066`, has a passing regression guard.
- `TARGET_EVIDENCE_MANIFEST.csv` has final evidence for every target row.
- The reusable target zero-grey gate passes for this execution_plan.
- Full SBSQL route tests pass over client/driver or tool, SBWP/TLS, listener,
  parser pool, parser, IPC, server, engine security, SBLR execution, MGA
  transactions, and response return path.
- Original donor regression/tool evidence passes for every donor family where
  local donor source/tools are available; unavailable donor toolchains must fail
  as environment-missing evidence, not as implementation closure.
- Driver/adaptor/tool lanes are integrated into CTest and rerun as regression.
- The full public zero-grey gate may fail only for non-target public rows, and a
  non-target regression audit proves no target or previously closed row regressed.
- `public_audit_summary`,
  `public_audit_summary`, and the CSV
  registry are synchronized.
