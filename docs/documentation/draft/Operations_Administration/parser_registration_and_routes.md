# Parser Registration And Routes

## Purpose

ScratchBird separates language parsing from query execution. A parser is an isolated process that translates a client's SQL dialect into an internal representation called SBLR (ScratchBird Logical Representation). The engine then independently verifies, secures, and executes that representation. This separation means the engine never trusts parser output alone — authorization and transaction management always run inside the engine regardless of which parser generated the request.

This chapter explains how parser packages are registered, how the listener manages parser worker processes, how route selection works, and what operators see when a parser is missing, quarantined, or incompatible.

## Parser Package Concepts

A **parser package** is a versioned, self-contained binary plus a configuration file that together define one supported SQL dialect. Each package carries:

- A `parser_uuid` — a stable identifier for this particular package.
- A `dialect` string — the name of the SQL dialect.
- A `build_id` — the specific build of this package.
- A `protocol_version` — the IPC protocol version the parser speaks.
- A `metrics_schema_version` — the version of the metrics schema the parser publishes.

These fields are exchanged in the `ParserHello` message when a parser worker starts. The engine responds with a `ParserHelloResult`. If the engine refuses the hello, the result carries `accepted = false` and a `MessageVectorSet` describing the refusal. The function `RefusedHelloResult` in `parser_server_ipc.hpp` constructs these refusal messages from a code and reason string.

The current IPC protocol version is `kParserServerIpcProtocolCurrent = 1`, with supported range 1–1. A parser reporting a protocol version outside this range will have its hello refused.

The native SBsql v3 parser package is identified in code as `client_dialect = "sbsql_v3"`. Its package request struct (`NativeV3ParserPackageRequest`) includes the `parser_package_uuid`, `parser_package_version`, and a `registry_snapshot_uuid` that ties the parse to a specific catalog generation.

## Parser Configuration Templates

Twenty-five compatibility parser templates ship under `config/templates/` with names of the form `SB_<DIALECT>_Parser.conf`. The following dialects are covered by templates:

Apache Ignite, Cassandra, ClickHouse, CockroachDB, Dolt, DuckDB, Firebird (FBSQL), FoundationDB, ImmuDB, InfluxDB, MariaDB, Milvus, MongoDB, MySQL, Neo4j, OpenSearch, OpenSearch SQL/PPL, PostgreSQL (PGSQL), Redis, SQLite, TiDB, TiKV, Vitess, XTDB, YugabyteDB.

Every template follows the same structure. For example, the PostgreSQL parser template (`SB_PGSQL_Parser.conf`) reads:

```
parser.profile = default
parser.family = postgresql
parser.worker_binary = bin/SB_PGSQL_Parser
parser.bundle_contract_id = sbp_postgresql@1
parser.protocol_min = 1
parser.protocol_max = 1
parser.security.auth_relay_required = true
parser.execution.engine_authority_required = true
parser.compatibility.logical_backup_restore_streams = enabled_when_remote_logical_stream
parser.compatibility.server_local_file_access = denied_by_default
parser.compatibility.physical_page_backup_restore = denied
parser.compatibility.low_level_repair_verify = denied
```

The four compatibility keys have fixed semantics:

| Key | Meaning |
|-----|---------|
| `logical_backup_restore_streams` | Whether this parser can issue logical backup or restore stream requests |
| `server_local_file_access` | Whether this parser may request access to server-local filesystem paths |
| `physical_page_backup_restore` | Whether this parser may request physical page-level backup or restore |
| `low_level_repair_verify` | Whether this parser may request low-level repair or verify operations |

All four keys are `denied` or `denied_by_default` in every shipped compatibility template. Logical backup and restore is set to `enabled_when_remote_logical_stream`, meaning the stream must originate from a remote source rather than a local file shortcut. The engine enforces these constraints independently of what the parser requests, so a compatibility parser cannot escalate to physical backup or low-level repair even if the parser's own code attempts to do so.

`engine_authority_required = true` is set on all templates. This means the parser process cannot complete any statement without the engine's independent security and authorization check.

## Parser Worker Lifecycle

The listener manages parser workers through a `ParserPool`. Each worker is a separate process. Worker states are defined by `ParserWorkerState` in `parser_pool.hpp`:

| State | Meaning |
|-------|---------|
| `kCold` | Not yet started |
| `kStarting` | Process launching, hello not yet exchanged |
| `kIdlePreauth` | Hello accepted; waiting to be assigned a connection |
| `kAssigned` | Actively serving a client connection |
| `kDraining` | Finishing current work before stopping |
| `kStopped` | Cleanly stopped |
| `kQuarantined` | Suspended due to repeated failures |

A worker moves to `kQuarantined` after repeated failures. The quarantine entry in `ParserWorker` includes a `quarantine_until_ms` timestamp; the worker will not be restarted until that time has elapsed. The pool tracks a `fault_history` deque to support diagnostics.

