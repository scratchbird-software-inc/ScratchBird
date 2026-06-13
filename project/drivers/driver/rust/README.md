# ScratchBird Rust Driver

Async Rust driver for ScratchBird using the native wire protocol.

## Documentation

- Getting started
- API reference
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## Beta Readiness Surface

- manifest identity/status is exported by `beta_driver_readiness_status()`
  (`driver:rust`, package UUID `019e12a0-0015-7000-8000-000000000015`,
  `beta_2`, `driver_rust_gate`)
- runtime mapping follows the native language binding over direct listener or
  `manager_proxy` with `sbwp_v1_1`, `native_sqlstate`, and recursive
  `sys_information` metadata
- `validate_advisory_cache_context(...)` and
  `validate_prepared_bundle_reuse(...)` refuse stale policy, schema,
  language, capability, authorization, database, or transaction contexts
- driver-local SBLR, UUID, and result caches are advisory only; server
  revalidation remains required before execution, and transaction finality
  remains owned by the engine MGA transaction inventory
- `resolve_language_profile(...)` and `validate_language_resource_state(...)`
  select supported language resources or fall back to standard English

## Auth And Bootstrap Contract

This lane now implements the shared staged auth/bootstrap contract.

- public staged probe: `probe_auth_surface(dsn)` and `client.probe_auth_surface().await`
- resolved auth reporting: `client.get_resolved_auth_context()`
- direct/native executable auth methods:
  - `PASSWORD`
  - `SCRAM_SHA_256`
  - `SCRAM_SHA_512`
  - `TOKEN`
- manager/front-door bootstrap:
  - `front_door_mode=manager_proxy`
  - `manager_auth_token`
  - deterministic MCP hello/auth/db-connect flow
- fail-closed negotiated-but-not-local methods:
  - `MD5`
  - `PEER`
  - `REATTACH`

Canonical payload keys exposed in the config/DSN surface include:

- `auth_token`
- `auth_method_payload`
- `auth_payload_json`
- `auth_payload_b64`
- `workload_identity_token`
- `proxy_principal_assertion`

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `Client::prepare_transaction(...)`, `commit_prepared(...)`, and
  `rollback_prepared(...)` expose prepared/limbo control SQL explicitly in
  lane source
- `supports_dormant_reattach()` is explicit and `detach_to_dormant(...)` /
  `reattach_dormant(...)` fail closed with `0A000` instead of implying that
  reconnect can recover dormant work
- `TxnBeginOptions` exposes the canonical MGA begin flags for
  `isolation_level`, `access_mode`, `deferrable`, `wait`, `timeout_ms`,
  `autocommit_mode`, `conflict_action`, and `read_committed_mode`
- current isolation alias mapping is explicit in lane source:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- the public `READ_COMMITTED_MODE_*` constants plus
  `canonical_read_committed_mode_label(...)` make the canonical
  `READ COMMITTED` sub-modes explicit in lane source; `read_committed_mode`
  now exposes `READ COMMITTED READ CONSISTENCY` directly
- `retry_scope_for_sqlstate(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- native `READY` / `TXN_STATUS` frames plus `current_txn_id` own transaction
  activity in lane source; ScratchBird sessions stay always in a transaction
  and `COMMIT` / `ROLLBACK` reopen the next boundary
- `begin(...)` restarts the current boundary with the requested options
  instead of assuming idle-session semantics
- native autocommit transitions stay local to the wrapper instead of sending
  `SET_OPTION autocommit` against a server-owned fresh boundary

See `../../../../public_audit_summary`.

## Build/Test (Windows/Linux)

See `docs/BUILD_MATRIX.md`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

## Usage

```rust
use scratchbird::{Client, Config};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut client = Client::new(Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb"
    )?);
    client.connect().await?;
    let result = client.query("SELECT 1").await?;
    println!("{:?}", result.rows[0][0]);
    client.close().await;
    Ok(())
}
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
