# ScratchBird Pascal/Delphi Driver

ScratchBird native wire protocol client and adapters for Delphi/FreePascal.

The lane now includes the shared staged auth/bootstrap contract:

- `TScratchBirdClient.ProbeAuthSurface(dsn)` for front-door auth discovery
- `TScratchBirdClient.GetResolvedAuthContext()` for post-connect resolved auth truth
- direct listener auth execution for `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN`
- fail-closed handling for admitted-but-not-local `MD5`, `PEER`, and `REATTACH`
- shared config keys for `auth_token`, auth plugin-selection payloads, and dormant identifiers

## Documentation

- [Baseline requirement mapping (S0)](BASELINE_REQUIREMENT_MAPPING.md)
- Getting started
- API reference

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `BeginTransactionEx(...)` exposes the canonical MGA begin payload fields for
  isolation/access/deferrable/wait/timeout/autocommit/conflict-action, and
  the overloaded `BeginTransactionEx(..., ReadCommittedMode)` plus adapter
  `StartTransactionEx(..., ReadCommittedMode)` surfaces now expose the
  canonical `READ COMMITTED` sub-mode selector directly.
  `CanonicalReadCommittedModeName(...)` makes that selector explicit in lane
  source, including `READ COMMITTED READ CONSISTENCY`, while
  lane source now spells out the current isolation-byte meaning:
  `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `RetryScopeForSqlState(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- `SupportsPreparedTransactions()` plus `PrepareTransaction(...)`,
  `CommitPrepared(...)`, and `RollbackPrepared(...)` make limbo control
  explicit via canonical transaction-control SQL instead of reconnect folklore
- `SupportsDormantReattach()` is explicit and false; `DetachToDormant()` and
  `ReattachDormant(...)` fail closed with `0A000` until the public front door
  exposes a real dormant token flow
- `TScratchBirdResultStream` now resumes only after an explicit
  `MSG_PORTAL_SUSPENDED` state and routes continuation through the internal
  `AllowPortalResume` / `ResumeSuspendedPortal(...)` guard, so blind resume
  attempts fail closed with `55000`

See `../../../../public_audit_summary`.

## Build/Test (Windows/Linux)

See `docs/BUILD_MATRIX.md`.
The driver now defaults to first-party native transport/TLS units in
`lanes/active/drivers/pascal/src` and requires OpenSSL runtime libraries
(`libssl`/`libcrypto`) to be available.

Native TLS status for `0.1.0`: runtime TLS is implemented via OpenSSL-backed
native transport in-driver (connect/handshake/read/write/close), with TLS
policy handling for `sslmode` (`disable`, `allow`, `prefer`, `require`,
`verify-ca`, `verify-full`) and hostname checks in `verify-full`.

Temporary compatibility path: define `SCRATCHBIRD_USE_INDY` and add vendored Indy
paths (`third_party/indy/Lib/Core`, `Lib/Protocols`, `Lib/System`, `Lib/Security`)
if you need legacy transport during migration.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build coverage (`fpc` compile). |
| Windows | Supported | CI build coverage (`fpc` compile). |
| macOS | Untested | Not currently covered in CI. |

## Usage (core client)

```pascal
uses
  ScratchBird.Client;

var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    Client.Connect('scratchbird://user:pass@localhost:3092/mydb');
    Client.ExecSQL('SELECT 1');
  finally
    Client.Free;
  end;
end;
```

## Adapters

- FireDAC: `ScratchBird.FireDAC`
- IBX: `ScratchBird.IBX`
- Zeos: `ScratchBird.Zeos`
- SQLdb: `ScratchBird.SQLdb`
