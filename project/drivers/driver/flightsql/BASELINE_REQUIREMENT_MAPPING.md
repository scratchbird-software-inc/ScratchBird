# Baseline Requirement Mapping (FlightSQL Host-Route Lane)

Scope: `project/drivers/driver/flightsql` only.

This lane provides a Flight SQL route-runner package. It exposes source,
documentation, a native tool, and guarded runtime diagnostics. It
does not claim release support until a real ScratchBird Flight SQL endpoint and
live conformance evidence are available.

Status legend:
- `Route-runner`: source/tooling exists and fails closed without required runtime support.
- `Host-runtime`: executed through Apache Arrow Flight SQL when available.
- `Not claimed`: release proof is intentionally absent.

## Baseline Groups

| Group | Status | Evidence anchors | Notes |
| --- | --- | --- | --- |
| `CONN` | Route-runner / Host-runtime | `src/scratchbird_flightsql/contract_tool.py`; `tools/sb_isql_flightsql`; `tests/test_flightsql_contract_tool.py` | Opens `FlightSqlClient` against network routes. |
| `TXN` | Route-runner | `src/scratchbird_flightsql/contract_tool.py` | Transaction commands execute only through Flight SQL statements; MGA finality remains engine-owned. |
| `EXEC` | Route-runner / Host-runtime | `src/scratchbird_flightsql/contract_tool.py` | Uses `FlightSqlClient`, prepared/execution, `DoGet`, and Arrow row materialization where available. |
| `META` | Route-runner / Host-runtime | `src/scratchbird_flightsql/contract_tool.py` | `SHOW DATABASE` and metadata snapshots are attempted through Flight SQL. |
| `TYPE` | Route-runner | `package_contract.json`; `README.md` | Arrow table rows are serialized into digest evidence. Full type proof is not claimed. |
| `ERR` | Route-runner | `src/scratchbird_flightsql/contract_tool.py`; `tests/test_flightsql_contract_tool.py` | Runtime dependency, route, parser, and execution failures produce `SB_DRIVER_FLIGHTSQL_*` diagnostics. |
| `RES` | Route-runner | `src/scratchbird_flightsql/contract_tool.py` | Writes matrix artifacts including command events, route environment, process metrics, result digests, metadata snapshots, native API coverage, JUnit, and logs. |

## Complete Coverage Gates

| Gate | Traceability | Current lane status |
| --- | --- | --- |
| `G1_MANIFEST` | `package_contract.json`; this mapping | Route-runner package manifest exists. |
| `G2_TRANSPORT` | `tools/sb_isql_flightsql`; route artifacts | Network Flight SQL routes represented; embedded/ipc fail closed. |
| `G3_AUTH_SECURITY` | Flight SQL handshake/options | Auth is passed to the endpoint; engine remains authority. |
| `G4_EXECUTION` | Flight SQL execute/DoGet path | Host-runtime only. |
| `G5_TYPES` | Arrow evidence surfaces | Route-runner evidence, no release proof. |
| `G6_TRANSACTIONS` | statement-level transaction requests | Engine MGA owns finality. |
| `G7_METADATA` | metadata snapshot artifact | Host-runtime only. |
| `G8_RESULTS_STREAMING_BULK` | Arrow result stream handling | Streaming proof not claimed. |
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
| `D2.*` | connection establishment | Host-runtime through Flight SQL. |
| `D3.*` | authentication/bootstrap | Passed to server through Flight SQL handshake/options. |
| `D4.*` | transport security | `sslmode` selects TLS or plaintext gRPC location. |
| `D5.*` | DSN parsing | CLI args form the Flight SQL location. |
| `D6.*` | statement execution | Host-runtime through Flight SQL statements. |
| `D7.*` | parameter binding | Prepared statement proof not claimed. |
| `D8.*` | result fetching/decoding | Host-runtime Arrow rows serialized into result digests. |
| `D9.*` | type system coverage | Route-runner Arrow evidence. |
| `D10.*` | transactions | Transaction statements use engine authority. |
| `D11.*` | cancellation/timeouts | Timeout argument accepted; full cancel proof not claimed. |
| `D12.*` | diagnostics/errors/warnings | Fail-closed diagnostics implemented. |
| `D13.*` | catalog/metadata access | Metadata snapshots attempted through host API. |
| `D14.*` | streaming/bulk transfer | Flight streams and Arrow artifacts represented; proof not claimed. |
| `D15.*` | connection pooling | Not claimed. |
| `D16.*` | async/reactive/pipeline | Not claimed. |
| `D17.*` | notifications/events | Not claimed. |
| `D18.*` | logging/telemetry/observability | Process metrics and transcripts written. |
| `D19.*` | cursor/portal advanced behavior | Not claimed. |
| `D20.*` | sharding/routing hints | Route args accepted; server authority preserved. |
| `D21.*` | wrapper/provider extensions | Flight SQL provider options only. |
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
