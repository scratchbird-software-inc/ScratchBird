# ScratchBird R Driver — R DBI Interface

The ScratchBird R driver gives R applications access to a ScratchBird
Convergent Data Engine (CDE) through the standard R DBI interface (the
`DBI` package). It implements the `DBIDriver`, `DBIConnection`, and
`DBIResult` virtual classes and speaks the ScratchBird Native Wire
Protocol (SBWP v1.1) directly — no ODBC or JDBC layer is involved.

The package name is `scratchbird` (R package, CRAN-style). It carries its
own native TLS transport (`src/tls_transport.c`) and a pure-R SBWP
protocol implementation (`R/protocol.R`, `R/client.R`).

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `component_id` | `driver:r` |
| `driver_package_uuid` | `019e12a0-0013-7000-8000-000000000013` |
| `driver_family` | `r` |
| `api_surface_set` | `dbi` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `session_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_r_gate` |

---

## Installation

The package is distributed as `scratchbird` (R package, version `0.1.0`).
Build and install from the source tree at `project/drivers/driver/r/`:

```r
# From within the project/drivers/driver/r/ directory:
install.packages(".", repos = NULL, type = "source")
```

Or, for development with `pkgload`:

```bash
Rscript -e "pkgload::load_all(quiet=TRUE)"
```

**System requirements:** OpenSSL (`libssl`, `libcrypto`) must be present.
The native TLS transport (`src/tls_transport.c`) is compiled on install
via `src/Makevars` (Linux/macOS) or `src/Makevars.win` (Windows).

**R package dependencies:** `DBI`, `openssl`, `jsonlite`, `methods`.
Test dependency: `testthat`.

**Platform support:**

| Platform | Status |
|---|---|
| Linux | Supported — CI build and test coverage |
| Windows | Supported — CI build and test coverage |
| macOS | Untested — not currently covered in CI |

License: MPL-2.0.

---

## Connecting

The driver entry point is the `Scratchbird()` constructor, which returns a
`ScratchbirdDriver` object. Pass it to `DBI::dbConnect()` along with a
DSN string or keyword arguments. The lower-level `sb_connect()` function
is also exported for use outside the DBI calling convention.

### DBI entry point

```r
library(DBI)
library(scratchbird)

# URI DSN
con <- dbConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb")

# Key-value DSN (space-separated or semicolon-separated)
con <- dbConnect(Scratchbird(), "host=localhost port=3092 dbname=mydb user=myuser password=mypass")

# Test reachability without holding a connection
ok <- dbCanConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb")
```

The `dbConnect` method delegates to `sb_connect(dsn, ...)`.

### DSN forms

**URI:**

```
scratchbird://user:password@host:3092/database?sslmode=require
```

**Key-value (space- or semicolon-separated):**

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

**Manager-proxy URI:**

```
scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token
```

### Selected DSN keys

| Key | Aliases | Default | Notes |
|---|---|---|---|
| `host` | `server`, `datasource` | `localhost` | |
| `port` | — | `3092` | |
| `database` | `dbname`, `initial catalog` | (required) | |
| `user` | `username`, `uid` | (required) | |
| `password` | `pwd` | | |
| `sslmode` | `ssl mode` | `require` | `require` enforces TLS 1.3 floor |
| `front_door_mode` | `connection_mode` | `direct` | `direct` or `manager_proxy` |
| `manager_auth_token` | `mcp_auth_token` | | Required for `manager_proxy` mode |
| `schema` | `search_path` | | Optional initial schema |
| `fetch_size` | `defaultrowfetchsize` | `0` (all) | Rows per incremental fetch |
| `binary_transfer` | `binarytransfer` | `TRUE` | Binary wire encoding |

The full key list is implemented in `R/config.R` (`apply_param`).

### Low-level connect

```r
# sb_connect returns a raw client environment (not a DBIConnection)
client <- sb_connect("scratchbird://user:pass@localhost:3092/mydb")
sb_disconnect(client)
```

### Authentication probe

```r
# Inspect available auth methods before connecting
surface <- sb_probe_auth_surface("scratchbird://user@localhost:3092/mydb")
ctx    <- sb_get_resolved_auth_context(client)
```

Natively supported auth methods: `PASSWORD`, `SCRAM_SHA_256`,
`SCRAM_SHA_512`, `TOKEN`, `manager_proxy` token bootstrap. Methods
`MD5`, `PEER`, and `REATTACH` are negotiated but fail-closed when locally
unsupported.

---

## Executing queries and fetching results

The R driver implements the standard DBI execution lifecycle.

### DBI methods

