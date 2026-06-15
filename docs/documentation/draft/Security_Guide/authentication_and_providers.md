# Authentication and Providers

## Purpose

This page describes the provider/plugin architecture that ScratchBird uses to
authenticate connections. It covers how a provider is declared, admitted to the
engine via plugin trust, and invoked. It also documents the challenge, credential,
and token lifecycle that provider plugins participate in. Every detail here is
grounded in the source at
`src/engine/internal_api/security/`.

## Overview

ScratchBird separates the authentication decision from the credential
verification work. Providers supply evidence. The engine owns the decision.

A provider plugin is registered with a family name, a set of declared
capabilities, and a trust state. The engine evaluates the provider's trust state
before allowing it to participate in authentication. At authentication time, the
engine calls into the provider (via the live adapter or a fixture path for
testing) and normalizes the result into an `AuthProviderLiveEvidenceResult`. The
engine then evaluates that result to produce a
`ConnectionSecurityContextRecord`, which becomes the security context for the
connection.

## Provider Descriptor

Source: `auth_provider_model.hpp` — `AuthProviderDescriptor`.

```
AuthProviderDescriptor
  provider_uuid          — UUIDv7 identity of this provider instance
  provider_family        — canonical family name (see auth_plugin_families.md)
  provider_version       — declared version of the provider
  implementation_version — implementation version (may differ from declared)
  capabilities           — SecurityProviderCapabilities (see below)
  policy_epoch           — epoch at which this descriptor was last evaluated (default: 1)
  trust_state            — "untrusted" (default), "admitted", or "revoked"
  rollout_state          — "disabled" (default), "enabled", or "deprecated"
  required_libraries     — runtime library dependencies
  allowed_policy_scopes  — policy scopes this provider may participate in
```

The `trust_state` starts as `"untrusted"`. A provider that has not been admitted
cannot be used for authentication. Admission is performed by
`EngineRegisterAuthProvider` after the trust evaluation in `plugin_trust_api`.

## Provider Capabilities

Source: `security_model.hpp` — `SecurityProviderCapabilities`.

Each provider declares a capability set. These are evaluated per-family by
`SecurityProviderCapabilitiesFor`.

| Capability | Meaning |
|------------|---------|
| `supports_authn` | Provider can authenticate connections |
| `supports_authz_claims` | Provider can supply authorization group claims |
| `supports_group_query` | Provider supports active group queries |
| `supports_transitive_group_expansion` | Provider can expand transitive group membership |
| `supports_membership_path_explain` | Provider can explain the path to group membership |
| `supports_mfa` | Provider participates in multi-factor authentication |
| `supports_token_introspection` | Provider can introspect tokens (e.g., OIDC introspection) |
| `supports_credential_rotation` | Provider supports in-band credential rotation |

## Plugin Trust API

Source: `plugin_trust_api.hpp`.

The plugin trust API has two entry points:

**`EngineEvaluateUdrTrust`** — evaluates whether a UDR (user-defined routine or
extension) plugin meets the trust requirements. Result: `admitted = true|false`.

**`EngineEvaluateManagerAdmission`** — evaluates whether a manager-level plugin
meets the admission requirements. Result: `admitted = true|false`.

The manifest_policy probe (`sb_auth_plugin_manifest_policy_probe`) tests four
trust rejection cases:

1. Provider with invalid signature is rejected (not admitted)
2. Provider with a missing required dependency is rejected
3. Provider whose ABI version is not supported is rejected
4. Provider with a stale implementation version is rejected

These checks occur at registration time via `EngineRegisterAuthProvider`, not at
authentication time. A provider that fails trust evaluation will not be admitted
and cannot be used.

## Provider Registration and Lifecycle

Source: `auth_provider_plugin_api.hpp`.

The provider plugin API exposes four operations:

| API | Purpose |
|-----|---------|
| `EngineRegisterAuthProvider` | Registers and admits a provider; performs trust evaluation |
| `EngineInspectAuthProvider` | Returns the provider's current descriptor and visibility status |
| `EngineDisableAuthProvider` | Disables a previously admitted provider |
| `EngineAuthenticateProvider` | Invokes the provider to authenticate a connection |

The registry probe (`sb_auth_plugin_registry_probe`) verifies:
- A valid provider can be registered and returns `admitted = true`
- Duplicate provider UUID is rejected
- An unknown provider family is rejected
- A provider with `provider:disabled` is rejected

## Provider Policy

Source: `auth_provider_model.hpp` — `AuthProviderPolicy`.

Each admitted provider is governed by a policy record:

```
AuthProviderPolicy
  enabled                — whether this provider is active (default: false)
  allow_password_compat  — whether cleartext-compatible fallback is allowed (default: false)
  require_mfa            — whether MFA is required (default: false)
  require_group_sync     — whether group synchronization is required (default: false)
  allow_cache_stale      — whether stale cache evidence is accepted (default: false)
  allow_fixture          — whether fixture-backed paths are allowed (default: true)
  stale_behavior         — what to do with stale evidence: "deny" (default)
  group_behavior         — how to handle group evidence: "none" (default)
  cache_bounds           — cache expiry behavior: "deny_when_expired" (default)
  audit_policy_ref       — reference to the audit policy for this provider
  redaction_policy_ref   — reference to the redaction policy for this provider
```

Policy can be reloaded without restarting via `EngineReloadAuthProviderPolicy`.
The default values enforce fail-closed behavior: `stale_behavior = "deny"` and
`cache_bounds = "deny_when_expired"` mean that a provider whose external service
is temporarily unavailable will deny new authentication attempts rather than
serving cached credentials.

## Authentication Request Flow

Source: `authentication_api.hpp` — `EngineAuthenticateRequest`,
`EngineAuthenticateResult`.

