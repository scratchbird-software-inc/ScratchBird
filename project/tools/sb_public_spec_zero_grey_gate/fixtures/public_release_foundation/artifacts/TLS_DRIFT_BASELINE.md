# TLS Drift Baseline

Search key: `PUBLIC_RELEASE_FOUNDATION_TLS_DRIFT_BASELINE`

## Baseline

Before this slice, TLS policy was represented in configuration and server
profile fields, but enforcement was incomplete at the listener/parser route.
The public release target `SB-PUBLIC-GAP-0066` requires the route to refuse
plaintext when TLS is required and to keep authentication authority in the
engine.

## Implemented Corrections

- Listener configuration validates `tls_required`, certificate, key, CA, and
  worker-family compatibility.
- Listener runtime refuses non-TLS preauth bytes when policy requires TLS.
- Parser pool forwards TLS policy evidence to parser workers.
- Server session registry evaluates TLS peer evidence through the engine
  authentication path and fails denied channels closed.
- Negative diagnostics cover plaintext, wrong CA, expired client certificate,
  and channel-binding mismatch.

## Verification

The coordinator verified these labels:

- `sbwp_tls_server_listener_gate`
- `sbwp_tls_engine_auth_gate`
- `sbwp_tls_negative_policy_gate`
- `tls_fixture_policy_gate`

Command:

```sh
ctest --test-dir build --output-on-failure -L "sbwp_tls_server_listener_gate|sbwp_tls_engine_auth_gate|sbwp_tls_negative_policy_gate|tls_fixture_policy_gate"
```

Result: 4/4 tests passed.