```r
# Simple one-shot query — returns a data.frame
df <- dbGetQuery(con, "SELECT id, name FROM orders WHERE status = $1", list("open"))

# Send / fetch / clear lifecycle
res  <- dbSendQuery(con, "SELECT id, name FROM orders")
df   <- dbFetch(res, n = 100)   # fetch up to 100 rows; n = -1 fetches all
info <- dbColumnInfo(res)        # column metadata data.frame (see below)
dbClearResult(res)

# DML / DDL
rows <- dbExecute(con, "UPDATE orders SET status = $1 WHERE id = $2", list("closed", 42L))
```

### Column metadata from `dbColumnInfo`

`dbColumnInfo(res)` returns a `data.frame` with one row per column:

| Column | Type | Description |
|---|---|---|
| `name` | `character` | Column name |
| `type_oid` | `integer` | ScratchBird OID |
| `type_size` | `integer` | Fixed size, or -1 for variable |
| `type_modifier` | `integer` | Precision/length modifier |
| `table_oid` | `integer` | Source table OID |
| `column_index` | `integer` | 1-based column index |
| `format` | `integer` | Wire format (`0` = text, `1` = binary) |
| `nullable` | `logical` | `TRUE` if column is nullable |

### Low-level query functions

| Symbol | Source | Notes |
|---|---|---|
| `sb_query(client, sql, ...)` | `R/client.R` | Send query, fetch all rows, return data.frame |
| `sb_get_query(client, sql, ...)` | `R/client.R` | Alias used by DBI layer |
| `sb_send_query(client, sql, ...)` | `R/client.R` | Non-blocking send; returns result env |
| `sb_fetch(result, n)` | `R/client.R` | Incremental fetch |
| `sb_clear_result(result)` | `R/client.R` | Drain and free result |
| `sb_cancel(client)` | `R/client.R` | Issue cancellation request |

---

## Transactions

ScratchBird sessions are always-in-a-transaction: `COMMIT` or `ROLLBACK`
immediately reopens the next transaction boundary. The driver tracks this
through native `READY`, `TXN_STATUS`, and `current_txn_id` protocol
fields.

### DBI transaction methods

```r
dbBegin(con)          # calls sb_begin(), sets autocommit FALSE
dbCommit(con)         # calls sb_commit(), sets autocommit TRUE
dbRollback(con)       # calls sb_rollback(), sets autocommit TRUE
```

### Isolation level mapping

The driver exposes the canonical mapping via `sb_canonical_isolation_label()`:

| SQL name | Canonical ScratchBird level |
|---|---|
| `READ UNCOMMITTED` | Legacy compatibility alias (not a distinct level) |
| `READ COMMITTED` | `READ COMMITTED` |
| `REPEATABLE READ` | `SNAPSHOT` |
| `SERIALIZABLE` | `SNAPSHOT TABLE STABILITY` |

`READ COMMITTED` sub-mode is selectable via
`sb_canonical_read_committed_mode_label()`, including
`READ COMMITTED READ CONSISTENCY`.

### `sb_begin` advanced options

`sb_begin(client, ...)` accepts named arguments that map directly to the
MGA engine begin payload:

| Argument | Notes |
|---|---|
| `isolation_level` | See canonical mapping above |
| `access_mode` | `READ WRITE` or `READ ONLY` |
| `deferrable` | Logical |
| `wait` | Logical (lock wait vs. no-wait) |
| `timeout_ms` | Lock wait timeout |
| `autocommit_mode` | |
| `conflict_action` | |
| `read_committed_mode` | Sub-mode selector for `READ COMMITTED` |

### Savepoints

```r
# Low-level savepoint operations
sb_savepoint(client, "sp1")
sb_release_savepoint(client, "sp1")
sb_rollback_to_savepoint(client, "sp1")
```

### Prepared / two-phase transactions

```r
sb_supports_prepared_transactions(client)      # TRUE/FALSE
sb_prepare_transaction(client, "xid_1")
sb_commit_prepared(client, "xid_1")
sb_rollback_prepared(client, "xid_1")
```

### Dormant sessions

Dormant session reattach is probed but intentionally fails closed:

```r
sb_supports_dormant_reattach(client)           # always FALSE currently
sb_detach_to_dormant(client)                   # raises SQLSTATE 0A000
sb_reattach_dormant(client, id, token)         # raises SQLSTATE 0A000
```

### Retry guidance

`sb_retry_scope_for_sqlstate(sqlstate)` returns the retry boundary:

| SQLSTATE | Retry scope |
|---|---|
| `40001`, `40P01` | Retry fresh statement only |
| `08xxx` | Reconnect or reopen only |
| Any other | No automatic replay |

---

## Type mapping

