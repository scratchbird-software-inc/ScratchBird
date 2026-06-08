# S2 TXN/EXEC Implementation (DLB-PYTHON-003)

Scope: `lanes/active/drivers/python` lane only.

## Changes

- Added lane-local transaction guardrails in `src/scratchbird/connection.py`:
  - `begin()` now rejects nested begin when a transaction is already active.
  - `commit()` and `rollback()` now no-op when no transaction is active (avoids unnecessary wire calls on txn id `0`).
  - `savepoint()`, `release_savepoint()`, and `rollback_to_savepoint()` now require an active transaction and validate savepoint names.
- Added JDBC-aligned autocommit transition behavior in `src/scratchbird/connection.py`:
  - `autocommit=True` now commits an active transaction before switching modes.
  - `autocommit` mode transitions now emit wire-level session updates via `SET_OPTION autocommit=on/off`.
  - `autocommit=False` now eagerly starts a transaction when no transaction is active.
  - No-op transitions (`autocommit` already set to requested value) now short-circuit.
- Added env-gated live integration coverage in `tests/test_integration.py`:
  - `test_transaction_begin_commit_rollback_cycle_integration` validates explicit begin/commit/rollback behavior against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_transaction_nested_begin_rejected_integration` validates nested-begin rejection after explicit begin against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_autocommit_mode_transition_integration` validates runtime autocommit toggles against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_transaction_savepoint_lifecycle_integration` validates savepoint lifecycle behavior against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
- Added env-gated live EXEC integration coverage in `tests/test_integration.py`:
  - `test_cursor_get_generated_keys_integration` validates direct generated-keys retrieval against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_cursor_nextset_integration` validates DB-API multi-result traversal against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_connection_call_callable_escape_integration` validates callable escape execution through `Connection.call(...)` against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_query_multi_summary_shape_integration` validates structured multi-result summary payload shape against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_execute_batch_summary_shape_integration` validates structured batch summary payload shape against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_query_batch_alias_integration` validates `query_batch(...)` alias behavior against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
- Added execution parity helper in `src/scratchbird/connection.py`:
  - `native_sql(sql, params=None)` returns normalized/native SQL rewrite without executing.
- Hardened parameter error behavior for execution paths in `src/scratchbird/connection.py`:
  - `_execute_query()` now maps SQL normalization `ValueError` into DB-API `ProgrammingError`.
- Added lane-local execution input validation in `src/scratchbird/cursor.py`:
  - `executemany(..., seq_of_params)` now raises `ProgrammingError` when `seq_of_params` is `None`.
- Added batched native execute reuse in `src/scratchbird/connection.py` and
  `src/scratchbird/cursor.py`:
  - repeated multi-row `INSERT ... VALUES` shapes emitted by
    `Cursor.executemany(...)` now reuse a session-local prepared statement
    handle instead of reparsing the same batch SQL every time.
  - the Python lane now admits materially larger default batch sizes for
    multi-row inserts, bounded by both total placeholder count and generated
    SQL text size, with the placeholder ceiling tuned below the current
    native front-door invalid-query boundary so high-volume loads reduce
    per-statement overhead without turning `executemany(...)` into an
    unbounded statement generator or tripping the live parser limit.
- Implemented command-complete generated-key parity in `src/scratchbird/connection.py` and `src/scratchbird/cursor.py`:
  - `ResultStream` now captures `COMMAND_COMPLETE.last_id` as `lastrowid`.
  - Cursor drain paths (`fetchone()` completion and `executemany()`) now propagate stream `lastrowid` consistently.
- Fixed named parameter normalization around cast syntax in `src/scratchbird/sql.py`:
  - `::` cast markers are no longer misinterpreted as named placeholders.
- Added targeted tests:
  - Extended `tests/test_txn_exec_parity.py` with result-stream `last_id` to `lastrowid` propagation and `executemany` final-`lastrowid` behavior checks.
  - Extended `tests/test_sql.py` with a cast-syntax rewrite regression test.
- Added callable and multi-result execution parity in `src/scratchbird/connection.py`, `src/scratchbird/cursor.py`, and `src/scratchbird/sql.py`:
  - `native_callable_sql(sql, params=None)` and `call(sql, params=None)` now expose callable normalization/execution on `Connection`.
  - `Cursor.callproc(procname, params=None)` now routes through callable normalization with placeholder rewriting.
  - `ResultStream` now tracks result-set boundaries and exposes next-result navigation; `Cursor.nextset()` now advances across result sets.
- Added first-class batch execution summaries in `src/scratchbird/connection.py`:
  - `execute_batch(sql, batch_params)` now returns per-item summaries (`index`, `rowCount`, `fields`, `command`, `lastId`) plus `totalRowCount`.
  - `query_batch(sql, batch_params)` now aliases `execute_batch(...)`.
