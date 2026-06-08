# S1 CONN Implementation (DLB-PASCAL-002)

Scope: `lanes/active/drivers/pascal` only.

## What Changed

- Added manager-proxy auth preflight in `src/ScratchBird.Client.pas`:
  - `Connect` now fails fast with `manager_proxy mode requires manager_auth_token` before `FTransport.Configure`/`FTransport.Connect`.
  - This prevents network dial attempts when manager-proxy auth configuration is incomplete.
- Expanded native compatibility TLS mode handling in `src/ScratchBird.Transport.Native.pas` and `src/ScratchBird.Tls.Context.pas`:
  - `ParseTlsMode` now accepts `sslmode=disable` and maps it to plaintext socket mode.
  - `TTlsContext` now supports a non-TLS socket handshake path for `tmDisable` while retaining TLS policy enforcement for TLS-enabled modes.
- Added lane-local connection/auth/protocol tests in `tests/ConnectionAuthProtocolTests.pas`:
  - unsupported protocol guardrail (`protocol=postgresql`) rejection.
  - manager-proxy missing token fail-fast behavior at `TScratchBirdClient.Connect`.
  - native transport `sslmode=disable` configure-time acceptance.
  - protocol parser behavior: oversized header rejection and truncated `AUTH_CONTINUE` rejection.
- Added deterministic manager-proxy fixture coverage in `tests/ConnectionManagerProxyTests.pas`:
  - manager-proxy connect success path across MCP negotiation and front-door password auth handshake.
  - manager-proxy auth failure path (`MCP_MSG_AUTH_RESPONSE` failure) mapping to SQLSTATE `28000` and disconnected final state.
  - outbound frame ordering assertions across MCP and native protocol writes.
- Added deterministic direct front-door auth matrix coverage in `tests/ConnectionDirectAuthMatrixTests.pas`:
  - direct password auth path (`AUTH_PASSWORD`) from startup/auth request through connected READY state.
  - direct SCRAM auth path (`AUTH_SCRAM_SHA256`) through connected READY state.
  - compatibility policy path with `sslmode=disable`, `binary_transfer=false`, and `compression=zstd`, including startup feature-bit assertions.
  - outbound frame ordering assertions for startup and auth response writes.
- Added shared staged auth/bootstrap closure in `src/ScratchBird.AuthBootstrap.pas`, `src/ScratchBird.Client.pas`, and `src/ScratchBird.Scram.pas`:
  - `TScratchBirdClient.ProbeAuthSurface(dsn)` now exposes direct listener and `manager_proxy` auth discovery without forcing blind password-first startup.
  - `TScratchBirdClient.GetResolvedAuthContext()` now reports the admitted front-door mode plus the resolved auth method/plugin that actually attached the session.
  - direct auth execution now supports `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and generic `TOKEN`.
  - admitted-but-not-local `MD5`, `PEER`, and `REATTACH` now fail closed with explicit typed auth errors instead of guessing.
  - DSN/config parsing now includes `auth_token`, `dormant_id`, and `dormant_reattach_token`.
- Added deterministic staged auth/bootstrap proof in `tests/ConnectionAuthBootstrapContractTests.pas`:
  - direct auth probe reports `SCRAM_SHA_512`.
  - manager probe reports `TOKEN` ingress.
  - resolved auth context is tracked for `SCRAM_SHA_512` and `TOKEN`.
  - `PEER` auth fails closed and preserves resolved-auth truth.
- Updated CONN evidence and gaps in `BASELINE_REQUIREMENT_MAPPING.md`.

## Targeted Tests Run

1. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FE./lanes/active/drivers/pascal/tests ./lanes/active/drivers/pascal/tests/ConnectionAuthProtocolTests.pas`
- Result: PASS (compile succeeded).

2. `./lanes/active/drivers/pascal/tests/ConnectionAuthProtocolTests`
- Result: PASS (`ConnectionAuthProtocolTests: OK`).

3. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FE./lanes/active/drivers/pascal/tests ./lanes/active/drivers/pascal/tests/ConfigTests.pas`
- Result: PASS (compile succeeded).

4. `./lanes/active/drivers/pascal/tests/ConfigTests`
- Result: PASS (`ConfigTests: OK`).

5. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_conn_mgr_build -FE/tmp/sb_pascal_conn_mgr_bin ./lanes/active/drivers/pascal/tests/ConnectionManagerProxyTests.pas`
- Result: PASS (compile succeeded).

6. `/tmp/sb_pascal_conn_mgr_bin/ConnectionManagerProxyTests`
- Result: PASS (`ConnectionManagerProxyTests: OK`).

7. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_conn_direct_build -FE/tmp/sb_pascal_conn_direct_bin ./lanes/active/drivers/pascal/tests/ConnectionDirectAuthMatrixTests.pas`
- Result: PASS (compile succeeded).

8. `/tmp/sb_pascal_conn_direct_bin/ConnectionDirectAuthMatrixTests`
- Result: PASS (`ConnectionDirectAuthMatrixTests: OK`).

9. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_auth_bootstrap_build -FE/tmp/sb_pascal_auth_bootstrap_bin ./lanes/active/drivers/pascal/tests/ConnectionAuthBootstrapContractTests.pas`
- Result: PASS (compile succeeded).

10. `strace -f -o /tmp/pascal_auth_bootstrap.strace /tmp/sb_pascal_auth_bootstrap_bin/ConnectionAuthBootstrapContractTests`
- Result: PASS (`ConnectionAuthBootstrapContractTests: OK`).

## CONN Status Recommendation

- Recommendation: `IMPLEMENTED`

Rationale:
- Lane-local coverage includes deterministic fail-fast checks for manager-proxy auth prerequisites, key protocol parser guardrails, deterministic end-to-end manager-proxy handshake/auth success and auth-failure paths, and compatibility policy negotiation coverage for startup features.
- Live integration connection coverage remains environment-gated, but deterministic lane coverage now closes the JDBC baseline CONN contract.

## Remaining Concrete Gaps

- No known blocking CONN gaps for JDBC baseline parity; live integration connection tests remain environment-gated (`SCRATCHBIRD_PASCAL_URL`).
