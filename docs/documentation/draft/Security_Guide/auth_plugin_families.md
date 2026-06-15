# Authentication Plugin Families

## Purpose

This page is the per-family reference for all authentication plugin families
declared in the ScratchBird engine. It covers what each family is, what it
requires, whether it is live (fully wired) or carries a guard that prevents
normal use, and its security-critical validation requirements.

Sources: `src/engine/internal_api/security/auth_provider_model.cpp`
(the `kAuthFamilyRegistry` array), `src/engine/internal_api/security/auth_provider_live_adapter.cpp`
(family-specific `Validate*` functions), and the 18 `sb_auth_plugin_*_probe`
probe tools.

## Terminology

**Live (fully wired)** — the family has a `ValidateXxx` function in the live
adapter and the registry marks `live_evidence_required = true`. A real
deployment can use this family if the required dependency is present.

**Fixture/test path only** — the family is declared in the registry and has a
probe, but the live adapter does not have a corresponding `ValidateXxx`
function. Authentication is possible only via the internal fixture path (used
in unit and integration testing).

**Guarded** — the registry entry marks `guarded = true`. This means the family
has a fail-closed guard that prevents use until a specific engine-internal
condition is met (e.g., verifier storage is implemented and tested). Guarded
families return a documented error code rather than allowing authentication.

**Policy-gated** — the registry marks `policy_gated = true`. The family
requires an explicit policy option to be present before it can be used. Without
the policy option, authentication is denied.

**Validator-only** — the family is not a login mechanism. It supplies evidence
for an associated login family but cannot be used directly to authenticate a
connection.

**Forbidden** — the family is in the registry but is refused unconditionally.

## Summary Comparison Table

| Family | Probe | Live adapter | Guards / gates | Required dependency | Group evidence | Membership path explain | MFA |
|--------|-------|-------------|----------------|---------------------|---------------|------------------------|-----|
| `local_password` | scram_password | Yes (via scram_sha256 path) | None | None | No | No | No |
| `scram_sha256` | scram_password | No (fixture) | Channel binding required | None | No | No | No |
| `scram_sha512` | scram_password | No | **Guarded** | None | No | No | No |
| `peer` / `ident` | peer_ident | No (fixture) | None | None | No | No | No |
| `pam` | pam | Yes | Policy-gated | `pam` | No | No | No |
| `certificate_mtls` | certificate_mtls | Yes | Dependency required | `tls_x509` | Yes (required) | No | No |
| `ldap_ad` | ldap_ad | Yes | Dependency required | `ldap_client` | Yes (required) | Yes | No |
| `kerberos_pac` | kerberos_pac | Yes | Dependency required | `gssapi_krb5` | Yes (PAC groups) | No | No |
| `oidc_jwt` | oidc_jwt | Yes | Dependency required | `oidc_jwt_client` | Yes (required) | No | No |
| `saml` | saml | Yes | Dependency required | `saml_xmlsig` | Yes (attributes) | No | No |
| `webauthn` | webauthn_mfa | Yes | Policy-gated, MFA required | `webauthn_fido2` | No | No | Yes |
| `radius` | radius | Yes | Policy-gated, dependency required | `radius_client` | Yes (from attributes) | No | No |
| `proxy_assertion` | proxy_assertion | Yes | Policy-gated, channel binding | `proxy_assertion_verifier` | Optional | No | No |
| `token_api_key` | token_authkey | Yes | None | None | Yes (required) | No | No |
| `bearer_token` | (live adapter only) | Yes | None | None | No | No | No |
| `workload_identity` | workload_identity | Yes | Dependency required | `spiffe_svid_or_workload_oidc` | Yes (service_group) | No | No |
| `managed_identity` | (live adapter only) | Yes | Dependency required | `spiffe_svid_or_workload_oidc` | Yes (service_group) | No | No |
| `factor_chain` | webauthn_mfa | Yes | Policy-gated, MFA required | `webauthn_fido2` | No | No | Yes |
| `challenge_state` | challenge_state | Challenge continuation | Replay/expiry protection | (uses other family) | — | — | — |
| `manifest_policy` | manifest_policy | Registration trust | Signature/ABI/dependency checks | — | — | — | — |
| `sblr_api` | sblr_api | SBLR dispatch path | Admin rights required | — | — | — | — |
| `registry` | registry | Registration lifecycle | Duplicate/unknown/disabled rejection | — | — | — | — |
| `secret_provider` | secret_provider | Credential rotation | Plaintext storage refused | — | — | — | — |

