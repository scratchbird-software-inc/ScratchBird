# ScratchBird Python Driver — DB-API 2.0 (PEP 249)

The ScratchBird Python driver gives Python applications access to a ScratchBird
Convergent Data Engine (CDE) through the standard DB-API 2.0 interface defined
by PEP 249. It speaks the ScratchBird Native Wire Protocol (SBWP v1.1) directly
and does not depend on any intermediate ODBC or JDBC layer.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0012-7000-8000-000000000012` |
| `api_surface_set` | `dbapi_2` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_python_gate` |

---

## Installation

The package is distributed as `scratchbird` (PyPI name). Build a wheel from
source using `setuptools`:

```bash
python -m pip install build
python -m build
python -m pip install dist/scratchbird-0.1.0-py3-none-any.whl
```

Or install in editable mode for development:

```bash
python -m pip install -e .
```

Requires Python 3.8 or later. License: MPL-2.0.
Source: `project/drivers/driver/python/` — `pyproject.toml`, package name
`scratchbird`, version `0.1.0`.

---

## Connecting

The driver accepts two DSN forms. The entry point is `scratchbird.connect(...)`,
which returns a `Connection` object.

### URI form

```
scratchbird://user:password@host:3092/database?sslmode=require
```

### Key-value form (whitespace or semicolon separated)

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass sslmode=require
```

Accepted aliases: `dbname` -> `database`, `username` -> `user`,
`connecttimeout` -> `connect_timeout`.

### Minimal connection example

```python
import scratchbird

conn = scratchbird.connect(
    "scratchbird://alice:secret@db.example.com:3092/prod?sslmode=require"
)
```

Or using keyword arguments:

```python
conn = scratchbird.connect(
    host="db.example.com",
    port=3092,
    database="prod",
    user="alice",
    password="secret",
    sslmode="require",
)
```

### Token auth

```python
conn = scratchbird.connect(
    host="db.example.com",
    port=3092,
    database="prod",
    user="alice",
    auth_method_id="scratchbird.auth.token",
    auth_token="<bearer-token>",
)
```

### Manager-proxy ingress

```python
conn = scratchbird.connect(
    host="proxy.example.com",
    port=3090,
    database="prod",
    user="alice",
    password="secret",
    front_door_mode="manager_proxy",
    manager_auth_token="<proxy-token>",
)
```

### Auth preflight probe

```python
result = scratchbird.probe_auth_surface(
    "scratchbird://alice@db.example.com:3092/prod"
)
```

The `Connection.get_resolved_auth_context()` method returns the auth surface
resolved after connect. Directly executable auth classes: `PASSWORD`,
`SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`. `MD5`, `PEER`, and `REATTACH` are
fail-closed.

The default port is `3092`. The default `sslmode` is `require`.

---

## Executing statements and transactions

### Cursors

```python
cur = conn.cursor()
cur.execute("SELECT id, name FROM users WHERE region = %s", ("west",))
rows = cur.fetchall()
```

Parameter style is `pyformat` (`%s` positional or `%(name)s` named). Named
parameters with `::` cast syntax are handled correctly — the cast marker is not
mistaken for a named placeholder.

### executemany

```python
cur.executemany(
    "INSERT INTO events (ts, kind) VALUES (%s, %s)",
    [(t1, "click"), (t2, "view")],
)
```

Repeated identical `INSERT ... VALUES` batch shapes reuse a session-local
prepared statement handle to reduce parse overhead for high-volume loads.
`seq_of_params` must not be `None`.

### Generated keys and status

```python
cur.execute("INSERT INTO orders (amount) VALUES (%s) RETURNING id", (99.5,))
print(cur.lastrowid)       # last generated key from COMMAND_COMPLETE
print(cur.statusmessage)   # command tag string
```

### Multi-result traversal

```python
cur.execute("SELECT 1; SELECT 2")
print(cur.fetchall())
cur.nextset()
print(cur.fetchall())
```

### Transaction control

ScratchBird sessions are always in a transaction. `COMMIT` and `ROLLBACK`
immediately reopen the next boundary.

```python
conn.begin()                    # start an explicit transaction
conn.execute("UPDATE ...")      # via Connection.execute shortcut
conn.commit()

conn.begin()
conn.rollback()

sp = conn.savepoint("sp1")
conn.rollback_to_savepoint("sp1")
conn.release_savepoint("sp1")
```

Isolation levels: the canonical MGA mapping is:
- `READ UNCOMMITTED` — legacy compatibility alias only
- `READ COMMITTED` — canonical `READ COMMITTED`
- `REPEATABLE READ` — canonical `SNAPSHOT`
- `SERIALIZABLE` — canonical `SNAPSHOT TABLE STABILITY`

Use `canonical_isolation_label(level)` and
`canonical_read_committed_mode_label(mode)` to inspect the current mapping.
`READ_COMMITTED_MODE_READ_CONSISTENCY` selects canonical `READ COMMITTED READ
CONSISTENCY`.

### Autocommit

```python
conn.autocommit = True   # commits any active transaction first, then switches
conn.autocommit = False  # eagerly starts a transaction if none is active
```

Autocommit transitions are local driver policy; the wrapper does not push a
synthetic wire `SET_OPTION autocommit` for the native lane.

### Batch and multi-result convenience APIs

```python
summary = conn.execute_batch(
    "INSERT INTO t (x) VALUES (%s)",
    [(1,), (2,), (3,)],
)
# returns: totalRowCount + per-item (index, rowCount, fields, command, lastId)