The pool maintains a target minimum and maximum worker count (`target_min`, `target_max`). When workers are quarantined, the available pool shrinks. If the pool cannot serve incoming connections because all workers are quarantined or draining, new connection requests will be refused with a message vector indicating parser unavailability.

## Route Selection and the Database Route Check

When a client attaches, the engine performs a route match. If the database associated with the connection does not match the expected route, the session registry records a `database_route_mismatch` denial. This check occurs at three attachment points:

1. During authentication handoff (`auth_handoff`)
2. During standard database attachment (`attach_database`)
3. During embedded attachment (`embedded_attach`)

Each mismatch produces an `AddAttachAdmissionDenied` diagnostic with reason `database_route_mismatch` and a rejected payload. Operators who see this denial should verify that the client is connecting to the correct listener endpoint and that the database path matches the route configured for that endpoint.

## Parser-Visible Schema Roots and Workareas

A parser workarea is the schema root set that a particular parser is allowed to resolve names against. The parser submits a `NameResolutionRequest` to the engine with a `search_path` and a `presented_name`. The engine resolves the name in the context of the session's catalog and returns the object UUID or a refusal. The parser never resolves names directly from the catalog; all name resolution flows through the engine.

Schema roots visible to parsers are a subset of the full set documented in [Identity, Security, And Policy](identity_security_and_policy.md). The `sys.parser` schema root (`kLocalSysParserSchemaPath`) exposes parser registration and status information.

## Parser Package Isolation

Each parser worker is a separate operating system process. A crash, assertion failure, or memory corruption in the parser process does not affect the engine process. The worker exits, the pool records the fault event, and the pool attempts a restart with exponential backoff. The quarantine mechanism prevents a repeatedly crashing parser from consuming restart resources continuously.

Parser output (the SBLR envelope) is treated as translation evidence only until the engine independently validates it. The `NativeV3ParserPackageResult` flag `parser_is_trusted` is set by the engine after the envelope is validated, not by the parser itself.

## Unsupported Surface Refusal

If a compatibility parser receives a statement that its grammar does not support, it is expected to return a refusal rather than silently accept or partially execute. The `parser.package_admission` bootstrap policy (`registered_packages_only_v1`) governs this behavior: unregistered parsers are refused and the parser profile must match the registered package.

Statements that reference reference-tool emulations (such as `GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, `FBTRACEMGR`) are mapped to `sbsql.emulated.reference_tool_non_file` in `statement_catalog.cpp`. Statements attempting `BACKUP DATABASE` or `RESTORE DATABASE` without a full file path are mapped to `sbsql.emulated.backup_restore_non_file`. These emulated surfaces are not native administrative operations.

Similarly, shadow filespace operations (`CREATE SHADOW`, `ALTER SHADOW`, `DROP SHADOW`) that do not use the ScratchBird filespace model are mapped to `sbsql.emulated.shadow_non_file`. Operators expecting native storage operations should use the filespace lifecycle statements described in [Language Reference: Filespace](../Language_Reference/syntax_reference/filespace.md).

## Diagnosing Parser Problems

When a parser is missing, incompatible, or quarantined, the message vector will identify the problem. Useful fields to examine in `ParserPoolStatus`:

- `quarantine_active` — whether the pool is in a quarantine state
- `quarantine_until_ms` — when the quarantine expires
- `recent_failure_count` — how many failures occurred recently
- `fault_history` — a history of fault events with per-event diagnostics

The `ParserPoolFaultEvent` struct includes the `diagnostic` string for each failure and whether the fault was `intentional` (i.e., triggered by an operator stop command rather than a crash).

Operators can restart a specific worker with the pool's `RestartWorker` method or kill a misbehaving connection with `KillConnection`.

If a hello is refused, inspect the `MessageVectorSet` in the `ParserHelloResult`. The code and reason in the refused result indicate whether the rejection was a protocol version mismatch, an unrecognized dialect, or a policy refusal.

## Operator Checklist

- Confirm that the parser binary path in `parser.worker_binary` is correct before starting the listener.
- Set `parser.protocol_min` and `parser.protocol_max` consistently with the parser binary version.
- Do not modify the four `compatibility.*` keys in parser templates unless you understand the security implications; all four are `denied` by default for good reason.
- Monitor `quarantine_active` and `recent_failure_count` in the parser pool status to detect parser stability problems early.
- A `database_route_mismatch` denial is a configuration problem, not a credential problem. Check the listener endpoint and database route binding.

## Related Pages

- [Configuration Reference](configuration_reference.md)
- [Identity, Security, And Policy](identity_security_and_policy.md)
- [Language Reference: Database](../Language_Reference/syntax_reference/database.md)
- [Language Reference: Filespace](../Language_Reference/syntax_reference/filespace.md)
- [Getting Started: Engine Parser Boundary](../Getting_Started/architecture/engine_parser_boundary.md)
