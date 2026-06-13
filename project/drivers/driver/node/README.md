# ScratchBird Node.js/TypeScript Driver

Native ScratchBird driver for Node.js with full TypeScript types.

## Documentation

- Getting started
- API reference
- Baseline requirement mapping: [`BASELINE_REQUIREMENT_MAPPING.md`](BASELINE_REQUIREMENT_MAPPING.md)

## Beta Readiness Surface

- manifest identity/status is exported by `betaDriverReadinessStatus()`
  (`driver:node`, package UUID `019e12a0-0008-7000-8000-000000000008`,
  `beta_2`, `driver_node_gate`)
- runtime mapping follows the native language binding over direct listener or
  `manager_proxy` with `sbwp_v1_1`, `native_sqlstate`, and recursive
  `sys_information` metadata
- `validateAdvisoryCacheContext(...)` and
  `validatePreparedBundleReuse(...)` refuse stale policy, schema, language,
  capability, authorization, database, or transaction contexts
- driver-local SBLR, UUID, and result caches are advisory only; server
  revalidation remains required before execution, and transaction finality
  remains owned by the engine MGA transaction inventory
- `resolveLanguageProfile(...)` and `validateLanguageResourceState(...)`
  select supported language resources or fall back to standard English

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- the internal portal-resume path now fails closed with `55000` unless the
  server first reported `PORTAL_SUSPENDED`
- same-client reconnect discards prepared handles, attachment parameters, and
  cached plan/SBLR frames from the abandoned session before the new handshake
- `prepareTransaction(...)`, `commitPrepared(...)`, and
  `rollbackPrepared(...)` expose explicit prepared/limbo control through
  canonical transaction-control SQL rather than reconnect heuristics
- `supportsDormantReattach()` is explicit and true on the native public lane,
  `detachToDormant()` returns the engine-issued `dormantId` plus
  `reattachToken`, and `reattachDormant(...)` uses those same explicit
  tokens through the public/native startup contract instead of implying
  reconnect-based recovery
- `beginTransaction(options)` exposes the canonical MGA begin flags for
  `isolationLevel`, `accessMode`, `deferrable`, `wait`, `timeoutMs`,
  `autocommitMode`, `conflictAction`, and `readCommittedMode`
- native `READY`, `TXN_STATUS`, and `current_txn_id` are treated as
  authoritative transaction-state surfaces; ScratchBird sessions stay always
  in a transaction and `COMMIT` / `ROLLBACK` reopen the next boundary
- `beginTransaction(options)` restarts the current boundary with the
  requested options instead of assuming idle-session semantics
- native autocommit transitions stay local to the wrapper instead of sending
  `SET_OPTION autocommit` or a synthetic replacement `BEGIN`
- current isolation alias mapping is explicit in lane source:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- the public `READ_COMMITTED_MODE_*` constants plus
  `canonicalReadCommittedModeLabel(...)` make the canonical `READ COMMITTED`
  sub-modes explicit in lane source; `readCommittedMode` now exposes
  `READ COMMITTED READ CONSISTENCY` directly
- `retryScopeForSqlState(...)` makes the retry boundary explicit:
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

## Install

```bash
npm install scratchbird
```

## Usage

```ts
import { Client } from "scratchbird";

const client = new Client({
  host: "localhost",
  port: 3092,
  user: "user",
  password: "pass",
  database: "db",
});

await client.connect();
const res = await client.query("select 1 as one");
console.log(res.rows);
await client.end();
```

## SSL/TLS

```ts
const client = new Client({
  host: "localhost",
  user: "user",
  password: "pass",
  database: "db",
  sslmode: "verify-full",
  sslrootcert: "/etc/ssl/certs/ca.pem",
});
```

## Tests

```bash
npm install
npm test
```

Integration test:

```bash
export SCRATCHBIRD_NODE_URL="scratchbird://user:pass@localhost:3092/db"
node --test node/test/integration.test.js
```
