# ScratchBird R Driver

R DBI-compatible driver for ScratchBird using the ScratchBird wire protocol.

## Documentation

- Lane baseline requirement mapping (S0): [BASELINE_REQUIREMENT_MAPPING.md](BASELINE_REQUIREMENT_MAPPING.md)
- Getting started
- API reference

## Auth / Bootstrap Surface

Public staged auth/bootstrap entry points:

- `sb_probe_auth_surface(dsn, ...)`
- `sb_get_resolved_auth_context(client)`

Implemented native auth/bootstrap classes:

- `PASSWORD`
- `SCRAM_SHA_256`
- `SCRAM_SHA_512`
- `TOKEN`
- `manager_proxy` token bootstrap

Negotiated but fail-closed when locally unsupported:

- `MD5`
- `PEER`
- `REATTACH`

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `sb_prepare_transaction(...)`, `sb_commit_prepared(...)`, and
  `sb_rollback_prepared(...)` expose prepared/limbo control SQL explicitly in
  lane source
- `sb_supports_dormant_reattach(...)` is explicit and
  `sb_detach_to_dormant(...)` / `sb_reattach_dormant(...)` fail closed with
  `0A000` instead of treating reconnect as dormant resume
- `sb_begin(...)` exposes the canonical MGA begin payload fields for
  `isolation_level`, `access_mode`, `deferrable`, `wait`, `timeout_ms`,
  `autocommit_mode`, `conflict_action`, and `read_committed_mode`
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction activity in this lane; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` reopen the next boundary
- `sb_begin(...)` is documented against that always-in-transaction contract
  rather than idle-session semantics
- result fetch ignores one stray reopen `READY` before any real result
  material so the first post-commit / post-rollback query sees actual rows
  instead of an empty-response misclassification
- `sb_canonical_isolation_label(...)` makes the current alias mapping explicit
  in lane source: `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `sb_canonical_read_committed_mode_label(...)` now makes the canonical
  `READ COMMITTED` sub-mode selector explicit in lane source, including
  `READ COMMITTED READ CONSISTENCY`
- `sb_retry_scope_for_sqlstate(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay

See `../../../../public_audit_summary`.

## Build/Test (Windows/Linux)

See `docs/BUILD_MATRIX.md`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

Dependencies: `DBI`, `openssl` (tests: `testthat`).

## Usage

```r
library(DBI)
library(scratchbird)

con <- dbConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb")
df <- dbGetQuery(con, "SELECT 1")
res <- dbSendQuery(con, "SELECT 1 AS value")
info <- dbColumnInfo(res)
dbClearResult(res)
dbDisconnect(con)
```

## Connection strings

URI:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value:

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

Manager-proxy URI (live integration path):

```
scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token
```

## Integration Tests

Live integration tests are environment-gated:

- `SCRATCHBIRD_R_URL` for direct-connect integration coverage.
- `SCRATCHBIRD_R_MANAGER_URL` for manager-proxy connect/query coverage.
- `SCRATCHBIRD_R_CANCEL_SQL` for cancel/drain lifecycle coverage.
- the focused native integration file now also proves direct post-rollback
  query correctness on the fresh MGA boundary

Local deterministic suite without live DSNs:

```bash
Rscript -e "pkgload::load_all(quiet=TRUE); testthat::test_dir('tests/testthat')"
```
