# Baseline Requirement Mapping (R2DBC Host-Route Lane)

Scope: `project/drivers/driver/r2dbc` only.

This lane provides an R2DBC route-runner package. It exposes source,
documentation, Java SPI anchors, a native tool, and guarded runtime
diagnostics. It does not claim release support until a real ScratchBird R2DBC
provider and live conformance evidence are available.

Status legend:
- `Route-runner`: source/tooling exists and fails closed without required runtime support.
- `Host-runtime`: executed through the Java R2DBC SPI when available.
- `Not claimed`: release proof is intentionally absent.

## Baseline Groups

| Group | Status | Evidence anchors | Notes |
| --- | --- | --- | --- |
| `CONN` | Route-runner / Host-runtime | `src/scratchbird_r2dbc/contract_tool.py`; `src/main/java/com/scratchbird/r2dbc/ScratchBirdR2dbcContractSurface.java`; `tools/sb_isql_r2dbc`; `tests/test_r2dbc_contract_tool.py` | Builds R2DBC `ConnectionFactoryOptions` for network routes. |
| `TXN` | Route-runner / Host-runtime | `src/scratchbird_r2dbc/contract_tool.py`; Java source anchors | Transaction commands execute through R2DBC statements; MGA finality remains engine-owned. |
| `EXEC` | Route-runner / Host-runtime | `src/scratchbird_r2dbc/contract_tool.py` | Uses `ConnectionFactory`, `Connection`, `Statement`, `Publisher`, and `Result` where available. |
| `META` | Route-runner / Host-runtime | `src/scratchbird_r2dbc/contract_tool.py` | `SHOW DATABASE` and metadata snapshots are attempted through R2DBC. |
| `TYPE` | Route-runner | `package_contract.json`; `README.md` | R2DBC row values are serialized into digest evidence. Full type proof is not claimed. |
| `ERR` | Route-runner | `src/scratchbird_r2dbc/contract_tool.py`; `tests/test_r2dbc_contract_tool.py` | Runtime dependency, route, parser, and execution failures produce `SB_DRIVER_R2DBC_*` diagnostics. |
| `RES` | Route-runner | `src/scratchbird_r2dbc/contract_tool.py` | Writes matrix artifacts including command events, route environment, process metrics, result digests, metadata snapshots, native API coverage, JUnit, and logs. |

## Complete Coverage Gates

| Gate | Traceability | Current lane status |
| --- | --- | --- |
| `G1_MANIFEST` | `package_contract.json`; this mapping | Route-runner package manifest exists. |
| `G2_TRANSPORT` | `tools/sb_isql_r2dbc`; route artifacts | Network routes represented; embedded/ipc fail closed. |
| `G3_AUTH_SECURITY` | R2DBC connection options | Auth is passed to the provider; engine remains authority. |
| `G4_EXECUTION` | R2DBC statement path | Host-runtime only. |
| `G5_TYPES` | row evidence surfaces | Route-runner evidence, no release proof. |
| `G6_TRANSACTIONS` | R2DBC transaction statements/control | Engine MGA owns finality. |
| `G7_METADATA` | metadata snapshot artifact | Host-runtime only. |
| `G8_RESULTS_STREAMING_BULK` | Publisher/Result handling | Streaming proof not claimed. |
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
| `D2.*` | connection establishment | Host-runtime through R2DBC. |
| `D3.*` | authentication/bootstrap | Passed to server through provider options. |
| `D4.*` | transport security | `sslmode` accepted in options. |
| `D5.*` | DSN parsing | CLI args form `ConnectionFactoryOptions`. |
| `D6.*` | statement execution | Host-runtime through R2DBC `Statement`. |
| `D7.*` | parameter binding | Not claimed. |
| `D8.*` | result fetching/decoding | Host-runtime rows serialized into result digests. |
| `D9.*` | type system coverage | Route-runner row evidence. |
| `D10.*` | transactions | Transaction statements/control use engine authority. |
| `D11.*` | cancellation/timeouts | Timeout argument accepted; full cancel proof not claimed. |
| `D12.*` | diagnostics/errors/warnings | Fail-closed diagnostics implemented. |
| `D13.*` | catalog/metadata access | Metadata snapshots attempted through host API. |
| `D14.*` | streaming/bulk transfer | Publisher/Result artifacts represented; proof not claimed. |
| `D15.*` | connection pooling | Not claimed. |
| `D16.*` | async/reactive/pipeline | R2DBC reactive surface represented; release proof not claimed. |
| `D17.*` | notifications/events | Not claimed. |
| `D18.*` | logging/telemetry/observability | Process metrics and transcripts written. |
| `D19.*` | cursor/portal advanced behavior | Not claimed. |
| `D20.*` | sharding/routing hints | Route args accepted; server authority preserved. |
| `D21.*` | wrapper/provider extensions | R2DBC provider options only. |
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
