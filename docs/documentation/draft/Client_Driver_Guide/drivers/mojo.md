# ScratchBird Mojo Driver — Language Binding (Experimental)

> **Status: beta\_2 / release\_candidate (experimental)** — The Mojo driver is
> validated on Linux only using a pixi-managed Mojo toolchain. Windows and macOS
> are not supported. The current implementation is a Mojo-Python interop lane:
> the public API is expressed in Mojo syntax but execution delegates to a Python
> transport shim (`src/scratchbird.py`) with an opt-in SBWP wire bridge
> (`sb_wire_transport=python`). Native Mojo socket/TLS transport is future work.

## Purpose

The Mojo driver provides a ScratchBird client surface for programs written in
Mojo (Magic's AI-native programming language). Because Mojo's native socket and
TLS ecosystem is still maturing, the lane currently operates as a Mojo-Python
interop shim. The facade module (`src/scratchbird.mojo`) re-exports the public
API from the native bootstrap (`src/scratchbird_native.mojo`), while real wire
execution routes through the Python-backed `_PythonWireConnection` adapter in
`src/scratchbird.py`.

Target audience: Mojo/MAX developers who need ScratchBird connectivity today
and want to migrate to pure native transport as the Mojo toolchain matures.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0007-7000-8000-000000000007`     |
| `driver_family`          | `mojo`                                     |
| `api_surface_set`        | `language_binding`                         |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `session_pool`                             |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_mojo_gate`                         |

## Requirements and Install

- Python 3.10+ (for the bridge shim)
- Mojo toolchain via `pixi` (recommended workspace: `~/mojo-work/sb-mojo`)

There is no published package yet. Use the lane directly from the source tree:

```bash
# Install pixi (https://prefix.dev/docs/pixi/overview)
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 \
  -I src -I src/scratchbird \
  tests/scratchbird_surface.mojo
```

## Connecting

The entry-point modules are `src/scratchbird.mojo` (Mojo facade) and
`src/scratchbird.py` (Python-backed shim/bridge).

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. Bracketed IPv6 hosts are supported (`[::1]:3092`). Key
aliases are extensive; for example `user|username|pguser`, `host|hostname|servername|pghost`,
`database|dbname|databaseName|pgdatabase`. The full alias list and all DSN
options are documented in ../connection\_and\_dsn.md.

`sslmode` / `ssl` values: `disable`, `allow`, `prefer`, `require`,
`verify-ca`, `verify-full`. `sslmode=disable` is accepted; production should
use a TLS-enabled mode.

**Connection from Mojo (via facade, verified against `src/scratchbird.mojo`):**

```mojo
from scratchbird import ScratchBirdConfig, ScratchBirdConnection, connect

fn main() raises:
    let cfg = ScratchBirdConfig.from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb"
    )
    let conn = connect(cfg)
    let rows = conn.query("SELECT 1")
    print(rows)
    conn.close()
```

**Connection from the Python shim (bridge mode, `src/scratchbird.py`):**

```python
from scratchbird import ScratchBirdConnection

conn = ScratchBirdConnection.connect(
    "scratchbird://user:pass@localhost:3092/mydb"
)
rows = conn.query("SELECT 1")
conn.close()
```

Wire execution is activated via `sb_wire_transport=python` DSN key or the
`SCRATCHBIRD_MOJO_WIRE_TRANSPORT` environment variable. Without it the
connection operates in deterministic (test-shim) mode.

### Auth discovery

```python
surface = probe_auth_surface("scratchbird://user@localhost:3092/mydb")
ctx     = conn.get_resolved_auth_context()
```

Supported auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`,
`manager_proxy` token bootstrap. Methods `MD5`, `PEER`, and `REATTACH` fail
closed.

## Executing Statements and Transactions

```python
# Simple query
rows = conn.query("SELECT id FROM users WHERE active = ?", [True])

# Parameterized (explicit)
rows = conn.query("SELECT * FROM t WHERE n = ?", [42])

# Transaction
conn.begin(isolation_level=2)  # SNAPSHOT
conn.query("UPDATE ...")
conn.commit()

# Savepoint
conn.begin()
conn.savepoint("sp1")
conn.rollback_to_savepoint("sp1")
conn.commit()
```

**Isolation-level aliases** (source: `src/scratchbird.py` —
`canonical_isolation_label`):

| Integer / alias       | Wire canonical label             |
|-----------------------|----------------------------------|
| `1` / `READ_COMMITTED`  | `READ COMMITTED`               |
| `2` / `REPEATABLE_READ` | `SNAPSHOT`                     |
| `3` / `SERIALIZABLE`    | `SNAPSHOT TABLE STABILITY`     |
| `0` / `READ_UNCOMMITTED`| legacy compatibility alias     |

`READ COMMITTED` sub-mode is set via `read_committed_mode`; use
`canonical_read_committed_mode_label(mode)` for the canonical label.

**Retry boundary** (source: `src/scratchbird.py` —
`retry_scope_for_sqlstate`):

| SQLSTATE         | Retry boundary                  |
|------------------|---------------------------------|
| `40001`, `40P01` | Fresh statement only            |
| `08xxx`          | Reconnect / reopen only         |
| All others       | No automatic replay             |

### Prepared transactions and dormant sessions

`supports_prepared_transactions()` → `true`. Use `prepare_transaction(gid)`,
`commit_prepared(gid)`, `rollback_prepared(gid)`.
`supports_dormant_reattach()` → `false`; related helpers fail closed with
SQLSTATE `0A000`.

### Streaming and cancellation

```python
stream = conn.stream("SELECT * FROM big_table")
for row in stream:
    process(row)
stream.close()

# Cancel in-flight
conn.cancel()  # SQLSTATE 57014 on the server side
```

Closed-stream reads raise with SQLSTATE `HY010`. Active-stream reads on a
closed connection raise `08003`.

### Pipeline and circuit breaker

Pipeline capacity is bounded by `pipeline_max_in_flight` DSN key; overflow
emits SQLSTATE `54000`. Circuit-breaker open state emits SQLSTATE `08006`.
The half-open recovery window is controlled by `cb_half_open_max_requests`.

## Type Mapping

Full reference: [../type\_mapping.md](../type_mapping.md).

| SBsql core type    | Python/Mojo type       | OID constant (src)         |
|--------------------|------------------------|----------------------------|
| `INTEGER`          | `int`                  | `OID_INT4` (23)            |
| `TEXT` / `VARCHAR` | `str`                  | `OID_TEXT`/`OID_VARCHAR`   |
| `DATE`             | `datetime.date`        | `OID_DATE` (1082)          |
| `TIME`             | `datetime.time`        | `OID_TIME` (1083)          |
| `TIMESTAMP`        | `datetime.datetime`    | `OID_TIMESTAMP` (1114)     |
| `TIMESTAMPTZ`      | `datetime.datetime`    | `OID_TIMESTAMPTZ` (1184)   |
| `JSON` / `JSONB`   | `dict` / `list`        | `OID_JSON`/`OID_JSONB`     |
| `UUID`             | `str`                  | `OID_UUID` (2950)          |
| `INET` / `CIDR`    | `str`                  | `OID_INET`/`OID_CIDR`      |
| `MACADDR`          | `str`                  | `OID_MACADDR` (829)        |
| `VECTOR`           | `[float]`              | `OID_SB_VECTOR` (16386)    |
| Arrays             | `list`                 | e.g. `OID_INT4_ARRAY` (1007)|
| `COMPOSITE`        | `list`                 | `OID_RECORD` (2249)        |

Type codecs (source: `src/scratchbird.py`) cover array/range/vector, composite,
geometry, inet/cidr/macaddr, json/jsonb, uuid, and temporal types including
intervals.

## Metadata via `sys.information.*`

Full reference: [../metadata\_sys\_information.md](../metadata_sys_information.md).

Metadata query constants are defined in `src/scratchbird.py` and exported via
`src/scratchbird.mojo`. Access via the native bootstrap or shim:

```python
conn.query_metadata("schemas")
conn.query_metadata("tables")
conn.query_metadata("columns")
conn.query_metadata_restricted("tables", {"TABLE_SCHEM": "public"})
conn.query_metadata_restricted_multi("tables",
    {"TABLE_SCHEM": "public", "TABLE_NAME": "users"})
conn.get_schema("tables")
conn.ddl_editor_schema_payload(schema_pattern="public")
```

Collection names: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `procedures`, `functions`, `routines`, `catalogs`,
`primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`type_info`.

## Errors and Diagnostics

Full reference: [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md).

The shim raises `ScratchBirdError(message, sqlstate=..., detail=..., hint=...)`
(source: `src/scratchbird.py`). Use `extract_sqlstate(err_str)` (source:
`src/scratchbird_native.mojo` / `src/scratchbird.mojo`) to extract the SQLSTATE
code from error strings in Mojo tests.

Key SQLSTATE codes emitted by the driver:

| Condition                          | SQLSTATE  |
|------------------------------------|-----------|
| Connection closed, op attempted    | `08003`   |
| Circuit breaker open               | `08006`   |
| Manager proxy token missing        | `08001`   |
| Auth failure                       | `28P01`   |
| Invalid DSN integer parameter      | `22023`   |
| Unsupported operation              | `0A000`   |
| Closed statement / stream          | `HY010`   |
| Pipeline capacity exceeded         | `54000`   |
| Query cancelled                    | `57014`   |
| Parameter count mismatch           | `07001`   |
| Savepoint missing                  | `3B001`   |

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md).

