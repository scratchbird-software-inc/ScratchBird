# ScratchBird Python Driver

ScratchBird DB-API 2.0 driver using the ScratchBird native wire protocol.

## Documentation

- Getting started
- API reference
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## Beta Readiness Surface

- manifest identity/status is exported by `beta_driver_readiness_status()`
  (`driver:python`, package UUID `019e12a0-0012-7000-8000-000000000012`,
  `beta_2`, `driver_python_gate`)
- runtime mapping follows DB-API 2.0 over direct listener or `manager_proxy`
  with `sbwp_v1_1`, `native_sqlstate`, and recursive `sys_information`
  metadata
- `validate_advisory_cache_context(...)` and
  `validate_prepared_bundle_reuse(...)` refuse stale policy, schema,
  language, capability, authorization, database, or transaction contexts
- driver-local SBLR, UUID, and result caches are advisory only; server
  revalidation remains required before execution, and transaction finality
  remains owned by the engine MGA transaction inventory
- `resolve_language_profile(...)` and `validate_language_resource_state(...)`
  select supported language resources or fall back to standard English

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `prepare_transaction(...)`, `commit_prepared(...)`, and
  `rollback_prepared(...)` now expose explicit prepared / limbo control
  surfaces through canonical transaction-control SQL
- `supports_dormant_reattach()` is explicit and true on the native public
  lane, `detach_to_dormant()` returns the engine-issued `dormant_id` plus
  `dormant_reattach_token`, and `reattach_dormant(...)` uses those explicit
  startup parameters on reconnect instead of implying reconnect-based recovery
- `begin(...)` exposes the canonical MGA begin payload fields for
  `isolation_level`, `access_mode`, `deferrable`, `wait`, `timeout_ms`,
  `autocommit_mode`, `conflict_action`, and `read_committed_mode`
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative
  transaction-state surfaces; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` immediately reopen the next boundary
- `autocommit` mode transitions are local driver policy on the native lane;
  the Python wrapper does not push a synthetic wire `SET_OPTION autocommit`
  or client-side `BEGIN` against the server-owned session boundary
- `canonical_isolation_label(...)` makes the current alias mapping explicit in
  lane source: `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `canonical_read_committed_mode_label(...)` plus the exported
  `READ_COMMITTED_MODE_*` constants make the canonical `READ COMMITTED`
  sub-modes explicit in lane source; `READ_COMMITTED_MODE_READ_CONSISTENCY`
  now selects canonical `READ COMMITTED READ CONSISTENCY`
- `retry_scope_for_sqlstate(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- staged auth/bootstrap is now explicit in the public lane through
  `probe_auth_surface(...)` and `get_resolved_auth_context()`, with direct
  auth execution covering `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and
  generic `TOKEN`
- internal result paging now enables portal resume only after
  `PORTAL_SUSPENDED`, and `_resume_suspended_portal(...)` rejects unsuspended
  resume with `55000`
- native batched `executemany(...)` now reuses session-local prepared
  statement shapes for repeated multi-row `INSERT ... VALUES` batches, so
  high-volume loads do not pay parse/describe cost on every identical batch
- native batched `executemany(...)` now admits larger multi-row batches by
  default, capped by both total placeholder count and generated SQL text
  size, and tuned to stay inside the current native front-door acceptance
  envelope, so high-volume loads reduce per-statement overhead without
  emitting unbounded batch text or tripping the live parser limit

See `../../../../public_audit_summary`.

## Build/Test (Windows/Linux)

See `docs/BUILD_MATRIX.md`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

## Development

```bash
python -m pip install -e .
```

## Testing

Unit tests:

```bash
python -m pip install -e ".[test]"
pytest
```

Integration tests (requires a running server and a DSN):

```bash
export SCRATCHBIRD_TEST_DSN="scratchbird://user:pass@localhost:3092/mydb"
pytest python/tests/test_integration.py
```

## Packaging

Build a wheel/sdist:

```bash
python -m pip install build
python -m build
```

## Publish

```bash
python -m pip install twine
twine upload dist/*
```
