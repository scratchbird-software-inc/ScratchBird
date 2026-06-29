# ScratchBird Dart Driver

Native Dart/Flutter driver for ScratchBird (SBWP v1.1).

## Lane Docs

- [Baseline Requirement Mapping (S0)](./BASELINE_REQUIREMENT_MAPPING.md)
- Getting started
- API reference

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `begin(...)` exposes the canonical MGA begin flags for `isolationLevel`,
  `readCommittedMode`, `accessMode`, `deferrable`, `wait`, `timeoutMs`,
  `autocommitMode`, and `conflictAction`
- current isolation alias mapping is explicit in lane source:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `readCommittedModeReadConsistency` plus
  `canonicalReadCommittedModeLabel(...)` now make the canonical
  `READ COMMITTED` sub-modes explicit in lane source and expose
  `READ COMMITTED READ CONSISTENCY` directly
- `retryScopeForSqlState(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- prepared / limbo truth is explicit in lane code through
  `supportsPreparedTransactions()`, `prepareTransaction(...)`,
  `commitPrepared(...)`, and `rollbackPrepared(...)`, which emit canonical
  transaction-control SQL rather than reconnect folklore
- dormant detach / reattach truth is explicit in lane code through
  `supportsDormantReattach() -> false`, `detachToDormant()`, and
  `reattachDormant(...)`, all of which fail closed with `0A000` until the
  public front door exposes real dormant-token flow
- internal result continuation is gated by `_allowPortalResume()` and
  `_resumeSuspendedPortal(...)`, so blind resume fails closed with `55000`
  unless `MessageType.portalSuspended` was observed first

See `../../../../public_audit_summary`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

## Install (local dev)

```bash
cd dart
dart pub get
```

## Quick Start

```dart
import 'package:scratchbird/scratchbird.dart';

Future<void> main() async {
  final config = ScratchBirdConfig.fromDsn(
    'scratchbird://user:pass@localhost:3092/mydb',
  );
  final client = await ScratchBirdClient.connect(config);
  final result = await client.query('SELECT 1');
  print(result.rows);
  await client.close();
}
```

## Tests

Integration tests use:

- `SCRATCHBIRD_TEST_DSN`
- `SCRATCHBIRD_TEST_MANAGER_DSN` (for manager-proxy connection path)

Default `dart test` also includes local loopback proof for staged
auth/bootstrap:

- direct auth probe
- manager-proxy auth probe
- `SCRAM_SHA_512` direct attach
- `TOKEN` direct attach
- fail-closed `PEER` auth handling

The local unit suite also covers richer metadata wrappers beyond the basic
`schemas/tables/columns` families:

- `metadataRoutines`, `metadataCatalogs`
- `metadataPrimaryKeys`, `metadataForeignKeys`
- `metadataTablePrivileges`, `metadataColumnPrivileges`
- `metadataTypeInfo`

The remaining metadata gap in this lane is live-wire proof for restrictions,
wildcards, and richer catalog payload completeness, not absence of the wrapper
surface.
