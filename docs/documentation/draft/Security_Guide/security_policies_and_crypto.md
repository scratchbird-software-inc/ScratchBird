# Security Policies and Cryptographic Policy

## Purpose

This page describes how security policies are structured and applied in
ScratchBird: the policy-pack model, the `default-local-password` pack that is
seeded at database creation time, the cryptographic policy implemented in
`security_crypto_policy`, and the hardening options available for token and
credential management.

Sources:
- `resources/policy-packs/default-local-password/` — default policy pack
- `src/engine/internal_api/security/security_crypto_policy.hpp`, `.cpp`
- `src/engine/internal_api/security/auth_provider_model.hpp` — `AuthProviderPolicy`
- `src/engine/internal_api/security/auth_provider_policy_api.hpp`
- `config/templates/SBsrv.conf`

## The Policy-Pack Model

A **policy pack** is a signed bundle of JSON documents that seeds the security
policy of a new database at creation time. It is not applied at runtime; once
a database is created, its policy is owned by the catalog and can only be
mutated through database commands (via `EngineMutatePolicy`).

Source: `identity_security_and_policy.md` — "Policies are created as part of
database bootstrap and can be mutated only through database commands, not through
filesystem policy packs at runtime."

The policy pack format is described by the `POLICY_PACK_MANIFEST.json` file.

### Policy Pack Manifest Fields

```json
{
  "schema_version": 1,
  "policy_pack_uuid": "018f7a10-1280-7000-8000-000000000001",
  "policy_pack_id": "default-local-password",
  "policy_pack_version": "1.0.0",
  "description": "First public release default policy pack: local password provider only.",
  "content_hash_algorithm": "sha256",
  "content_sha256": "<hash>",
  "signature_status": "signature-ready-unsigned",
  "provenance": {
    "source": "public-project-tree",
    "generated": false,
    "private_inputs_required": false,
    "external_provider_runtime_required": false
  },
  "create_time_only": true,
  "post_create_filesystem_authority": false
}
```

Key fields:

- `signature_status: "signature-ready-unsigned"` — the pack is ready to
  sign but has not been signed in the public project tree. Operators deploying
  from the source tree should be aware of this status.
- `create_time_only: true` — the pack is applied only at database creation time.
- `post_create_filesystem_authority: false` — after creation, the filesystem
  pack has no authority over the database's policy.
- `content_sha256` — the pack's content integrity hash. Verify this against the
  files before use.

### Default-Local-Password Pack Contents

The `default-local-password` pack contains five policy files:

| File | Purpose |
|------|---------|
| `policies/security_providers.json` | Provider family declarations and their default enabled state |
| `policies/roles.json` | Standard role seed records |
| `policies/groups.json` | Standard group seed records |
| `policies/grants.json` | Standard privilege grant seed records |
| `policies/policy_profiles.json` | Profile-area-to-mode mappings governing default system behavior |
| `catalog_materialization.json` | Catalog materialization metadata |

### Provider Policy in the Pack

From `policies/security_providers.json`:

Only `local_password` is enabled by default:

```json
{
  "provider_family": "local_password",
  "enabled_by_default": true,
  "authority": "durable_catalog_row",
  "credential_verifier_policy": "local_password_verifier_required",
  "external_provider": false
}
```

All other provider families — `ldap_ad`, `oidc_jwt`, `saml`, `kerberos_pac`,
`pam`, `radius`, `webauthn`, `certificate_mtls`, `workload_identity`,
`managed_identity`, `custom_cpp_plugin` — are declared with
`"enabled_by_default": false` and `"authority": "unsupported_by_default"`.

This means a freshly created database will reject authentication attempts from
any provider family other than `local_password` until the operator explicitly
enables an additional family and satisfies its dependency requirements.

### Policy Profiles

From `policies/policy_profiles.json`:

Policy profiles govern the default posture for named areas of engine behavior.
The profiles seeded by the default pack are:

| Profile area | Mode | Meaning |
|-------------|------|---------|
| `security_provider_selection` | `local_password_only` | Only local password is active |
| `standard_roles_groups_grants` | `uuid_catalog_seed` | Roles/groups/grants are seeded from UUID catalog records |
| `default_security_posture` | `deny_by_default` | No access is assumed; all access requires explicit grant |
| `memory_resource_governance` | `configured_policy_required` | Memory policy must be explicitly configured |
| `storage_filespace_page_policy` | `local_durable_fail_closed` | Storage operations fail closed |
| `transaction_mga_cleanup_archive_backup_forward` | `mga_inventory_authority` | MGA owns transaction inventory |
| `optimizer_statistics_feedback` | `catalog_backed_or_diagnostic_only` | Optimizer stats are catalog-backed |
| `index_maintenance` | `provider_admission_required` | Index maintenance requires admission |
| `agent_policy` | `evidence_not_authority` | Agent evidence is not authority |
| `diagnostics` | `stable_redacted_diagnostics` | Diagnostics are redacted |
| `observability` | `redacted_evidence_only` | Observability output is redacted |
| `unsupported_feature_behavior` | `deterministic_fail_closed` | Unsupported features fail closed |
| `cluster_boundary` | `external_provider_required` | Cluster operations require an external provider |
| `release_default_configuration` | `secure_defaults` | Release defaults are secure |

The `deny_by_default` posture for `default_security_posture` is the source for
the fail-closed invariant described in [security_model_overview.md](security_model_overview.md).

## Cryptographic Policy

Source: `security_crypto_policy.hpp`, `security_crypto_policy.cpp`.

The security crypto policy provides the approved cryptographic primitives for
authentication and security-event integrity. The source comment documents:

> OpenSSL supplies the approved SHA-256/HMAC-SHA-256 algorithms; equality
> checks stay constant-time with respect to input length.

