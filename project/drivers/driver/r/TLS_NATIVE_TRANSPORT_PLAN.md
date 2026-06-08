# R TLS Native Transport Plan

## Objective
Implement a real native TLS socket transport for the R driver so SBWP/MCP frames are sent and received over TLS without external wrappers.

## Scope
- Add a compiled C/OpenSSL transport layer exposed via `.Call`.
- Replace `readBin`/`writeBin` connection I/O paths in `R/client.R` with native TLS I/O wrappers.
- Preserve existing protocol/auth/query logic and manager-proxy handshake behavior.

## Design
1. Native TLS backend (`src/tls_transport.c`)
- `sb_tls_connect`: TCP connect + TLS 1.3 handshake + sslmode policy enforcement.
- `sb_tls_write`: write full frame payload with retry semantics.
- `sb_tls_read_exact`: blocking exact-length read for protocol framing.
- `sb_tls_close`: deterministic socket/SSL cleanup.
- External pointer finalizer for safe cleanup.

2. R wrapper glue (`R/native_transport.R`)
- Thin wrappers around `.Call` entrypoints.
- Input normalization for optional cert/key/password/rootcert values.

3. Driver integration (`R/client.R`)
- `sb_open_socket` uses native TLS connect and returns external pointer.
- New helpers: `sb_socket_write`, `sb_socket_read_exact`, `sb_socket_close`.
- `sb_send_message`, `sb_recv_message`, MCP frame I/O switched to helpers.
- Disconnect/terminate uses native close helper.

4. Package registration/build wiring
- `src/init.c` routine registration.
- `NAMESPACE`: `useDynLib(scratchbird, .registration=TRUE)`.
- `src/Makevars` / `src/Makevars.win` link against OpenSSL.

## sslmode policy
- `disable`: hard error (ScratchBird requires TLS).
- `allow`, `prefer`: TLS enabled but cert validation relaxed (compat mode).
- `require`, `verify-ca`: cert chain validation required.
- `verify-full`: cert chain + hostname validation required.

## Validation
- Build/install package via local library path.
- Smoke-test native symbols and connect error paths.
- Existing config parser tests remain valid.

## Follow-ups
- Add dedicated TLS integration tests against a local TLS endpoint in CI.
- Add OCSP/revocation policy controls if needed for parity with other drivers.