The rows marked `challenge_state`, `manifest_policy`, `sblr_api`, `registry`,
and `secret_provider` correspond to the 18 probe tools listed in the task
specification. These probes test infrastructure cross-cutting concerns (trust
evaluation, challenge continuation, SBLR dispatch, registration lifecycle,
credential rotation) rather than discrete login-family authentication.

The 18 probe directories are:
`certificate_mtls`, `challenge_state`, `kerberos_pac`, `ldap_ad`,
`manifest_policy`, `oidc_jwt`, `pam`, `peer_ident`, `proxy_assertion`,
`radius`, `registry`, `saml`, `sblr_api`, `scram_password`, `secret_provider`,
`token_authkey`, `webauthn_mfa`, `workload_identity`.

---

## Per-Family Reference

### local_password

**What it is:** Built-in password authentication using the engine's local
credential verifier. This is the only provider enabled by default in the
`default-local-password` policy pack.

**Registry:** `supports_authn = true`, no guards, no policy gate, no dependency.

**How it works:** Credentials are stored as SCRAM-verifier artifacts in the
protected material catalog. The engine verifies the credential locally without
an external service call.

**Default policy state:** `enabled_by_default = true`, authority:
`durable_catalog_row`, `credential_verifier_policy:
local_password_verifier_required`.

**Security notes:**
- Plain-text credential storage is refused. The credential rotation probe
  confirms that `store_reusable_plaintext:true` is rejected.
- A principal with no stored credential cannot authenticate (the probe confirms
  `missing_material_rejected`).
- Password-compatible cleartext fallback (`password_compat` family) is
  policy-gated (`allow_password_compat` must be set) and is refused by default.

---

### scram_sha256

**What it is:** SCRAM-SHA-256 channel-binding authentication. A sub-variant of
the local password family.

**Registry:** `supports_authn = true`, channel binding required, no dependency.

**Live adapter:** No separate `ValidateScram` function in the live adapter.
Authentication is handled via the fixture/internal path. The scram_password
probe exercises this path.

**What the probe confirms:**
- `scram_ok` — SCRAM authentication succeeds on the fixture path
- `compat_default_rejected` — password-compat fallback is rejected by default
- `compat_allowed` — password-compat is allowed when `allow_password_compat:true` is set
- `downgrade_rejected` — a downgrade attempt to `scram_sha512` with `downgrade_attempt:true` is rejected

**Security notes:**
- The engine refuses algorithm downgrade attempts on this family.
- Channel binding is required by the registry entry.

---

### scram_sha512

**What it is:** SCRAM-SHA-512. Declared in the registry but **guarded** pending
verifier storage implementation and test coverage.

**Registry:** `guarded = true`, fail-closed code: `SECURITY.AUTH_METHOD_UNSUPPORTED`,
detail: `scram_sha512_guarded_until_verifier_storage_and_tests`.

**Security notes:** Do not attempt to configure this family in production. It
will return `SECURITY.AUTH_METHOD_UNSUPPORTED` regardless of credential state.
This guard is intentional and is a source-documented invariant.

---

### peer / ident

**What it is:** Operating-system process credential authentication. `peer`
verifies the client's OS UID via a socket credential exchange (POSIX
`SO_PEERCRED` or equivalent). `ident` queries an RFC 1413 ident server on the
network connection.