### Approved Primitives

| Function | Algorithm | Use |
|----------|-----------|-----|
| `SecurityConstantTimeEqual` | Constant-time string comparison | Comparing credentials or tokens |
| `SecuritySha256Hex` | SHA-256 | Integrity of security payloads |
| `SecurityHmacSha256Hex` | HMAC-SHA-256 | Keyed integrity of security payloads |

### Cluster Evidence Integrity

The engine also supports cluster catalog evidence integrity via
`EvaluateClusterEvidenceIntegrity`. The supported protection modes are:

| Mode | Algorithm |
|------|-----------|
| `sha256` | SHA-256 digest (default for simple integrity) |
| `hmac_sha256` | HMAC-SHA-256 with a named key |
| `signature_ready_ed25519` | Signature-ready metadata with Ed25519 key reference |

The `ClusterEvidenceIntegrityResult` carries `fail_closed = true` and
`weak_evidence_rejected = false` (becomes `true` when a weak evidence attempt
is made). The source comment states: "Weak checksums cannot support catalog
authority claims." This means weak hash algorithms (MD5, SHA-1, CRC) will be
rejected if they are presented as catalog authority evidence.

### Algorithm Downgrade Denial

The live adapter enforces algorithm downgrade denial across all provider families.
From `auth_provider_live_adapter.cpp` — `RejectAdversarialPayload`:

```
alg in {none, md5, sha1, rs1} → provider_algorithm_downgrade_denied
```

This check runs before family-specific validation. It applies to all provider
families that present an `alg` field in their payload.

Operators configuring OIDC JWT providers must ensure the IDP issues tokens
with an approved algorithm. The `oidc_jwt` family additionally enforces
`alg != "none"` as a family-specific check.

## Provider Policy Hardening

Source: `auth_provider_model.hpp` — `AuthProviderPolicy`.

Each admitted provider's policy can be configured for stricter behavior:

### Disabling Password-Compat Fallback

```json
"allow_password_compat": false
```

The default is `false`. Cleartext password compatibility (`password_compat`
family) is refused unless this is explicitly set to `true`. The `password_compat`
family is also `policy_gated` and requires `allow_password_compat` to be set in
the provider policy. Do not set this to `true` in new deployments.

### Requiring MFA

```json
"require_mfa": true
```

When set, the provider policy requires MFA evidence to be present in every
authentication request. This works in conjunction with `webauthn` or
`factor_chain` families.

### Requiring Group Sync

```json
"require_group_sync": true
```

When set, the provider policy requires group synchronization to have been
performed recently. Combined with `stale_behavior: "deny"`, this prevents
principal authentication when group state cannot be verified.

### Stale Evidence Behavior

```json
"stale_behavior": "deny",
"allow_cache_stale": false,
"cache_bounds": "deny_when_expired"
```

These are the default values. They enforce fail-closed behavior: when the
provider's external service is unavailable, cached credentials expire and new
authentication attempts are denied rather than serving stale data.

### Audit and Redaction Policy

```json
"audit_policy_ref": "<policy-uuid>",
"redaction_policy_ref": "<policy-uuid>"
```

Each provider can reference a separate audit policy and redaction policy. These
should be configured to route provider-level authentication events to the audit
system and to apply appropriate redaction to authentication evidence in
diagnostic output.

## Token Hardening

The following token hardening behaviors are enforced uniformly across all
token-based provider families (OIDC JWT, SAML, WebAuthn, bearer token, API key):

| Hardening | Enforcement point | Diagnostic code |
|-----------|------------------|-----------------|
| Algorithm downgrade denial | `RejectAdversarialPayload` in live adapter | `provider_algorithm_downgrade_denied` |
| Replay detection | `RejectAdversarialPayload` in live adapter | `provider_replay_denied` |
| Token revocation | `RejectAdversarialPayload` in live adapter | `SECURITY.TOKEN_REVOKED` |
| Assertion freshness | Family-specific `Validate*` | Family-specific (e.g., `oidc_jwt_expired`, `saml_assertion_expired`) |
| User verification for WebAuthn | `ValidateWebAuthn` | `webauthn_user_verification_required` |
| OIDC algorithm `none` | `ValidateOidcJwt` | `oidc_jwt_alg_none_forbidden` |
| OIDC validator boundary | `ValidateOidcJwt` | `oidc_jwt_validator_boundary_required` |

These checks are not configurable; they are source-enforced invariants.

## Credential Hardening

From `security_principal_lifecycle.hpp` — `EngineSecurityCreatePrincipalResult`:

```
plaintext_material_stored = false   — confirmed by default
protected_material_redacted = true  — confirmed by default
```

The engine returns these flags to the caller after principal creation. When
`plaintext_material_stored = false` and `protected_material_redacted = true`,
no plaintext credential has been stored or returned. Operators should confirm
these flags in audit output after principal provisioning.

The diagnostic code `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` is emitted
if a call path attempts to return plaintext protected material.

## Policy Reload

Source: `auth_provider_policy_api.hpp` — `EngineReloadAuthProviderPolicy`.

Provider policy can be reloaded without restarting the server. The reload
operation returns the current `AuthProviderPolicy` and sets
`reloaded = true` on success. This allows operators to tighten or relax
provider policy (within the bounds of what the engine permits) without
a full server restart.

## Related Pages

- [security_model_overview.md](security_model_overview.md) — fail-closed invariant, epochs
- [authentication_and_providers.md](authentication_and_providers.md) — provider policy fields
- [auth_plugin_families.md](auth_plugin_families.md) — per-family policy-gate requirements
- [platform_configuration.md](platform_configuration.md) — platform-specific configuration
- [Operations and Administration: Identity, Security, and Policy](../Operations_Administration/identity_security_and_policy.md)