The `EngineAuthenticate` API accepts:

```
EngineAuthenticateRequest
  provider_family          — the canonical provider family to use
  principal_claim          — the claimed principal identity
  credential_evidence      — the serialized credential payload
  credential_evidence_present — whether credential_evidence is supplied
  credential_invalid_claim — set by caller if credential is known invalid (used in testing)
  mfa_evidence_present     — whether MFA evidence is included
```

The result carries a `ConnectionSecurityContextRecord` (see
[security_model_overview.md](security_model_overview.md)) and an
`authenticated` boolean. A failed authentication returns `authenticated = false`
and a diagnostic code.

A mid-session `EngineRefreshSecurityContext` call re-evaluates the security
context without re-authenticating. This is used when a principal's grants or
group memberships change during an active session.

## Live Evidence Validation

Source: `auth_provider_live_adapter.hpp`, `auth_provider_live_adapter.cpp`.

The live adapter normalizes provider-specific payloads into a standard
`AuthProviderLiveEvidenceResult`:

```
AuthProviderLiveEvidenceResult
  evaluated              — was live evidence evaluation attempted?
  ok                     — did validation succeed?
  authenticated          — is the principal authenticated?
  groups_materialized    — did the provider return group evidence?
  membership_explainable — can group membership be explained (path available)?
  mfa_verified           — was MFA evidence present and verified?
  provider_family        — canonical family name
  principal              — the resolved principal identity
  credential_kind        — what kind of credential was used
```

The live adapter is activated when the request includes `adapter_mode:live`,
`provider_driver:live`, or a non-empty `provider_payload:` option. Without
these options the engine uses its fixture path (used in testing).

The adapter enforces a set of cross-family adversarial payload rejections before
dispatching to family-specific validation:
- Replay detected (`replay=true` or `replayed=true` in payload) → `provider_replay_denied`
- Expired assertion (`expired=true`) → `provider_assertion_expired`
- Algorithm downgrade (`alg` in `{none, md5, sha1, rs1}`) → `provider_algorithm_downgrade_denied`
- Key ID mismatch (`kid=mismatch`) → `provider_key_id_mismatch`
- Invalid signature (`signature=bad` or `sig=bad`) → `provider_signature_invalid`
- Revoked token (`revoked=true` or `token_revoked=true`) → `SECURITY.TOKEN_REVOKED`

These checks are enforced before family-specific validation runs, meaning they
apply to all provider families uniformly.

## External Client Dependency Gate

Source: `auth_provider_live_adapter.cpp` — `ValidateExternalClientGate`.

When a request is marked as using a real external client (`client_mode:external`,
`provider_client:real`, or `real_external_client:true`), the live adapter checks
whether the required runtime dependency for the provider family is available. If
the dependency is absent, the authentication is denied with
`real_client_dependency_missing:<dependency>`. If the external service is
unavailable, the denial is `real_client_service_unavailable`.

This gate ensures that a provider family that requires an external service
(e.g., `ldap_ad` requires `ldap_client`) cannot succeed if the service is not
reachable, rather than falling back to a less secure path.

## Challenge Flow

Source: `auth_challenge_api.hpp` — `EngineContinueAuthChallenge`.

Challenge-based authentication (for example, WebAuthn) uses a multi-step flow:

1. Initial authentication request issues a challenge.
2. The client returns a response via `EngineContinueAuthChallenge`.
3. The engine validates the challenge state and produces
   `challenge_accepted = true` on success.

The challenge_state probe (`sb_auth_plugin_challenge_state_probe`) verifies
four rejection cases:
- Expired challenge (`challenge_expired:true`) → rejected
- Replayed challenge (`challenge_replayed:true`) → rejected
- Attempt limit exceeded (`attempt_limit_exceeded:true`) → rejected
- A valid challenge → accepted (`challenge_accepted = true`)

## Credential Rotation

Source: `auth_credential_api.hpp` — `EngineRotateCredential`.

The engine supports in-band credential rotation via `EngineRotateCredential`.
This allows a provider to update a principal's stored credential (for example,
to rotate a SCRAM verifier) without requiring an external administrative action.

The secret_provider probe (`sb_auth_plugin_secret_provider_probe`, which tests
SCRAM rotation) verifies three properties:
- A valid rotation request succeeds (`rotated = true`)
- A rotation request that attempts to store reusable plaintext (`store_reusable_plaintext:true`) is rejected
- A rotation request with missing credential material is rejected

This confirms that the engine refuses to store credentials in a form that would
allow password reuse or plaintext extraction.

## Token Revocation

Source: `auth_token_api.hpp` — `EngineRevokeToken`.

Tokens (API keys, bearer tokens, OIDC tokens) can be explicitly revoked via
`EngineRevokeToken`. After revocation, subsequent authentication attempts with
the same token return `SECURITY.TOKEN_REVOKED`.

The token_authkey probe verifies:
- A valid token authentication succeeds
- A revoke operation sets `revoked = true`
- A subsequent authentication attempt with the revoked token (via the `unsynced_rejected` case) fails

## Provider Observability

Source: `auth_provider_observability_api.hpp` — `EngineInspectAuthProviderMetrics`.

Provider metrics can be inspected via `EngineInspectAuthProviderMetrics`. The
result carries `metrics_available = true|false`. Metrics are accessible only
to principals with the `OBS_METRICS_READ_FAMILY` right (as referenced in the
sblr_api probe).

## Related Pages

- [security_model_overview.md](security_model_overview.md) — principal kinds, epochs, decision model
- [auth_plugin_families.md](auth_plugin_families.md) — per-family requirements and status
- [security_policies_and_crypto.md](security_policies_and_crypto.md) — policy-pack model and crypto hardening
- [platform_configuration.md](platform_configuration.md) — OS-specific configuration for each family