**Registry:** Both map to the `peer` base family in `BaseFamily()`. `ident` is
normalized to `peer` by `CanonicalAuthProviderFamily`. `supports_authn = true`,
no guards, no dependency declared in the registry.

**Live adapter:** No `ValidatePeer` or `ValidateIdent` function in the live
adapter. Uses fixture path.

**The peer_ident probe confirms:**
- `peer_ok` — peer authentication succeeds with `provider:peer` and `principal:local_user`
- `ident_ok` — ident authentication succeeds on the fixture path
- `spoof_rejected` — a stale ident response (`freshness:stale`) is rejected

**Platform notes:** `peer` requires a POSIX socket with `SO_PEERCRED` or
equivalent. See [platform_configuration.md](platform_configuration.md).
`ident` relies on an RFC 1413 server on the network and is generally not
suitable for production deployments.

---

### pam

**What it is:** Pluggable Authentication Modules. Delegates authentication to
the OS PAM stack.

**Registry:** `supports_authn = true`, `policy_gated = true` (policy option:
`pam_policy_enabled`), dependency: `pam`, `live_evidence_required = true`.

**Live adapter:** `ValidatePam` function present. Required payload fields:
`user`, `service`, `module`, `password`, `prompt`, `account`, `session`.

**Validation rules:**
- `prompt` must be `"hidden"` or `"secret"` — insecure prompt types are refused
  with `pam_insecure_prompt`
- `account` must be `"allow"` — PAM account phase must succeed
- `session` must be `"open"` — PAM session-open phase must succeed

**The pam probe confirms:**
- `pam_auth` — authentication succeeds on the fixture path
- `pam_failure_rejected` — a fixture with `fixture_fail:true` is rejected

**Platform notes:** PAM is POSIX-only. It is not available on Windows without a
compatibility layer. On Linux and BSD, the PAM module name corresponds to a
file in `/etc/pam.d/`. See [platform_configuration.md](platform_configuration.md).

**Security notes:** PAM conversations with non-hidden prompts are refused at
the engine level, not just at policy level. The `pam_policy_enabled` policy
option must be set to activate this family.

---

### certificate_mtls

**What it is:** Mutual TLS client certificate authentication.

**Registry:** `supports_authn = true`, dependency: `tls_x509`,
`live_evidence_required = true`, `group_materialization_required = true`,
`channel_binding_required = true`.

**Live adapter:** `ValidateCertificate` function present. Required payload fields:
`subject`, `san`, `chain`, `revoked`, `eku`, `fingerprint`.

**Validation rules:**
- `chain` must be `"trusted"` — untrusted chains return `certificate_chain_untrusted`
- `revoked` must be `false` — revoked certificates return `certificate_revoked`
- `eku` must be `"clientAuth"` — incorrect EKU returns `certificate_eku_invalid`
- `fingerprint` must be a 64-character hex string
- Group evidence (`groups` or `group`) must be present — `certificate_group_materialization_required`

**The certificate_mtls probe confirms:**
- `mtls_ok` — authentication succeeds with valid credential
- `group_materialization_required` — authentication without group evidence fails

**Security notes:** Certificate revocation status must be checked and confirmed
before the certificate is accepted. The engine will not accept a certificate
that is marked `revoked = true`. Group materialization is required; a certificate
that authenticates but does not produce group membership cannot complete
authentication.

---

### ldap_ad

**What it is:** LDAP / Active Directory bind authentication with group
synchronization.

