# TLS Threat Model

Search key: `PUBLIC_RELEASE_FOUNDATION_TLS_THREAT_MODEL`

## Assets

- SBWP client credentials and proof material.
- Parser/listener channel identity.
- Engine authentication decision and deny message vectors.
- Session and transaction admission state.

## Threats Covered

| Threat | Required behavior |
| --- | --- |
| Plaintext client connects while TLS is required | Listener rejects before parser/server admission. |
| Client presents certificate from the wrong CA | Engine authentication path denies with stable diagnostic. |
| Client presents expired certificate | Engine authentication path denies with stable diagnostic. |
| Channel-binding proof mismatches the TLS channel | Engine authentication path denies and no session is created. |
| Parser/listener attempts to become auth authority | Fails design review; only the engine can accept or deny. |

## Authority Boundary

The listener and parser may carry TLS state and proof material. They do not own
authentication or authorization. Final accept/deny remains engine-owned and must
produce a session/transaction only after the engine accepts the proof.

## Verification

Passing labels:

- `sbwp_tls_server_listener_gate`
- `sbwp_tls_engine_auth_gate`
- `sbwp_tls_negative_policy_gate`
- `tls_fixture_policy_gate`
