# Security/Auth Reconciliation Model

Search key: `DRIVER-SERVER-RECONCILIATION-SECURITY-AUTH-MODEL`.

## Purpose

Close the gap between driver-required authentication surfaces, canonical
security contracts, and implementation-visible provider families.

## Required Registry Additions

The auth registries must explicitly cover every code-visible and
driver-required family:

| Family | Required status |
| --- | --- |
| password/internal verifier | Supported when configured by policy. |
| password compatibility | Disabled unless policy admits compatibility mode. |
| SCRAM-SHA-256 | Supported when verifier storage and channel binding rules are present. |
| SCRAM-SHA-512 | Guarded until SHA-512 verifier storage and tests exist. |
| mTLS/certificate | Supported with TLS trust, revocation, EKU, and principal mapping. |
| Kerberos/GSSAPI | Supported only through declared external directory verifier boundary. |
| LDAP/AD | Supported only with StartTLS policy, bind/search rules, referrals, group materialization. |
| OIDC/JWT | Must state whether validator-only or cryptographic JWKS/introspection is available. |
| SAML | Must state assertion signature, audience, expiry, and attribute mapping rules. |
| RADIUS | Must state Access-Request verifier boundary and shared secret handling. |
| WebAuthn/FIDO2 | Must state challenge, RP, origin, UV, signature, and MFA rules. |
| Workload identity/SPIFFE/managed identity | Must state trust bundle, expiry, signature, workload group mapping. |
| PEER/ident | Must fail closed without verified OS peer credential evidence. |
| Proxy assertion / manager bootstrap token | Must bind to manager/listener trust and engine security authority. |
| PAM / custom C++ plugin | Must be bounded by plugin trust, memory, timeout, cancellation, and C++-only rules. |

## Driver-Facing Requirements

- Auth-surface probe must be available without privileged database action.
- Resolved-auth reporting must show method, provider, principal class, security
  generation, and policy generation after attach.
- Token rotation/refresh must have a REAUTH envelope before long-session token
  rows can close.
- Unsupported/admitted-but-unavailable methods fail closed with deterministic
  SQLSTATE/diagnostic mapping.

## Required Tests

- Positive and negative test for every registered auth method.
- Fail-closed test for every unsupported or verifier-incomplete method.
- PEER/ident no-OS-evidence denial and OS-evidence acceptance.
- Parser/listener/driver cannot become authentication authority.
- Redaction tests for secrets, tokens, client cert keys, and provider payloads.

## DSR-013 Reconciliation Closure

Status: completed for canonical registry contract.

Search key: `DRIVER-SERVER-RECONCILIATION-SECURITY-AUTH-MODEL-DSR-013-CLOSURE`.

Canonical registry authority:

| Registry | Closure result |
| --- | --- |
| `public_contract_snapshot` | Defines method IDs, aliases, support state, verifier authority, verifier boundary, verifier storage, production admission, channel-binding posture, fail-closed diagnostics, and redaction for all driver-required and code-visible auth families. |
| `public_contract_snapshot` | Defines provider IDs, code-visible family aliases, supported method IDs, provider class, verifier boundary, support state, production admission, health/failover behavior, fail-closed diagnostics, and redaction for all implementation-visible provider families. |

Coverage rows closed by registry definition:

| Family | Canonical method/provider coverage |
| --- | --- |
| password/internal verifier | `internal.password_verifier` / `builtin.internal_password`. |
| password compatibility | `internal.password_compat` / `builtin.password_compat`, guarded and policy-gated. |
| SCRAM-SHA-256 | `internal.scram_sha256` / `builtin.scram`, channel binding required. |
| SCRAM-SHA-512 | `internal.scram_sha512` / `builtin.scram`, guarded until verifier storage and tests exist. |
| mTLS/certificate | `tls.client_certificate` / `tls.client_certificate_store`. |
| PEER/ident | `os.peer_credential` / `os.peer_credential`, fail-closed without verified OS peer credential evidence. |
| Kerberos/GSSAPI | `kerberos.gssapi_ticket` / `directory.kerberos_gssapi`. |
| LDAP/AD | `ldap.starttls_bind` / `directory.ldap_ad_starttls`, StartTLS or stronger transport required. |
| OIDC/JWT and OAuth validator | `token.oidc_jwt` / `federation.oidc_jwt_validator`; `federation.oauth_validator` is validator-only and not a login provider. |
| SAML | `saml.signed_assertion` / `federation.saml_validator`. |
| RADIUS | `radius.access_request` / `radius.access_request`, policy-gated. |
| WebAuthn/FIDO2 and MFA | `webauthn.fido2_assertion`, `mfa.factor_chain` / matching provider rows. |
| workload identity/SPIFFE/managed identity | `workload.spiffe_svid`, `workload.managed_identity` / matching provider rows. |
| token/API key/authkey and refresh | `token.bearer`, `token.api_key_authkey`, `token.refresh_reauth` / token provider rows; long-session refresh requires engine-owned reauthentication. |
| proxy assertion / manager bootstrap | `proxy.manager_assertion` / `proxy.manager_assertion_provider`. |
| PAM and custom C++ plugin | `pam.service_conversation`, `custom.cpp_plugin` / matching provider rows. |
| policy-gated unsupported provider | `policy.unsupported_provider_refusal` / `policy.trust_reject`. |

Non-authority boundary:

Drivers, listeners, managers, parsers, adapters, and tools may transport credential packets, assertions, and donor-rendered auth messages only. Session acceptance remains engine-owned, and registry rows require fail-closed diagnostics when a verifier boundary, support state, policy gate, channel binding, redaction policy, or production-admission requirement is not satisfied.