**Registry:** `supports_authn = true`, dependency: `ldap_client`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateLdap` function present. Required payload fields:
`user`, `endpoint`, `starttls`, `bind`. One of `password`, `bind_secret`, or
`bind_token` must also be present.

**Validation rules:**
- `starttls` must be `true` — plain-text LDAP binds return `ldap_starttls_required`
- `bind` must be `"allow"` — a denied bind returns `ldap_bind_denied`
- Group evidence (`groups` or `group`) must be present — `ldap_group_materialization_required`

**The ldap_ad probe confirms:**
- `ldap_auth` — authentication succeeds on the fixture path
- `ldap_sync` — group synchronization via `EngineSyncExternalGroups` succeeds and sets `materialized = true`
- `ldap_explain` — membership path explanation via `EngineExplainMembership` is available and sets `explainable = true`

**Security notes:** StartTLS is enforced at the engine level. There is no
configuration option to bypass it. Group synchronization is required; a
principal whose LDAP groups cannot be resolved cannot authenticate.

---

### kerberos_pac

**What it is:** Kerberos ticket authentication with PAC (Privilege Attribute
Certificate) group extraction.

**Registry:** `supports_authn = true`, dependency: `gssapi_krb5`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateKerberos` function present. Required payload fields:
`spn`, `subject`, `nonce`, `kdc`, `sig`, `exp`, `pac_groups`.

**Validation rules:**
- `sig` must be a 64-character hex string — invalid signatures return `kerberos_signature_invalid`
- `exp` must be a future timestamp in milliseconds — expired tickets return `kerberos_ticket_expired`
- All required fields must be present

**The kerberos_pac probe confirms:**
- `kerberos_auth` — authentication succeeds on the fixture path
- `effective_only_no_path` — membership path explanation is not available
  (`!exp_r.ok`); the kerberos family provides effective groups from the PAC
  only and does not support path explanation

**Security notes:** The PAC provides effective group membership at ticket
issuance time. Group membership is not re-queried after authentication; a
principal's effective groups are those encoded in the ticket. Ticket expiry is
enforced.

---

### oidc_jwt

**What it is:** OpenID Connect JWT authentication. Validates ID tokens issued by
an OIDC provider.

**Registry:** `supports_authn = true`, dependency: `oidc_jwt_client`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateOidcJwt` function present. Required payload fields:
`iss`, `aud`, `sub`, `alg`, `exp`, `sig`, `groups`, `validator`.

**Validation rules:**
- `validator` must be `"jwks"` or `"introspection"` — other values return `oidc_jwt_validator_boundary_required`
- `alg` must not be `"none"` — returns `oidc_jwt_alg_none_forbidden`
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp
- All required fields must be present

**The oidc_jwt probe confirms:**
- `oidc_auth` — authentication succeeds
- `overage_requires_sync` — when `groups_overage:true` is set (groups claim
  exceeds the token limit), authentication fails; a group sync is required before
  the principal can be admitted
- `oauth_not_login` — the `oauth_validator` sub-family cannot be used directly
  for login (validator-only)

**Security notes:** JWT algorithm `none` is explicitly rejected. Token
validation must use either JWKS or token introspection; in-process verification
without an external validator boundary is not supported. Groups overage
(when the IDP omits groups from the token due to claim size limits) requires
explicit group synchronization before login.

---

### saml

**What it is:** SAML 2.0 assertion authentication.

**Registry:** `supports_authn = true`, dependency: `saml_xmlsig`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateSaml` function present. Required payload fields:
`issuer`, `audience`, `subject`, `not_on_or_after`, `signature`, `attributes`.

**Validation rules:**
- `signature` must be a 64-character hex string — invalid signatures return `saml_signature_invalid`
- `not_on_or_after` must be a future timestamp — expired assertions return `saml_assertion_expired`

**The saml probe confirms:**
- `saml_auth` — authentication succeeds
- `stale_assertion_rejected` — a request with `freshness:stale` is rejected

**Security notes:** SAML assertion freshness is enforced. Stale assertions are
rejected at the engine level. XML signature validation requires the `saml_xmlsig`
dependency.

---

### webauthn

**What it is:** WebAuthn / FIDO2 authenticator-based authentication. Used as an
MFA second factor or as a standalone authenticator.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`webauthn_policy_enabled`), `supports_mfa = true`, dependency: `webauthn_fido2`,
`live_evidence_required = true`.

