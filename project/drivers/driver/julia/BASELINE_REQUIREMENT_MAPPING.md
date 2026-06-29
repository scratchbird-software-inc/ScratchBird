# ScratchBird Julia Driver Baseline Requirement Mapping

Scope: lane-local S0 mapping for `project/drivers/driver/julia`.

Status key:

- `Route-runner source present`: public source/API shape exists in this lane.
- `Fail-closed`: the lane refuses unsupported runtime behavior with precise
  diagnostics instead of returning fabricated success.
- `Open`: full SBWP execution evidence is not present yet.

## MGA Recovery Contract

- ScratchBird MGA remains engine-owned.
- The Julia lane does not synthesize transaction finality locally.
- Reconnect can only repair transport/session setup; it never resurrects
  abandoned in-flight transactions or replays lost statements.
- Transaction helpers in `src/ScratchBird.jl` currently fail closed with
  `0A000` until the Julia SBWP transaction executor is present.
- Driver-local parsing, SBLR, UUID, and statement preparation remain untrusted
  client hints until server admission validates them.

## Baseline Groups

| Group | Current status | Lane-local anchors |
| --- | --- | --- |
| CONN | Route-runner source present, fail-closed where incomplete | `Project.toml`, `src/ScratchBird.jl` (`DBInterface.connect`, native TCP/local IPC setup), `tools/sb_isql_julia.jl` |
| TXN | Open, fail-closed | `src/ScratchBird.jl` (`begin_transaction!`, `commit!`, `rollback!`) |
| EXEC | Open, fail-closed | `src/ScratchBird.jl` (`DBInterface.prepare`, `DBInterface.execute`), `tools/sb_isql_julia.jl` |
| META | Open, fail-closed | `src/ScratchBird.jl` (`query_metadata`, sys catalog SQL map) |
| TYPE | Open | `Tables.rows`, `Tables.schema`, and result shape surfaces in `src/ScratchBird.jl` |
| ERR | Route-runner source present | `ScratchBirdError` with SQLSTATE-style messages, native tool diagnostics artifacts |
| RES | Route-runner source present | `Base.close(::ScratchBirdConnection)`, process metrics and artifact cleanup in native tool |

## G1-G14 Closure Traceability

| Gate | Julia lane status | Evidence / blocker |
| --- | --- | --- |
| G1_MANIFEST | Route-runner source present | `package_contract.json`, `Project.toml`, README |
| G2_TRANSPORT | Partial, fail-closed | native TCP/local IPC setup; TLS and embedded fail closed |
| G3_AUTH_SECURITY | Open | host/user/password/role surface exists; auth protocol execution requires server binding |
| G4_EXECUTION | Open, fail-closed | DBInterface prepare/execute API exists; SBWP execution refuses fake rows |
| G5_TYPES | Open | Tables-compatible result shape exists; full type matrix awaits codec evidence |
| G6_TRANSACTIONS | Open, fail-closed | transaction helpers refuse client-side finality |
| G7_METADATA | Open, fail-closed | metadata collection SQL map exists; execution requires the SBWP executor |
| G8_RESULTS_STREAMING_BULK | Open | no streaming/bulk SBWP evidence yet |
| G9_DIAGNOSTICS | Route-runner source present | diagnostics, error, summary, JUnit, and native API coverage artifacts |
| G10_CANCEL_TIMEOUT_POOLING | Open | CLI accepts timeout/concurrency args; runtime behavior awaits executor binding |
| G11_LOCAL_SBSQL | Open | no local SBsql intelligence accepted as authority |
| G12_BIC_OVERLAY | Open | no best-in-class overlay evidence yet |
| G13_RELEASE_EVIDENCE | Open | dependency and fail-closed tests only |
| G14_FULL_SURFACE_CONFORMANCE | Open | full live matrix cannot pass until SBWP execution lands |

## D1-D27 Registry Traceability

| Registry section | Julia lane status | Evidence / blocker |
| --- | --- | --- |
| D1 Driver discovery and registration | Route-runner source present | package layout and manifest contract |
| D2 Connection establishment | Partial | DBInterface connection and native transport setup |
| D3 Authentication and bootstrap | Open | credentials accepted, auth protocol execution requires server binding |
| D4 Transport security | Partial | TLS modes fail closed without a Julia TLS transport |
| D5 DSN and connection string parsing | Partial | keyword connection surface in package/tool |
| D6 Statement execution | Open, fail-closed | DBInterface execution refuses fake results |
| D7 Parameter binding | Open | API slot exists only through DBInterface call shape |
| D8 Result fetching and decoding | Open | Tables surface exists, no SBWP row decoder |
| D9 Type system coverage | Open | no complete type codec |
| D10 Transactions | Open, fail-closed | MGA finality remains engine-owned |
| D11 Cancellation, timeouts, and interrupts | Open | CLI args accepted; runtime path absent |
| D12 Diagnostics, errors, and warnings | Route-runner source present | diagnostics/error artifacts and `ScratchBirdError` |
| D13 Catalog and metadata access | Open, fail-closed | sys catalog SQL map only |
| D14 Streaming and bulk transfer | Open | no streaming/bulk implementation |
| D15 Connection pooling | Open | no pool implementation |
| D16 Async, reactive, and pipeline | Open | no async implementation |
| D17 Notifications and events | Open | no notification implementation |
| D18 Logging, telemetry, and observability | Route-runner source present | transcript, metrics, summary artifacts |
| D19 Cursor and portal advanced behavior | Open | no cursor/portal implementation |
| D20 Sharding and routing hints | Open | no sharding/routing-hint behavior |
| D21 Wrapper, escape, and provider extensions | Open | no wrapper/escape implementation |
| D22 Encoding and locale | Partial | language resource args captured in artifacts |
| D23 Driver release-readiness evidence | Open | syntax/dependency tests only |
| D24 ScratchBird-specific surfaces | Open | package names ScratchBird APIs; behavior incomplete |
| D25 Local SBsql intelligence | Open | local intelligence is not accepted as authority |
| D26 Commercial-grade driver release | Open | not release-ready |
| D27 Enterprise driver release | Open | not enterprise-ready |

Summary: this lane now closes the route-runner source/package gap for
G1-G14/D1-D27 traceability, but it remains planned/not-release-ready until the
Julia SBWP executor, auth, type, metadata, transaction, and live conformance
evidence are present.
