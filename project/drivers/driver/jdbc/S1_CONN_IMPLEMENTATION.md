# JDBC Auth / Bootstrap Closure

Status: implemented
Last Updated: 2026-04-17

## What Landed

- public staged probe surfaces:
  - `SBDriver.probeAuthSurface(url, Properties)`
  - `SBConnection.probeAuthSurface(SBConnectionProperties)`
- resolved auth reporting:
  - `SBConnection.getResolvedAuthContext()`
  - `SBProtocolHandler.getResolvedAuthContext()`
- direct execution coverage:
  - `PASSWORD`
  - `SCRAM_SHA_256`
  - `SCRAM_SHA_512`
  - `TOKEN`
- manager-proxy bootstrap probe/runtime alignment
- fail-closed admitted-method handling:
  - `MD5`
  - `PEER`
  - `REATTACH`
- canonical config support for `auth_token`

## Local Verification

```bash
cd lanes/active/drivers/jdbc
./gradlew test --tests com.scratchbird.jdbc.SBAuthBootstrapContractTest --tests com.scratchbird.jdbc.SBDriverTest --tests com.scratchbird.jdbc.SBScramClientTest --tests com.scratchbird.jdbc.SBProtocolHandlerStartupFeaturesTest
```

## Remaining External Proof

- Full `./gradlew test` still depends on a reachable live ScratchBird listener
  for the integration suites.