**Live adapter:** `ValidateWebAuthn` function present. Required payload fields:
`challenge`, `rp`, `origin`, `credential`, `subject`, `uv`, `exp`, `signature`.

**Validation rules:**
- `uv` (user verification) must be `true` — `webauthn_user_verification_required`
- `signature` must be a 64-character hex string
- `exp` must be a future timestamp

**The webauthn_mfa probe confirms:**
- `webauthn_auth` — authentication succeeds when `mfa:present` is set
- `missing_mfa_rejected` — authentication without `mfa:present` fails
- `factor_not_login` — the `factor_chain` sub-family cannot be used directly for
  login (it is a multi-factor chain composition, not a standalone login)

**Security notes:** User verification is required. The `webauthn_policy_enabled`
policy option must be set. MFA evidence must be present in the request.

---

### factor_chain

**What it is:** Multi-factor authentication chain. Composes a primary
authentication context with a second factor.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`factor_chain_policy_enabled`), `supports_mfa = true`, dependency: `webauthn_fido2`.

**Live adapter:** `ValidateFactorChain` function present. Required payload fields:
`primary_auth_context_uuid`, `factor_policy_uuid`, `factor_results`,
`challenge_transcript_hash`, `subject`.

**Validation rules:**
- `factor_results` must be `"allow"` — anything else returns `SECURITY.MFA_REQUIRED`
- `challenge_transcript_hash` must be a 64-character hex string

**Security notes:** The factor chain is a composition mechanism, not a standalone
login method. It requires a completed primary authentication context UUID. The
`factor_chain_policy_enabled` policy option must be set.

---

### radius

**What it is:** RADIUS protocol authentication.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`radius_policy_enabled`), dependency: `radius_client`, `live_evidence_required = true`,
`group_materialization_required = true`.

**Live adapter:** `ValidateRadius` function present. Required payload fields:
`user`, `authenticator`, `result`, `attribute`, `shared_secret_handle`.

**Validation rules:**
- `authenticator` must be a 64-character hex string — invalid values return `radius_authenticator_invalid`
- `result` must be `"accept"` — any other result returns `radius_rejected`

**The radius probe confirms:**
- `radius_auth` — authentication succeeds
- `radius_no_path` — membership path explanation is not available for RADIUS
  principals (similar to kerberos_pac; effective groups only)

**Security notes:** RADIUS access-accept is the only admitted result. The
shared secret is handled via a `shared_secret_handle` (protected material
reference) rather than inline. The `radius_policy_enabled` policy option must
be set.

---

### proxy_assertion

**What it is:** Trusted middle-tier assertion. Used when a trusted proxy
component forwards a user's identity to the engine rather than requiring the
user to authenticate directly.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`proxy_assertion_policy_enabled`), `channel_binding_required = true`,
dependency: `proxy_assertion_verifier`, `live_evidence_required = true`.

**Live adapter:** `ValidateProxy` function present. Required payload fields:
`iss`, `sub`, `aud`, `proxy`, `source`, `manager_trust`, `listener_binding`,
`exp`, `sig`.

**Validation rules:**
- `source` must be `"trusted"` — returns `proxy_source_untrusted`
- `manager_trust` must be `"trusted"` — returns `proxy_manager_trust_required`
- `listener_binding` must be `"verified"` — returns `proxy_listener_binding_required`
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp

**The proxy_assertion probe confirms:**
- `proxy_ok` — assertion succeeds
- `replay_rejected` — a replayed assertion fails

**Security notes:** This provider is for trusted middle-tier components only,
not for direct user authentication. All three trust conditions (source,
manager_trust, listener_binding) must be satisfied simultaneously. Channel
binding is required. This family must not be exposed to untrusted clients.

---

### token_api_key

**What it is:** API key / authkey token authentication.

