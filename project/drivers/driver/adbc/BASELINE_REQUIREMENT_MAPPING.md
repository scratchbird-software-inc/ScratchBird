# Baseline Requirement Mapping (ADBC Host-Route Lane)

Scope: `project/drivers/driver/adbc` only.

This lane provides an ADBC route-runner package. It exposes source,
documentation, native tool entry points, and guarded runtime diagnostics. It
does not claim release support until a real ScratchBird ADBC runtime driver is
present and live conformance evidence is captured.

Status legend:
- `Route-runner`: source/tooling exists and fails closed without required runtime support.
- `Host-runtime`: executed through the Apache Arrow ADBC API when the runtime driver is available.
- `Not claimed`: release proof is intentionally absent.

## Baseline Groups

| Group | Status | Evidence anchors | Notes |
| --- | --- | --- | --- |
| `CONN` | Route-runner / Host-runtime | `src/scratchbird_adbc/contract_tool.py`; `tools/sb_isql_adbc`; `tests/test_adbc_contract_tool.py` | Builds ADBC connection options for direct listener and manager routes. Missing `adbc_driver_manager` or `SCRATCHBIRD_ADBC_DRIVER` fails closed. |
| `TXN` | Route-runner / Host-runtime | `src/scratchbird_adbc/contract_tool.py` | Commit and rollback are requested through the ADBC connection. MGA finality remains engine-owned. |
| `EXEC` | Route-runner / Host-runtime | `src/scratchbird_adbc/contract_tool.py` | Statements execute through ADBC DBAPI/driver-manager surfaces, not another ScratchBird lane. |
| `META` | Route-runner / Host-runtime | `src/scratchbird_adbc/contract_tool.py` | Metadata snapshots use `SHOW DATABASE` and `sys.*` queries through ADBC when available. |
| `TYPE` | Route-runner | `package_contract.json`; `README.md` | Arrow row/batch payloads are preserved as JSON-safe evidence. Full Arrow type proof is not claimed. |
| `ERR` | Route-runner | `src/scratchbird_adbc/contract_tool.py`; `tests/test_adbc_contract_tool.py` | Runtime dependency, route, parser, and execution failures produce `SB_DRIVER_ADBC_*` diagnostics. |
| `RES` | Route-runner | `src/scratchbird_adbc/contract_tool.py` | Writes matrix artifacts including command events, route environment, process metrics, result digests, metadata snapshots, native API coverage, JUnit, and logs. |

## Complete Coverage Gates

| Gate | Traceability | Current lane status |
| --- | --- | --- |
| `G1_MANIFEST` | `package_contract.json`; this mapping | Route-runner package manifest exists. |
| `G2_TRANSPORT` | `tools/sb_isql_adbc`; `route-environment.json` | Network routes are represented; unsupported routes fail closed. |
| `G3_AUTH_SECURITY` | ADBC connection options; diagnostics | Auth is passed to the host API; engine remains authority. |
| `G4_EXECUTION` | `AdbcStatement` execution path | Host-runtime only. |
| `G5_TYPES` | Arrow evidence surfaces | Route-runner evidence, no release proof. |
| `G6_TRANSACTIONS` | ADBC commit/rollback requests | Engine MGA owns finality. |
| `G7_METADATA` | metadata snapshot artifact | Host-runtime only. |
| `G8_RESULTS_STREAMING_BULK` | `ArrowArrayStream` trace token and result digests | Streaming proof not claimed. |
| `G9_DIAGNOSTICS` | `diagnostics.jsonl` | Implemented fail-closed. |
| `G10_CANCEL_TIMEOUT_POOLING` | timeout argument accepted | Cancellation/pooling proof not claimed. |
| `G11_LOCAL_SBSQL` | parser-mode fail-closed checks | Driver-local SBsql/SBLR is not trusted. |
| `G12_BIC_OVERLAY` | no overlay claim | Not claimed. |
| `G13_RELEASE_EVIDENCE` | summary/status artifacts | Not claimed for release. |
| `G14_FULL_SURFACE_CONFORMANCE` | native matrix CLI/artifacts | Route-runner shape present; live pass not claimed. |

Required range marker: `G1-G14`.

## Registry Sections

| Registry | Traceability | Current lane status |
| --- | --- | --- |
| `D1.*` | package and tool discovery | Route-runner package. |
| `D2.*` | connection establishment | Host-runtime through ADBC. |
| `D3.*` | authentication/bootstrap | Passed to engine through ADBC options. |
| `D4.*` | transport security | `sslmode` accepted and routed into URI/options. |
| `D5.*` | DSN parsing | CLI args form the ADBC URI. |
| `D6.*` | statement execution | Host-runtime through ADBC statements. |
| `D7.*` | parameter binding | Not claimed; scripts execute as top-level statements. |
| `D8.*` | result fetching/decoding | Host-runtime rows serialized into result digests. |
| `D9.*` | type system coverage | Route-runner Arrow evidence. |
| `D10.*` | transactions | Commit/rollback requested through ADBC. |
| `D11.*` | cancellation/timeouts | Timeout argument accepted; full cancel proof not claimed. |
| `D12.*` | diagnostics/errors/warnings | Fail-closed diagnostics implemented. |
| `D13.*` | catalog/metadata | Metadata snapshots attempted through host API. |
| `D14.*` | streaming/bulk transfer | Arrow stream token and artifact contract present; proof not claimed. |
| `D15.*` | connection pooling | Not claimed. |
| `D16.*` | async/reactive/pipeline | Not applicable to ADBC C API lane; no release claim. |
| `D17.*` | notifications/events | Not claimed. |
| `D18.*` | logging/telemetry/observability | Process metrics and transcripts written. |
| `D19.*` | cursor/portal advanced behavior | Not claimed. |
| `D20.*` | sharding/routing hints | Route args accepted; server authority preserved. |
| `D21.*` | wrapper/provider extensions | ADBC driver-manager options only. |
| `D22.*` | encoding/locale | language resource args recorded in artifacts. |
| `D23.*` | release-readiness evidence | Not claimed. |
| `D24.*` | ScratchBird-specific surfaces | MGA/server authority fields recorded. |
| `D25.*` | local SBsql-intelligence | Unsupported parser modes fail closed. |
| `D26.*` | commercial-grade release | Not claimed. |
| `D27.*` | enterprise release | Not claimed. |

Required range marker: `D1-D27`.

## Authority Notes

Driver-local parsing, UUID/SBLR hints, retries, replay, and transaction
finality are not authority in this lane. The engine must revalidate all work
and MGA transaction inventory remains finality authority.