results = conn.query_multi("SELECT 1; SELECT 2", None)
# returns: list of {rows, rowCount, fields, command, lastId}
```

### Retry scope

```python
from scratchbird import retry_scope_for_sqlstate
scope = retry_scope_for_sqlstate("40001")
# "statement" — retry at a fresh statement boundary
```

`40001`/`40P01` → `RETRY_SCOPE_STATEMENT`;
`08xxx` → `RETRY_SCOPE_RECONNECT`;
all others → `RETRY_SCOPE_NONE`.

---

## Type mapping

The `sbsql_core` type-mapping profile applies. The table below lists canonical
SBsql types and their Python representations.

| SBsql canonical type | Python type | Notes |
|---|---|---|
| `BOOLEAN` | `bool` | text tokens `true`/`t` → `True`, others → `False` |
| `SMALLINT` (`INT2`) | `int` | |
| `INTEGER` (`INT4`) | `int` | |
| `BIGINT` (`INT8`) | `int` | unknown-text integer capped at signed int64 |
| `REAL` (`FLOAT4`) | `float` | |
| `DOUBLE PRECISION` (`FLOAT8`) | `float` | |
| `NUMERIC` / `DECIMAL` | `decimal.Decimal` | |
| `TEXT` / `VARCHAR` / `CHAR` | `str` | |
| `BYTEA` | `bytes` | hex (`\x`/`0x`) and octal-escape decode |
| `DATE` | `datetime.date` | |
| `TIME` | `datetime.time` (naive) | rejects timezone-offset payloads |
| `TIMETZ` | `datetime.time` (offset-aware) | 12-byte binary: micros + zone seconds west |
| `TIMESTAMP` | `datetime.datetime` (naive) | |
| `TIMESTAMPTZ` | `datetime.datetime` (UTC-aware) | |
| `UUID` | `uuid.UUID` | |
| `JSON` / `JSONB` | `scratchbird.Json` / `scratchbird.Jsonb` | |
| `XML` | `scratchbird.SqlXml` | |
| array types | `list` | typed OID inference for bool/int/float/text/date/time/timetz/timestamp/timestamptz/numeric/uuid/bytea |
| range types | `scratchbird.Range` | string temporal bounds coerced to UTC binary |
| geometry | `scratchbird.Geometry` | |
| `BLOB` | `scratchbird.Blob` → `OID_BYTEA` | |
| `CLOB` | `scratchbird.Clob` → `OID_TEXT` | |
| `ROWID` | `scratchbird.RowId` → `OID_BYTEA` | |
| `REF` | `scratchbird.Ref` → `OID_TEXT` | |
| `SQLXML` | `scratchbird.SqlXml` → `OID_XML` | |

Python `Enum` values encode as text using the member name.
Custom objects fall back to `str(value)`.

See [../type_mapping.md](../type_mapping.md) for the shared `sbsql_core`
profile.

---

## Metadata and introspection

The driver exposes `sys.information.*` through collection-based metadata APIs
on `Connection`:

```python
tables = conn.get_schema("tables", restrictions={"TABLE_SCHEMA": "public"})
schemas = conn.get_schema("schemas")
pks = conn.get_schema("primary_keys", restrictions={"TABLE_NAME": "orders"})
```

Supported collections: `tables`, `columns`, `schemas`, `indexes`,
`index_columns`, `constraints`, `catalogs`, `primary_keys`, `foreign_keys`,
`procedures`, `functions`, `routines`, `table_privileges`,
`column_privileges`, `type_info`.

Restrictions support `%`/`_` wildcard matching (JDBC-style, with escape
handling) and `"null"` literal matching.

Recursive schema-tree navigation:

```python
payload = conn.ddl_editor_schema_payload(
    schema_pattern="pub%",
    expand_schema_parents=True,
)
# returns: {schemaPaths: [...], schemaTree: {...}}
```

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

The driver implements the full DB-API 2.0 exception hierarchy:

| Class | Meaning |
|---|---|
| `scratchbird.Warning` | Non-fatal warning |
| `scratchbird.InterfaceError` | Driver API misuse |
| `scratchbird.DatabaseError` | Base for all server errors |
| `scratchbird.DataError` | Bad data value |
| `scratchbird.OperationalError` | Connection / server errors |
| `scratchbird.IntegrityError` | Constraint violation |
| `scratchbird.InternalError` | Server internal error |
| `scratchbird.ProgrammingError` | SQL syntax / API error |
| `scratchbird.NotSupportedError` | Unsupported operation |

SQLSTATE codes from the server are preserved in `exception.args` alongside
DETAIL and HINT fields. SQL normalization `ValueError` is mapped to
`ProgrammingError`.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The driver is `thread_safe` (one connection per thread is the safe default for
DB-API). The `scratchbird.ConnectionPool` class provides explicit pooling with
configurable min/max size, checkout/reuse, stale-connection replacement,
statement caching (`StatementCache`), retry backoff (`retry_with_backoff`),
circuit-breaker support (`CircuitBreaker`), and keepalive validation. Each
pooled connection resets cached borrow state before reuse.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

Conformance gate: `driver_python_gate`. All JDBCBL groups (CONN, TXN, EXEC,
META, TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md)
- [../connection_and_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md)