**Registry:** `supports_authn = true`, no guards, no policy gate, no dependency,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateApiKey` function present. Required payload fields:
`key_id`, `proof`, `generation`, `subject`, `groups`.

**Validation rules:**
- `proof` must be a 64-character hex string — returns `api_key_proof_invalid`
- Group evidence (`groups`) must be present

**The token_authkey probe confirms:**
- `token_auth` — authentication succeeds
- `revoked` — revoke via `EngineRevokeToken` sets `revoked = true`
- `unsynced_rejected` — a token authentication without proper group state fails

**Security notes:** Token revocation is supported and must be performed before
issuing a replacement token. Group evidence is required; an API key without
associated group membership cannot complete authentication.

---

### bearer_token

**What it is:** Opaque bearer token. A simpler token format without the
structured proof of `token_api_key`.

**Registry:** `supports_authn = true`, no guards, no policy gate, no dependency.

**Live adapter:** `ValidateBearerToken` function present. Required payload fields:
`token_id`, `proof`, `exp`, `subject`.

**Validation rules:**
- `proof` must be a 64-character hex string
- `exp` must be a future timestamp

**Security notes:** No group evidence is required. This family is intended for
short-lived, scoped access tokens. No probe directory exists for this family
specifically; it is exercised through the live adapter only.

---

### workload_identity

**What it is:** Workload identity authentication for non-human service principals
using SPIFFE SVID or workload OIDC tokens.

**Registry:** `supports_authn = true`, dependency: `spiffe_svid_or_workload_oidc`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateWorkload` function present. Required payload fields:
`trust_bundle`, `exp`, `sig`, `service_group`, and at least one of `spiffe_id`
or `sub`.

**Validation rules:**
- If `spiffe_id` is present, it must begin with `spiffe://`
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp
- `service_group` must be present

**The workload_identity probe confirms:**
- `workload_auth` — authentication succeeds
- `service_mapping_required` — authentication with `provider:workload_identity`
  but without proper service mapping fails

**Security notes:** Service group mapping is required. A workload that
authenticates but cannot be mapped to a service group cannot complete
authentication.

---

### managed_identity

**What it is:** Cloud-managed identity authentication (Azure Managed Identity,
GCP Workload Identity, or similar).

**Registry:** `supports_authn = true`, dependency: `spiffe_svid_or_workload_oidc`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateManagedIdentity` function present. Required payload
fields: `issuer`, `audience`, `subject`, `exp`, `sig`, `service_group`.

**Validation rules:**
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp

**Security notes:** This family shares the `spiffe_svid_or_workload_oidc`
dependency with `workload_identity`. No probe directory exists for this family
specifically; it is exercised via the live adapter.

---

### Infrastructure Probes (Not Login Families)

The following probe tools test cross-cutting infrastructure rather than discrete
login flows. They do not correspond to standalone authentication families.

**challenge_state** — Tests the `EngineContinueAuthChallenge` API. Verifies
that expired, replayed, and rate-limited challenges are all rejected. Used by
WebAuthn and any other challenge-based family.

**manifest_policy** — Tests `EngineRegisterAuthProvider` trust evaluation.
Verifies four rejection cases: invalid signature, missing dependency, unsupported
ABI, and stale implementation version.

**registry** — Tests the provider registration lifecycle: valid registration,
duplicate UUID rejection, unknown family rejection, and disabled provider
rejection.

**sblr_api** — Tests the SBLR dispatch path for security operations. Confirms
that `security.register_auth_provider`, `security.authenticate_provider`, and
`security.revoke_token` are all accessible via the SBLR dispatch and require
the `AUTH_PROVIDER_ADMIN` right.

**secret_provider** — Tests `EngineRotateCredential`. Verifies that credential
rotation succeeds, that plaintext storage is refused, and that missing material
is rejected.

---

## Related Pages

- [authentication_and_providers.md](authentication_and_providers.md) — provider architecture
- [platform_configuration.md](platform_configuration.md) — OS-specific notes per family
- [security_policies_and_crypto.md](security_policies_and_crypto.md) — policy-gated family configuration
