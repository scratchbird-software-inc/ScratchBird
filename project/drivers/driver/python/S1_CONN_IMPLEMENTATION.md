# S1 CONN Implementation (DLB-PYTHON-002)

Scope: `lanes/active/drivers/python` only.

## What Changed

- Extended DSN key-value parsing to accept both whitespace and semicolon separators in `src/scratchbird/dsn.py`.
- Expanded connection config alias handling in `src/scratchbird/connection.py` for:
  - `dbname` -> `database`
  - `username` -> `user`
  - `connecttimeout` -> `connect_timeout`
  - `sockettimeout` -> `socket_timeout`
  - `applicationname` -> `application_name`
  - `binarytransfer` -> `binary_transfer`
- Added auth startup config fields on `ConnectionConfig`:
  - `auth_token`
  - `auth_method_id`
  - `auth_payload_json`
  - `auth_payload_b64`
  - `auth_provider_profile`
- Added `_build_startup_params()` and wired `_startup_and_auth()` to apply protocol auth plugin selection (`apply_auth_plugin_selection`) before sending `STARTUP`.
- Added staged auth/bootstrap surfaces in `src/scratchbird/connection.py`:
  - public `probe_auth_surface(...)`
  - public `get_resolved_auth_context()`
  - direct probe visibility for admitted auth method families
  - manager-proxy probe visibility for managed token bootstrap
- Expanded direct auth execution in `_startup_and_auth()` for:
  - `PASSWORD`
  - `SCRAM_SHA_256`
  - `SCRAM_SHA_512`
  - generic `TOKEN`
- Added fail-closed unsupported/broker-required handling for admitted-but-not-local
  auth methods:
  - `MD5`
  - `PEER`
  - generic `REATTACH`
- Added fail-fast validation in `_connect()` so `front_door_mode=manager_proxy` without `manager_auth_token` errors before any socket connect attempt.
- Added runtime session-schema parity helpers in `src/scratchbird/connection.py`:
  - `get_session_schema()` returns the normalized active session-schema setting.
  - `set_session_schema(schema)` updates local session-schema state and executes `SET SCHEMA`/`SET SEARCH_PATH` (resetting `None` to `public`).
- Added JDBC-style liveness helper in `src/scratchbird/connection.py`:
  - `is_valid(timeout_ms=0)` returns a boolean health probe backed by `ping()`.
  - Closed connections return `False`; negative timeout raises `ProgrammingError`.
  - When possible, timeout is applied/restored via socket timeout mutation around `ping()`.
- Added targeted unit tests in `tests/test_connection_auth_protocol.py` covering:
  - semicolon DSN parsing
  - alias-based connection config normalization
  - generic auth-token DSN/config capture
  - auth startup field capture
  - direct auth-surface probing
  - manager-proxy auth-surface probing
  - top-level probe/config bootstrap
  - startup auth plugin parameter assembly
  - SCRAM-SHA-512 auth negotiation
  - generic TOKEN auth negotiation
  - fail-closed PEER auth handling
  - invalid auth method namespace handling
  - manager proxy token fail-fast behavior
- Added targeted unit tests in `tests/test_txn_exec_parity.py` covering:
  - runtime session-schema set/get behavior
  - session-schema reset-to-public behavior
  - unchanged session-schema no-op behavior
  - non-string session-schema input validation
- Added env-gated live integration coverage in `tests/test_integration.py`:
  - `test_session_schema_runtime_integration` validates runtime session-schema transitions against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_connection_ping_integration` validates wire-level connection liveness checks against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
  - `test_connection_is_valid_integration` validates boolean liveness probing and closed-connection behavior against a live endpoint when `SCRATCHBIRD_TEST_DSN` is configured.
- Added JDBC policy-alignment updates in `src/scratchbird/dsn.py` and `src/scratchbird/connection.py`:
  - non-native `protocol|parser|dialect` hints are accepted and normalized to native mode.
  - `sslmode=disable` now opens plaintext transport without TLS wrapping.
  - `compression=zstd` is accepted; unknown compression values fail fast.
  - `binary_transfer=false` is accepted and represented in startup/query behavior.
- Added deterministic always-on runtime contract coverage in `tests/test_runtime_contract_gate.py` so connection policy parity is verified without environment variables.

## Test Commands Run

1. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_connection_auth_protocol.py`
- Result: PASS (`17 passed`)

2. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_sql.py lanes/active/drivers/python/tests/test_types.py`
- Result: PASS (`66 passed`)

3. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_txn_exec_parity.py`
- Result: PASS (`49 passed`)

4. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_sql.py lanes/active/drivers/python/tests/test_connection_auth_protocol.py lanes/active/drivers/python/tests/test_txn_exec_parity.py lanes/active/drivers/python/tests/test_integration.py lanes/active/drivers/python/tests/test_types.py`
- Result: PASS (`132 passed, 27 skipped`)

5. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_integration.py`
- Result: PASS (`27 skipped`) when `SCRATCHBIRD_TEST_DSN` is not configured.

6. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests/test_runtime_contract_gate.py`
- Result: PASS (`3 passed`)

7. `PYTHONDONTWRITEBYTECODE=1 pytest -q lanes/active/drivers/python/tests`
- Result: PASS (`267 passed, 27 skipped, 1 warning`)

## CONN Status Recommendation

- Recommendation: `IMPLEMENTED`
- Reason:
  - Connection/config parity now includes non-native protocol-hint normalization, TLS and plaintext transport modes, staged auth-surface probing, `binary_transfer`/`compression` policy parity, startup/auth parameter assembly, direct `PASSWORD`/`SCRAM_SHA_256`/`SCRAM_SHA_512`/`TOKEN` execution, and manager-proxy fail-fast plus managed bootstrap visibility.
  - Runtime connection behavior is covered by deterministic lane tests plus always-on runtime contract gate assertions, so parity no longer depends exclusively on env-gated integration runs.