- Added first-class multi-result summary helpers in `src/scratchbird/connection.py`:
  - `query_multi(sql, params)` now returns all result sets as structured summaries (`rows`, `rowCount`, `fields`, `command`, `lastId`).
  - `execute_multi(sql, params)` now aliases `query_multi(...)`.
- Added status-message propagation in `src/scratchbird/connection.py` and `src/scratchbird/cursor.py`:
  - `ResultStream` now captures `COMMAND_COMPLETE.tag` as `command`.
  - Cursor drain paths now expose that as `cursor.statusmessage`.
- Added dedicated generated-keys result-set API in `src/scratchbird/cursor.py` and `src/scratchbird/connection.py`:
  - `Cursor.get_generated_keys()` now returns a generated-keys result-set object (`fetchone/fetchmany/fetchall`, `description`, `rowcount`).
  - Generated keys are accumulated across execute, executemany, and multi-result boundaries.
  - `Connection.execute_with_generated_keys(sql, params)` now provides convenience execution + generated-keys retrieval.
- Added callable and next-result tests:
  - Extended `tests/test_txn_exec_parity.py` with `Connection.call`, `native_callable_sql`, and `Cursor.callproc/nextset` coverage.
  - Extended `tests/test_sql.py` with JDBC escape callable normalization coverage.
- Added batch/statusmessage tests:
  - Extended `tests/test_txn_exec_parity.py` with `Connection.execute_batch/query_batch` coverage and status-message assertions.
- Added generated-keys tests:
  - Extended `tests/test_txn_exec_parity.py` with `Cursor.get_generated_keys` and `Connection.execute_with_generated_keys` coverage, including multi-result accumulation.
- Added multi-result summary tests:
  - Extended `tests/test_txn_exec_parity.py` with `Connection.query_multi/execute_multi` coverage.
- Updated TXN/EXEC rows in `BASELINE_REQUIREMENT_MAPPING.md` with current evidence and status notes.
- Added deterministic binary-transfer toggle parity tests in `tests/test_txn_exec_parity.py`:
  - `test_send_simple_query_respects_binary_transfer_toggle`
  - `test_send_extended_query_uses_text_result_format_when_binary_transfer_disabled`
  - `test_send_cached_extended_query_reuses_prepared_statement_for_identical_sql_shape`
- Added always-on runtime TXN/EXEC contract coverage in `tests/test_runtime_contract_gate.py`:
  - `test_runtime_gate_txn_exec_without_env` validates transaction/savepoint lifecycle and multi-result execution flow without env-gated integration dependencies.

## Tests Run

1. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_integration.py`
- Result: PASS (`27 skipped`) when `SCRATCHBIRD_TEST_DSN` is not configured.

2. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_txn_exec_parity.py`
- Result: PASS (`49 passed`)

3. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_runtime_contract_gate.py`
- Result: PASS (`3 passed`)

4. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests`
- Result: PASS (`214 passed, 27 skipped, 1 warning`)

## TXN Status

- Recommendation: `IMPLEMENTED`
- Reason:
  - Explicit begin/commit/rollback/savepoint APIs now have deterministic local guardrails and focused unit coverage.
  - `autocommit` transition semantics now align better with JDBC (`autocommit=True` commits an active transaction before mode switch).
  - Wire-level autocommit mode transitions are now emitted via `SET_OPTION autocommit=on/off`.
  - `autocommit=False` now starts a transaction when no transaction is active.
  - Deterministic always-on runtime contract coverage now includes explicit transaction/savepoint lifecycle validation without environment gating.

## EXEC Status

- Recommendation: `IMPLEMENTED`
- Reason:
  - Execution normalization and dispatch parity now includes `native_sql`/`native_callable_sql`, callable execution (`Connection.call` / `Cursor.callproc`), normalization-error mapping to DB-API `ProgrammingError`, cast-safe named parameter rewrite, explicit `executemany` input validation, repeated batched-insert prepared-shape reuse on the native lane, first-class batch summaries (`execute_batch`/`query_batch`), first-class multi-result summaries (`query_multi`/`execute_multi`), dedicated generated-keys result-set retrieval (`get_generated_keys` / `execute_with_generated_keys`), generated-key propagation (`COMMAND_COMPLETE.last_id` to `cursor.lastrowid`), command-tag propagation (`cursor.statusmessage`), and multi-result traversal via `Cursor.nextset()`, all with lane-local tests.
  - Deterministic always-on runtime contract coverage now validates transaction/multi-result wire behavior and binary-result toggles without environment gating.