The type mapping profile is `sbsql_core`. The driver encodes parameters
in binary (`SB_FORMAT_BINARY = 1L`) by default. The following OID
constants are defined in `R/types.R` and govern encode/decode dispatch:

| R type | ScratchBird OID constant | Notes |
|---|---|---|
| `logical` (scalar) | `SB_OID_BOOL` (16) | |
| `integer` (scalar) | `SB_OID_INT4` (23) | |
| `numeric` (integer-valued, in INT4 range) | `SB_OID_INT4` (23) | Auto-promoted |
| `numeric` (double) | `SB_OID_FLOAT8` (701) | |
| `character` (scalar) | `SB_OID_TEXT` (25) | UUID strings detected and sent as `SB_OID_UUID` |
| `character` (UUID pattern) | `SB_OID_UUID` (2950) | Regex-matched |
| `Date` | `SB_OID_DATE` (1082) | |
| `POSIXct` / `POSIXt` | `SB_OID_TIMESTAMPTZ` (1184) | |
| `raw` | `SB_OID_BYTEA` (17) | |
| `sb_jsonb` | `SB_OID_JSONB` (3802) | Construct with `sb_jsonb(value = ...)` |
| `sb_geometry` | `SB_OID_POINT` (600) | Construct with `sb_geometry(wkb, srid)` |
| `sb_range` | OID from `encode_range()` | Construct with `sb_range(...)` |
| `sb_composite` | `SB_OID_RECORD` (2249) | Construct with `sb_composite(fields)` |
| `numeric` vector (length > 1) | `SB_OID_SB_VECTOR` (16386) | ScratchBird vector type |
| `list` / atomic vector (length > 1) | Array literal (OID 0) | |

**Decoding** (`decode_value` / `decode_binary_value` in `R/types.R`):

| ScratchBird OID | R result type |
|---|---|
| `SB_OID_BOOL` | `logical` |
| `SB_OID_INT2`, `SB_OID_INT4` | `integer` |
| `SB_OID_INT8` | `numeric` (64-bit via `read_i64_numeric`) |
| `SB_OID_FLOAT4`, `SB_OID_FLOAT8` | `numeric` |
| `SB_OID_NUMERIC` | `numeric` (or `character` if non-numeric) |
| `SB_OID_MONEY` | `numeric` (cents / 100) |
| `SB_OID_TEXT`, `SB_OID_VARCHAR`, `SB_OID_BPCHAR`, `SB_OID_CHAR` | `character` |
| `SB_OID_JSON`, `SB_OID_XML` | `character` |
| `SB_OID_JSONB` | `sb_jsonb` S3 object |
| `SB_OID_BYTEA` | `raw` |
| `SB_OID_UUID` | `character` (formatted UUID string) |
| `SB_OID_DATE` | `Date` (days from 2000-01-01) |
| `SB_OID_TIME` | `POSIXct` (seconds from epoch UTC) |
| `SB_OID_TIMESTAMP`, `SB_OID_TIMESTAMPTZ` | `POSIXct` (microseconds from 2000-01-01 UTC) |
| `SB_OID_INTERVAL` | Named `list` (`micros`, `days`, `months`) |
| Range OIDs (`INT4RANGE`, `INT8RANGE`, `NUMRANGE`, `TSRANGE`, `TSTZRANGE`, `DATERANGE`) | `sb_range` S3 object |
| `SB_OID_RECORD` | `sb_composite` S3 object |
| `SB_OID_TSVECTOR`, `SB_OID_TSQUERY`, `SB_OID_INET`, `SB_OID_CIDR`, `SB_OID_MACADDR`, `SB_OID_MACADDR8` | `character` |

See [../type_mapping.md](../type_mapping.md) for the full SBsql type
reference.

---

## Metadata

Schema, table, and column metadata are available through DBI metadata
methods and dedicated helper functions:

### DBI metadata methods

```r
dbListTables(con)                          # schema-qualified table names
dbExistsTable(con, "myschema.orders")      # TRUE/FALSE
dbListFields(con, "orders")                # column name vector
```

### Low-level metadata helpers

| Symbol | Source | Returns |
|---|---|---|
| `sb_metadata_schemas_query()` | `R/metadata.R:3` | SQL string for schema enumeration |
| `sb_metadata_tables_query()` | `R/metadata.R:7` | SQL string for table enumeration |
| `sb_metadata_columns_query()` | `R/metadata.R:11` | SQL string for column enumeration |
| `sb_metadata_indexes_query()` | `R/metadata.R:18` | SQL string for index enumeration |
| `sb_metadata_index_columns_query()` | `R/metadata.R:22` | SQL string for index column detail |
| `sb_metadata_constraints_query()` | `R/metadata.R:26` | SQL string for constraint enumeration |
| `sb_metadata_procedures_query()` | `R/metadata.R:30` | SQL string for procedure enumeration |
| `sb_metadata_functions_query()` | `R/metadata.R:34` | SQL string for function enumeration |

