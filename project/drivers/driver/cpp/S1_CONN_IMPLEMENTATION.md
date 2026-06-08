# DLB-CPP-002 S1 CONN Implementation

## What Changed

- Expanded DSN/property normalization in
  [`src/driver_config.cpp`](src/driver_config.cpp):
  - normalized supported transport policy for `inet_listener`, `local_ipc`,
    and `managed`
  - normalized `front_door_mode`
  - parsed shared auth/bootstrap inputs, including `auth_token`,
    startup auth selection hints, and manager-proxy inputs
  - aligned `auth_method` parsing to include `SCRAM_SHA_512`, `TOKEN`,
    `PEER`, and `REATTACH`
- Added staged bootstrap/auth surfaces in
  [`include/scratchbird/client/network_client.h`](include/scratchbird/client/network_client.h),
  [`include/scratchbird/client/connection.h`](include/scratchbird/client/connection.h),
  [`src/network_client.cpp`](src/network_client.cpp),
  [`src/connection.cpp`](src/connection.cpp),
  and [`src/scratchbird_client_c.cpp`](src/scratchbird_client_c.cpp):
  - public `probeAuthSurface(...)` surfaces for C++ and JSON-returning C API
  - resolved-auth reporting for both attached C++ sessions and C handles
  - direct-listener probe and manager-proxy probe
  - stable admitted-method reporting with plugin ids and broker-required flags
- Updated connect/auth behavior in
  [`src/network_client.cpp`](src/network_client.cpp):
  - connect and staged probe now share the same socket/bootstrap normalization path
  - `front_door_mode=manager_proxy` still fails fast with `08001` when a
    real attach requires `manager_auth_token`
  - startup auth selection is applied through startup params
  - native auth execution now includes `PASSWORD`, `SCRAM_SHA_256`,
    `SCRAM_SHA_512`, and `TOKEN`
  - admitted but unsupported or broker-required methods (`MD5`, `PEER`,
    `REATTACH`) now fail closed with `0A000`
  - SCRAM digest helpers were generalized so SHA-512 uses the correct digest size
- Added lane auth/bootstrap proof in
  [`tests/test_driver_connectivity.cpp`](tests/test_driver_connectivity.cpp):
  - direct probe
  - manager-proxy probe
  - `SCRAM_SHA_512` handshake
  - `TOKEN` execution
  - fail-closed `PEER`
  - C API probe/resolved-auth JSON surfaces

## Targeted Tests Run

- `cmake -S lanes/active/drivers/cpp -B lanes/active/drivers/cpp/build_odbc_gate -DBUILD_TESTING=ON`
  - Result: `PASS`
- `cmake --build lanes/active/drivers/cpp/build_odbc_gate --target scratchbird_client_tests -j8`
  - Result: `PASS`
- `lanes/active/drivers/cpp/build_odbc_gate/scratchbird_client_tests`
  - Result: `PASS` (`72 passed, 2 skipped`)

## CONN Status Recommendation

- `IMPLEMENTED` (baseline-complete for the 0.1.0 scope).
