# Example / Test Database

This directory is a **test harness**. It builds a fully populated example
ScratchBird database by running an ordered series of native **SBsql** scripts
through the `sb_isql` command-line client against an already-running ScratchBird
server, exercising as much of the feasible single-node DDL and DML command
surface as practical.

Its purpose is twofold: (1) populate an example database, and (2) **report every
error**. The scripts deliberately exercise the documented command surface as
written. If a statement or object turns out to have no actual SBsql support, that
is a finding — the run records it as an error rather than working around it. The
errors are a deliverable.

## Run it

```bash
sudo ./run_example_database.sh
```

The runner connects to a running server and executes every `sql/*.sql` script in
numeric order. By default it runs **everything** (it does not stop at the first
failure) so that all errors are collected in one pass.

For each script it writes, under `logs/`:

- `<script>.out.log` — results and echoed statements (sb_isql `-o` output)
- `<script>.err.log` — error output (sb_isql `SET ERROR` directive)

and at the end aggregates every error into `logs/errors_summary.log`. The process
exits non-zero if any errors were reported.

> Input / output / error are routed through sb_isql's own directives: input via
> `-f`, output via `-o`, and errors via the `SET ERROR <file>` line directive
> (sb_isql has no error-file command-line flag, so the runner injects the
> directive the same way `SET TERM` is used).

## Configuration

Defaults match the standard local connection; override any with environment
variables:

| Variable | Default | Meaning |
| --- | --- | --- |
| `SB_BIN` | autodetected | Directory containing `sb_isql` (build output / install) |
| `SB_HOST` | `127.0.0.1` | Server host |
| `SB_PORT` | `3092` | Server TCP port |
| `SB_USER` | `alice` | Login user |
| `SB_PASSWORD` | `scratchbird` | Login password |
| `SB_ROLE` | `sysarch` | Connection role (passed as `--conn-opt role=`) |
| `SB_SSLMODE` | `require` | TLS mode (`disable\|allow\|prefer\|require\|verify-ca\|verify-full`) |
| `SB_DB` | `example_db` | Target database name/path |
| `SB_CREATE_DB` | `0` | `1` = run `sql/00_create_database.sql` first |
| `SB_STOP_ON_ERROR` | `0` | `1` = stop at the first script that reports an error |
| `SB_BAIL_IN_SCRIPT` | `0` | `1` = stop a script at its first failing statement (`-b`) |
| `SB_RUN_REFUSALS` | `0` | `1` = also run `sql/expected_refusals/` (inverted scoring; see below) |
| `SB_TEARDOWN` | `0` | `1` = run `90_teardown.sql` at the end (drops everything) |
| `SB_LOG_DIR` | `./logs` | Where per-script `.out.log`/`.err.log` files are written |
| `SB_ERROR_REGEX` | `^Error` | Pattern that marks a failed statement in the error output |

By default the runner creates (optionally) and **populates** the database and
**leaves it populated** — teardown and the refusal pass are opt-in.

Example:

```bash
SB_BIN=/opt/scratchbird/bin SB_DB=example_db SB_CREATE_DB=1 \
    sudo -E ./run_example_database.sh
```

## Script order

| Script | Purpose |
| --- | --- |
| `00_create_database.sql` | (optional) create the database |
| `10_schemas.sql` | schemas (recursive schema tree) |
| `11_domains.sql` | domains |
| `12_sequences.sql` | sequences |
| `13_tables.sql` | tables: all feasible column types and constraints, comments |
| `14_indexes.sql` | indexes (b-tree, unique, and specialized index types) |
| `15_views.sql` | views and materialized views |
| `20_functions.sql` | functions (procedural SQL) |
| `21_procedures.sql` | procedures (blocks, control flow, cursors, exceptions) |
| `22_triggers.sql` | triggers and event triggers |
| `23_security.sql` | roles, groups, grants, policies, masks, row-level security |
| `30_insert.sql` | inserts (single, multi-row, INSERT … SELECT, RETURNING) |
| `31_update_delete_merge.sql` | updates, deletes, merge / upsert |
| `32_queries.sql` | queries: joins, CTEs, set operations, window, group/having |
| `33_transactions.sql` | transactions, savepoints, isolation |
| `34_multimodel.sql` | document, vector, graph, key-value, time-series, search |
| `35_builtin_functions.sql` | built-in function packages exercised in queries |
| `36_catalog_introspection.sql` | catalog (`sys.catalog.*`/`sys.security.*`) introspection, `SHOW` |
| `37_casts.sql` | type conversions: every **supported** explicit `cast`/`try_cast` per the conversion matrix |
| `40_alter.sql` | `ALTER`, `RENAME`, `COMMENT`, `RECREATE` |
| `90_teardown.sql` | drop all created objects (opt-in: `SB_TEARDOWN=1`) |
| `expected_refusals/cast_refusals.sql` | conversions the engine should **refuse** (opt-in: `SB_RUN_REFUSALS=1`) |

### Type and cast coverage

`13_tables.sql` declares a column of every confirmed scalar/temporal/binary/uuid/
json/vector type (`app.catalog.type_demo`). `37_casts.sql` then exercises the
**conversion matrix** (`data_types/conversion_matrix.md`): integer widening,
signed/unsigned, integer↔decimal, numeric↔text, boolean↔text, text↔temporal,
temporal↔temporal, text↔uuid, uuid↔binary(16), scalar↔document, and domain
casts — plus `try_cast`. Conversions that need a named function (e.g. binary↔text
via `encode`/`decode`) or unconfirmed literal syntax (array/vector construction)
are marked `OMITTED` in the file and listed in `TEST_FINDINGS.md`.

The **expected-refusals** set (`SB_RUN_REFUSALS=1`) covers conversions the matrix
documents as *refused* (fractional decimal→int, `timestamptz`→`timestamp`,
text truncation, NaN/Infinity→exact, negative→unsigned, out-of-range narrowing,
invalid text→numeric/uuid/temporal, domain-check violations). These run with
**inverted scoring**: each statement is expected to error, so a statement that
*succeeds* is the finding (a silent lossy/unsafe conversion). The refusal pass
runs before teardown because it references the example domains.

## Notes

- These scripts use only native SBsql. They avoid cluster/distributed
  statements (not feasible on a single-node server) and operations that write to
  the host filesystem.
- This is a **test harness**. Whether every statement executes depends on the
  build, configuration, and policy; the `.out.log`/`.err.log` files and
  `errors_summary.log` in `logs/` show exactly what ran and what was refused.
- Constructs whose SBsql support could not be confirmed from the documented
  command surface at authoring time are listed in `TEST_FINDINGS.md`. They are
  left in the scripts on purpose so that running the harness confirms or refutes
  support; the corresponding fixes belong in the engine/parser, not here.