### Recursive schema navigation

```r
paths <- sb_metadata_schema_paths_for_navigation(schemas_df)
tree  <- sb_metadata_build_schema_tree(schemas_df)
rows  <- sb_metadata_build_schema_tree_rows(database_name, schemas_df)
```

`sb_metadata_build_schema_tree_rows` returns a flattened depth-first row
sequence with a root `database` row followed by schema rows — suitable
for tree-view navigation in IDEs and tools.

**Conformance note:** The META conformance area is listed as `Partial`
in the S3 implementation record. Richer privilege, key, and DDL-editor
metadata coverage is pending.

See [../metadata_sys_information.md](../metadata_sys_information.md) for
the `sys.information.*` view hierarchy.

---

## Errors and diagnostics

The diagnostic mapping profile is `native_sqlstate`. Errors raised by the
driver are R conditions of class `c("<sqlstate_class>", "error")` where
`<sqlstate_class>` is derived from the five-character SQLSTATE code
returned by the server.

Key error-path symbols in `R/client.R`:

| Symbol | Source | Role |
|---|---|---|
| `sb_sqlstate_error_class(sqlstate)` | `R/client.R:565` | Maps SQLSTATE to R condition class |
| `sb_raise_query_error(...)` | `R/client.R:615` | Constructs and signals the condition |
| `parse_error_message(...)` | `R/protocol.R:590` | Parses the server error frame |

The condition carries `sqlstate`, `detail`, and `hint` fields.

**Retry guidance:**

```r
scope <- sb_retry_scope_for_sqlstate("40001")
# Returns "statement" — retry fresh statement only
```

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) for
the full SQLSTATE class map.

---

## Pooling and concurrency

The manifest records `connection_thread_confined` thread-safety and
`session_pool` pooling capability. Each `ScratchbirdConnection` object
confines its transport and session state to the creating thread. Do not
share a `ScratchbirdConnection` across R threads or `parallel` workers;
create a separate connection per worker instead.

The `session_pool` capability means connections can be held in an
application-managed pool and reused across requests via `dbConnect` /
`dbDisconnect` lifecycle management.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md) for
ScratchBird pooling and session concurrency concepts.

---

## Reconnection and resilience

The driver follows the MGA recovery contract:

- `sb_prepare_connection(client)` resets abandoned transaction and
  prepared-statement state before re-entering the startup/auth sequence.
- Reconnect repairs transport and session state only; it never
  resurrects in-flight transactions or replays lost statements.
- Transaction recovery means reset, rollback, reopen, or retry against
  engine truth.

`sb_is_valid(client)` (exposed as `dbIsValid(conn)`) tests whether the
underlying transport is still live.

---

## Running the test suite

Local deterministic tests (no live server required):

```bash
Rscript -e "pkgload::load_all(quiet=TRUE); testthat::test_dir('tests/testthat')"
```

Live integration tests are environment-gated:

| Environment variable | Purpose |
|---|---|
| `SCRATCHBIRD_R_URL` | Direct-connect integration coverage |
| `SCRATCHBIRD_R_MANAGER_URL` | Manager-proxy connect/query coverage |
| `SCRATCHBIRD_R_CANCEL_SQL` | Cancel/drain lifecycle coverage |

---

## Conformance

Conformance profile reference: `driver_r_gate`.

Implementation status per baseline requirement mapping:

| Area | Status |
|---|---|
| CONN (connection/auth) | Partial — offline tests pass; live coverage is environment-gated |
| TXN (transactions) | Partial — DBI lifecycle implemented; live server proof pending |
| EXEC (execution) | Implemented |
| META (metadata) | Partial — recursive schema shaping implemented; richer metadata families pending |
| TYPE (type mapping) | Implemented |
| ERR (error/diagnostics) | Implemented |
| RES (resource cleanup) | Implemented |

See [../conformance_baseline.md](../conformance_baseline.md) for the
baseline conformance definition.

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1 wire protocol
- [../type_mapping.md](../type_mapping.md) — SBsql type mapping
- [../metadata_sys_information.md](../metadata_sys_information.md) — sys.information metadata
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — SQLSTATE reference
- [../pooling_and_concurrency.md](../pooling_and_concurrency.md) — Pooling and concurrency
- [../conformance_baseline.md](../conformance_baseline.md) — Conformance baseline
