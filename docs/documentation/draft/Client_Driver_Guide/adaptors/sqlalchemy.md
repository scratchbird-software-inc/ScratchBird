# SQLAlchemy Adaptor

The SQLAlchemy adaptor (`scratchbird-sqlalchemy-dialect`) provides a SQLAlchemy dialect for ScratchBird. It enables Python applications using SQLAlchemy — including ORM-mapped classes, Core expressions, and Inspector-based reflection — to connect to ScratchBird using a `scratchbird://` URL. The adaptor delegates all wire transport to the ScratchBird Python driver (DB-API 2.0); the dialect layer handles connection argument normalization, type mapping, and schema reflection queries.

**Status:** beta_2 (release_candidate bucket). Deterministic contract tests run without a live server; full ORM transaction and migration coverage requires a running ScratchBird instance.

**Conformance profile:** `adaptor_sqlalchemy_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-sqlalchemy-dialect` |
| driver\_package\_uuid | `019e12a0-0021-7000-8000-000000000021` |
| api\_surface | `application_adapter` |
| ingress\_mode | `driver_embedded_python` |
| delegates to / pooling | `delegates_to_python` (driver:python) |
| type\_mapping\_profile | `python_dbapi_mapping` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| license | Apache-2.0 (pyproject.toml) |

---

## Installation

The dialect is distributed as a Python wheel (`scratchbird-sqlalchemy`). Install it alongside the ScratchBird Python driver.

**Package name** (from `pyproject.toml`):

```bash
pip install scratchbird-sqlalchemy
# or editable install from source:
pip install -e path/to/scratchbird-sqlalchemy-dialect
```

**Dependencies:**

- `scratchbird>=0.1.0` (the ScratchBird Python driver)
- `SQLAlchemy>=1.4,<3.0`
- Python 3.9 or later

**Dialect registration** is via the `sqlalchemy.dialects` entry point declared in `pyproject.toml`:

```
scratchbird = "scratchbird_sqlalchemy.dialect:ScratchBirdDialect"
```

After installation, SQLAlchemy resolves `scratchbird://` URLs to `ScratchBirdDialect` automatically. No manual registration is required.

**Dialect class** (from `scratchbird_sqlalchemy/dialect.py`):

```
scratchbird_sqlalchemy.dialect.ScratchBirdDialect
```

`ScratchBirdDialect` extends `sqlalchemy.engine.default.DefaultDialect`.

---

## Configuring a connection

Pass a `scratchbird://` URL to `create_engine`. From the source README:

```python
from sqlalchemy import create_engine, inspect

engine = create_engine(
    "scratchbird://user:pass@localhost:3092/mydb?sslmode=require&binaryTransfer=true"
)
inspector = inspect(engine)
print(inspector.get_schema_names())
```

**URL structure:**

```
scratchbird://<user>:<password>@<host>:<port>/<database>[?param=value&...]
```

Default host is `localhost`, default port is `3092`.

**Connection argument policy** (`create_connect_args` in source):

The dialect normalizes URL query parameters into the keyword arguments passed to the Python driver. Aliases are resolved (e.g. `binaryTransfer` → `binary_transfer`, `currentSchema` → `schema`). The following values are rejected at connection-arg construction time before any driver call is made:

| Parameter | Rejected value | Reason |
|---|---|---|
| `sslmode` (or `ssl`) | `disable` | TLS floor policy |
| `binary_transfer` | `false` | Protocol requirement |
| `compression` | `zstd` | Not supported |
| `front_door_mode` | any value other than `direct` or `manager_proxy` | Policy |
| `auth_method_id` | any value not starting with `scratchbird.auth.` | Policy |

All manager-proxy, token/assertion, channel-binding, and dormant-reattach options are accepted as canonical query parameters.

---

## Type and feature mapping

`ScratchBirdDialect` carries a built-in `_TYPE_MAP` (from `dialect.py`) covering the full ScratchBird type surface. Representative entries:

| ScratchBird type | SQLAlchemy type |
|---|---|
| BOOLEAN | Boolean |
| SMALLINT | SmallInteger |
| INTEGER / INT | Integer |
| BIGINT / INT8 | BigInteger |
| REAL / FLOAT / DOUBLE | Float |
| NUMERIC / DECIMAL | Numeric |
| VARCHAR / CHAR / TEXT | String / Text |
| DATE / TIME / TIMESTAMP | Date / Time / DateTime |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | DateTime(timezone=True) |
| UUID | Uuid |
| JSON / JSONB | JSON |
| BYTEA / BLOB | LargeBinary |
| ARRAY | ARRAY(String) |
| VECTOR | ARRAY(Float) |
| GEOMETRY / GEOGRAPHY | LargeBinary |
| COMPOSITE / RECORD / ROW / RANGE | String |
| TSVECTOR / TSQUERY | Text |
| INET / CIDR / MACADDR | String |
| XML / INTERVAL | Text / String |
| MONEY | Numeric |

Type names with precision/scale suffixes (e.g. `VARCHAR(255)`) are normalized by stripping the parenthetical before lookup. Array-suffix types (e.g. `TEXT[]`) map to `ARRAY(String)`.

For the full type mapping reference see [../type_mapping.md](../type_mapping.md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- `create_engine("scratchbird://...")` URL handling and connection argument construction
- Inspector reflection:
  - `get_schema_names` — queries `sys.schemas`
  - `get_table_names` — queries `sys.tables` joined to `sys.schemas`, excludes views
  - `get_view_names` — queries `sys.tables` where `table_type = 'VIEW'`
  - `has_table` — existence check via `sys.tables`
  - `get_columns` — queries `sys.columns`; returns `name`, `type`, `nullable`, `default`, `autoincrement`
  - `get_pk_constraint` — queries `information_schema.table_constraints` / `key_column_usage`
  - `get_foreign_keys` — queries `information_schema.referential_constraints`
  - `get_indexes` — queries `sys.indexes` and `sys.index_columns`
- Schema-qualified reflection (pass `schema=` keyword to all reflection methods)
- Native boolean, decimal, and UUID support (`supports_native_boolean/decimal/uuid = True`)
- Named paramstyle (`paramstyle = "named"`)
- Statement cache support (`supports_statement_cache = True`)

**Not confirmed in source / deferred to driver or engine:**

- DDL generation (CREATE TABLE, ALTER TABLE, migrations) — not tested in contract suite
- ORM `Session` transaction lifecycle against a live server — deterministic tests are offline
- Connection pooling — managed by the underlying Python driver and SQLAlchemy pool; the adaptor itself has no pool

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/python.md](../drivers/python.md) — ScratchBird Python driver (DB-API 2.0)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection parameter reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../type_mapping.md](../type_mapping.md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — SQLSTATE and error mapping