The driver is `thread_safe` with `session_pool` capability. Lifecycle
scaffolds (source: `src/scratchbird/` directory):

- `circuit_breaker.mojo` — state-based breaker
- `keepalive.mojo` — idle-window tracker and manager
- `leak_detector.mojo` — checkout/checkin bookkeeping
- `telemetry.mojo` — tracing, metrics, slow-query retention
- `pipeline.mojo` — queue and flush management

## Conformance

Full reference: [../conformance\_baseline.md](../conformance_baseline.md).

Conformance gate: `driver_mojo_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status                                    |
|--------------|-------------------------------------------|
| `CONN`       | Implemented (native facade + wire bridge) |
| `TXN`        | Implemented (hybrid parity)               |
| `EXEC`       | Implemented (hybrid parity)               |
| `META`       | Implemented (hybrid parity)               |
| `TYPE`       | Implemented (deterministic + wire bridge) |
| `ERR`        | Implemented (deterministic + wire negative-path) |
| `RES`        | Implemented (deterministic + wire lifecycle) |

Open gap: pure Mojo-native socket/TLS transport (replacing the Python bridge).

## Platform Support

| Platform | Status      |
|----------|-------------|
| Linux    | Experimental (pixi-managed Mojo toolchain) |
| Windows  | Not supported |
| macOS    | Not supported |

## See Also

- [../README.md](../README.md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire\_protocol\_sbwp.md](../wire_protocol_sbwp.md)
- [../type\_mapping.md](../type_mapping.md)
- [../metadata\_sys\_information.md](../metadata_sys_information.md)
- [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md)
- [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md)
- [../conformance\_baseline.md](../conformance_baseline.md)
