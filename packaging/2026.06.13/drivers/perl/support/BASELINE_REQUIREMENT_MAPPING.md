# ScratchBird Perl DBI Driver Baseline Requirement Mapping

Scope: lane-local S0 mapping for `project/drivers/driver/perl`.

Status key:

- `Route-runner source present`: public source/API shape exists in this lane.
- `Fail-closed`: the lane refuses unsupported runtime behavior with precise
  diagnostics instead of returning fabricated success.
- `Open`: full SBWP execution evidence is not present yet.

## MGA Recovery Contract

- ScratchBird MGA remains engine-owned.
- The Perl DBI lane does not synthesize transaction finality locally.
- Reconnect can only repair transport/session setup; it never resurrects
  abandoned in-flight transactions or replays lost statements.
- `commit`, `rollback`, and explicit `BEGIN` paths currently fail closed until
  the Perl SBWP transaction executor is present.
- Driver-local parsing, SBLR, UUID, and statement preparation remain untrusted
  client hints until server admission validates them.

## Baseline Groups

| Group | Current status | Lane-local anchors |
| --- | --- | --- |
| CONN | Route-runner source present, fail-closed where incomplete | `Makefile.PL`, `lib/DBD/ScratchBird.pm` (`DBI->connect`, native TCP/local IPC setup), `tools/sb_isql_perl.pl` |
| TXN | Open, fail-closed | `DBD::ScratchBird::db::commit`, `rollback`, native tool transaction handling |
| EXEC | Open, fail-closed | `prepare`, `execute`, `fetchrow_arrayref` in `lib/DBD/ScratchBird.pm` and native tool |
| META | Open, fail-closed | `table_info`, `column_info` metadata query surfaces |
| TYPE | Open | no complete type codec yet |
| ERR | Route-runner source present | SQLSTATE-style errors, `errstr`, diagnostics artifacts |
| RES | Route-runner source present | `disconnect`, process metrics, artifact cleanup in native tool |

## G1-G14 Closure Traceability

| Gate | Perl lane status | Evidence / blocker |
| --- | --- | --- |
| G1_MANIFEST | Route-runner source present | `package_contract.json`, `Makefile.PL`, README |
| G2_TRANSPORT | Partial, fail-closed | native TCP/local IPC setup; TLS dependency and embedded fail closed |
| G3_AUTH_SECURITY | Open | user/password/role surface exists; auth protocol execution requires server binding |
| G4_EXECUTION | Open, fail-closed | DBI prepare/execute API exists; SBWP execution refuses fake rows |
| G5_TYPES | Open | no complete type mapping or decoder |
| G6_TRANSACTIONS | Open, fail-closed | transaction APIs refuse client-side finality |
| G7_METADATA | Open, fail-closed | `table_info`/`column_info` metadata query surfaces |
| G8_RESULTS_STREAMING_BULK | Open | no streaming/bulk SBWP evidence yet |
| G9_DIAGNOSTICS | Route-runner source present | diagnostics, error, summary, JUnit, and native API coverage artifacts |
| G10_CANCEL_TIMEOUT_POOLING | Open | CLI accepts timeout/concurrency args; runtime behavior awaits executor binding |
| G11_LOCAL_SBSQL | Open | no local SBsql intelligence accepted as authority |
| G12_BIC_OVERLAY | Open | no best-in-class overlay evidence yet |
| G13_RELEASE_EVIDENCE | Open | syntax/dependency tests only |
| G14_FULL_SURFACE_CONFORMANCE | Open | full live matrix cannot pass until SBWP execution lands |

## D1-D27 Registry Traceability

| Registry section | Perl lane status | Evidence / blocker |
| --- | --- | --- |
| D1 Driver discovery and registration | Route-runner source present | `DBD::ScratchBird` package layout and manifest contract |
| D2 Connection establishment | Partial | DBI connection and native transport setup |
| D3 Authentication and bootstrap | Open | credentials accepted, auth protocol execution requires server binding |
| D4 Transport security | Partial | TLS requires `IO::Socket::SSL`; missing support fails closed |
| D5 DSN and connection string parsing | Partial | semicolon DBI DSN parser |
| D6 Statement execution | Open, fail-closed | `execute` refuses fake results |
| D7 Parameter binding | Open | bind values accepted by call shape only |
| D8 Result fetching and decoding | Open | `fetchrow_arrayref` exists, no SBWP row decoder |
| D9 Type system coverage | Open | no complete type codec |
| D10 Transactions | Open, fail-closed | MGA finality remains engine-owned |
| D11 Cancellation, timeouts, and interrupts | Open | CLI args accepted; runtime path absent |
| D12 Diagnostics, errors, and warnings | Route-runner source present | `errstr`, SQLSTATE-style diagnostics artifacts |
| D13 Catalog and metadata access | Open, fail-closed | `table_info`/`column_info` metadata query surfaces |
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
Perl SBWP executor, auth, type, metadata, transaction, and live conformance
evidence are present.
