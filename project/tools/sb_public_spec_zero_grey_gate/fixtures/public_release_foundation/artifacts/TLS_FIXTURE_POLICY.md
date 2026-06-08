# TLS Fixture Policy

Search key: `PUBLIC_RELEASE_FOUNDATION_TLS_FIXTURE_POLICY`

## Fixture Location

All generated certificates, keys, trust stores, logs, and packet captures must
be written under the active build directory. No generated TLS material may be
written to the source tree.

## Required Fixtures

| Fixture | Expected Result |
| --- | --- |
| Test CA and valid server certificate | Listener/server route accepts TLS when policy allows. |
| Valid client certificate for mTLS | Engine security layer maps subject to principal or policy identity. |
| Plaintext client with TLS required | Connection refused before session or transaction creation. |
| Wrong CA client certificate | Authentication denied by engine authority. |
| Expired client certificate | Authentication denied with stable diagnostic. |
| Hostname/SNI mismatch | Connection denied or diagnostic emitted according to policy. |
| Channel-binding mismatch | Engine authentication denies session creation. |
| TLS disabled policy | Plaintext is allowed only when explicit policy permits it. |

## Engine Authority Rule

The listener and parser transport TLS state and proof. They do not authorize the
session. The engine security layer is the only authority that accepts or denies
authentication and session creation.

## Acceptance

The `tls_fixture_policy_gate` passes only when all fixtures above are generated
under build output paths and are consumed by CTest gates.
