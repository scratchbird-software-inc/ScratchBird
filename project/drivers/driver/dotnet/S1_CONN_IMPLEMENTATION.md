# DLB-DOTNET-001 S1 CONN Implementation

## What Changed

- Expanded DSN/property normalization in
  [`src/ScratchBird.Data/Config.cs`](src/ScratchBird.Data/Config.cs):
  - added generic `auth_token` aliases
  - preserved shared auth hint / pinning inputs
  - kept `front_door_mode` direct vs `manager_proxy` normalization
- Added staged bootstrap/auth surfaces in
  [`src/ScratchBird.Data/AuthBootstrap.cs`](src/ScratchBird.Data/AuthBootstrap.cs),
  [`src/ScratchBird.Data/ProtocolClient.cs`](src/ScratchBird.Data/ProtocolClient.cs),
  and [`src/ScratchBird.Data/ScratchBirdConnection.cs`](src/ScratchBird.Data/ScratchBirdConnection.cs):
  - public staged probe via `ScratchBirdConnection.ProbeAuthSurface(...)`
  - resolved-auth reporting via `GetResolvedAuthContext()`
  - direct-listener probe and manager-proxy probe
  - stable admitted-method reporting with plugin ids and broker-required flags
- Updated live connect/auth behavior in
  [`src/ScratchBird.Data/ProtocolClient.cs`](src/ScratchBird.Data/ProtocolClient.cs):
  - shared socket/bootstrap normalization path for connect and probe
  - native auth execution now includes `PASSWORD`, `SCRAM_SHA_256`,
    `SCRAM_SHA_512`, and `TOKEN`
  - admitted but unsupported or broker-required methods (`MD5`, `PEER`,
    `REATTACH`) now fail closed with `0A000`
  - resolved auth context now records front-door mode, attach truth,
    manager-auth state, and negotiated auth method/plugin
- Generalized SCRAM digest handling in
  [`src/ScratchBird.Data/ScramClient.cs`](src/ScratchBird.Data/ScramClient.cs)
  so the lane now supports both SHA-256 and SHA-512
- Expanded the public connection-string builder in
  [`src/ScratchBird.Data/ScratchBirdConnectionStringBuilder.cs`](src/ScratchBird.Data/ScratchBirdConnectionStringBuilder.cs)
  to expose staged auth/bootstrap settings directly
- Added deterministic auth/bootstrap proof in
  [`tests/ScratchBird.Data.Tests/AuthBootstrapContractTests.cs`](tests/ScratchBird.Data.Tests/AuthBootstrapContractTests.cs):
  - direct probe
  - manager-proxy probe
  - `SCRAM_SHA_512` handshake
  - `TOKEN` execution
  - fail-closed `PEER`

## Targeted Tests Run

- `dotnet test lanes/active/drivers/dotnet/tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj --filter AuthBootstrapContractTests`
  - Result: `PASS` (`5 passed`)
- `dotnet test lanes/active/drivers/dotnet/tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj --filter "FullyQualifiedName!~IntegrationTests&FullyQualifiedName!~JDBC203PoolingAndRecoveryContractTests"`
  - Result: `PASS` (`164 passed`)

## Live Verification Note

- `dotnet test lanes/active/drivers/dotnet/tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj`
  currently requires a reachable live listener through `SCRATCHBIRD_DOTNET_URL`
  or the lane's default fallback DSN at `127.0.0.1:13092`.
- In the current environment that listener is not available, so the live
  integration suites fail with `Connection refused`. This is a server/evidence
  gate, not a remaining lane-local auth/bootstrap implementation gap.

## CONN Status Recommendation

- `IMPLEMENTED` (baseline-complete for the 0.1.0 scope, with remaining live
  evidence gated on a reachable server).
