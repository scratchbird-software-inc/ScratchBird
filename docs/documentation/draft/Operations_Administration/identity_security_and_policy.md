# Identity, Security, And Policy

## Purpose

This chapter defines the operator-facing model for identity, authentication, authorization, schema roots, protected material, policy, and redaction in ScratchBird. Understanding these concepts before configuring a production deployment prevents a class of hard-to-diagnose access failures that stem from conflating who a principal is (identity), how they prove it (authentication), and what they are allowed to do (authorization).

## The Three-Layer Model

ScratchBird separates identity concerns into three distinct layers that execute in sequence for every connection:

1. **Authentication** — proves that the client is who they claim to be, using a configured provider family.
2. **Authorization** — determines what the authenticated principal may do by evaluating grants, roles, and group memberships against the requested operation.
3. **Deep enforcement** — applies masking, row-level security, and audit obligations as the engine executes the work.

Parser routes do not grant authority by themselves. A parser accepted on a given route still passes through the full engine authorization chain before any data is read or written.

## Principal Kinds

ScratchBird recognizes three principal kinds, validated in `security_principal_lifecycle.cpp`:

| Kind | Typical use |
|------|-------------|
| `user` | Interactive or application accounts identified by name |
| `service` | Non-interactive workloads, background jobs, API integrations |
| `system_actor` | Internal engine subsystems; not creatable by operators |

Each principal has a lifecycle state of either `active` or `disabled`. The engine also accepts `enabled` as an alias for `active` and `disable` as an alias for `disabled` during mutations; these are normalized internally. A disabled principal will fail authentication regardless of credential validity.

Roles and groups are tracked separately from principals. A role is owned by a principal and may be granted to other principals. A group carries an `external_authority_ref` field that links it to an identity provider's group claim, enabling provider-sourced group membership to flow into the engine's authorization model.

## Authentication Providers

The engine supports a broad set of authentication provider families. The mapping between a provider family name and its required runtime dependency is maintained in `auth_provider_live_adapter.cpp`:

| Provider family | Required dependency |
|-----------------|---------------------|
| `ldap_ad` | `ldap_client` |
| `kerberos_pac` | `gssapi_krb5` |
| `pam` | `pam` |
| `radius` | `radius_client` |
| `oidc_jwt` | `oidc_jwt_client` |
| `saml` | `saml_xmlsig` |
| `webauthn` / `factor_chain` | `webauthn_fido2` |
| `workload_identity` / `managed_identity` | `spiffe_svid_or_workload_oidc` |
| `certificate_mtls` | `tls_x509` |
| `proxy_assertion` | `proxy_assertion_verifier` |
| `remote_security_database` | `remote_scratchbird_security` |
| `bearer_token` | _(none required)_ |
| `token_api_key` | _(none required)_ |

If a required dependency is absent at authentication time, the engine returns a denial with the detail `real_client_dependency_missing:<dependency>` rather than silently falling back to an insecure path.

### LDAP / Active Directory

LDAP authentication requires `starttls=true` — plain-text LDAP binds are refused with diagnostic `ldap_starttls_required`. Group materialization is also required; a successful bind without groups in the payload returns `ldap_group_materialization_required`.

### PAM

The PAM provider requires a hidden or secret prompt mode. A PAM conversation with an insecure prompt type is refused as `pam_insecure_prompt`. Both the account phase and the session-open phase must succeed.

### RADIUS

The RADIUS authenticator value must be a valid 64-character hex string. A result other than `accept` from the RADIUS server returns `radius_rejected`.

### Proxy Assertion

The proxy assertion provider (`proxy_assertion`) requires a verified source, confirmed manager trust, and a validated listener binding. All three must be `trusted` or `verified`; a failure on any returns the corresponding denial code. This provider is intended for trusted middle-tier components that forward a user's identity rather than for direct user authentication.

### Token Hardening

Across all token-based providers (OIDC JWT, SAML, WebAuthn, bearer token, API key), the engine enforces:

- Algorithm downgrade denial — algorithms `none`, `md5`, `sha1`, and `rs1` are refused with `provider_algorithm_downgrade_denied`.
- Replay detection — a payload marked as replayed returns `provider_replay_denied`.
- Token revocation — a revoked token returns `SECURITY.TOKEN_REVOKED`.

### Multi-Factor Authentication

The `factor_chain` provider enforces a multi-factor policy. A chain where `factor_results != allow` returns `SECURITY.MFA_REQUIRED`. The challenge transcript hash must be a valid 64-character hex value.

## Authentication Request Structure

The engine's `EngineAuthenticate` API accepts a `provider_family`, a `principal_claim`, and `credential_evidence`. The result carries a `ConnectionSecurityContextRecord` that becomes the security context for the session. A subsequent `EngineRefreshSecurityContext` call updates the context if the principal's grants or group memberships change mid-session.

Denied authentication always produces a diagnostic with code `SECURITY.AUTHENTICATION.FAILED` or a more specific code such as `SECURITY.TOKEN_REVOKED` or `SECURITY.MFA_REQUIRED`. These codes are safe to return to clients.

## Authorization and Grants

Authorization is materialized from durable grant records. The `EngineGrantRight` and `EngineRevokeRight` APIs record grant mutations into the catalog. The materialized authorization context for a connection lists its effective subjects — the principal's own UUID plus any roles and groups the principal is a member of. An operation is admitted only when the materialized context carries a matching privilege for the target object.

The default authorization policy at database creation is `default_deny_explicit_allow` (from the bootstrap policy `security.authorization_default`). No privilege is assumed unless explicitly granted.

## Deep Security Enforcement

The `EngineEvaluateDeepSecurity` API is the single authority point for executor, storage, and catalog callers. A single evaluation answers:

- Is the operation admitted by rights? (`admitted`, `authorized`)
- Is the object visible to this principal? (`visible`)
- Should masking be applied? (`masked`)
- Should row-level security filter the result? (`rls_applied`)
- Has audit been recorded before success? (`audit_written`)
- Is the side effect permitted? (`side_effect_permitted`)

This API is not a parser hook; it executes inside the engine after the parser has translated the statement.

## Schema Roots

When a database is created, the engine bootstraps a fixed set of schema root paths. These paths are defined in `bootstrap_schema_roots.hpp` and include:

| Path | Purpose |
|------|---------|
| `sys` | Engine system namespace root |
| `sys.catalog` | Catalog visibility |
| `sys.security` | Security catalog tables |
| `sys.metrics` | Metrics and observability |
| `sys.audit` | Audit event records |
| `sys.storage` | Storage and filespace management views |
| `sys.parser` | Parser registration and configuration |
| `sys.diagnostics` | Diagnostic output |
| `sys.information` / `sys.information_schema` | Standard information schema views |
| `users` | User home schema root |
| `users.public` | Default public schema |
| `remote` | Remote access schema surface |
| `emulated` | Emulated compatibility schema overlay |

Schema roots are created with fresh UUIDv7 identifiers at database creation time; the paths are fixed but the UUIDs are not pre-determined. User home schemas are created under the `users` tree by default (policy `security.user_home_schema`). The `kLocalUserHomePolicyRoot` constant (`"users"`) governs the default home root.

A parser workarea is the subset of schema roots that the parser process can resolve names against. Each parser operates inside this boundary; catalog objects outside the parser's assigned workarea are not resolvable from that parser.

## Protected Material

Protected material is the engine's term for encryption keys, secrets, and credential artifacts that must never appear in diagnostic output, logs, or support bundles. The `protected_material_api.hpp` enforces this contract: every result struct that could potentially return secret content includes a `protected_material_redacted = true` field and a `plaintext_material_returned = false` field. No call path that is expected to keep material protected returns plaintext.

Encryption keys are admitted into the engine's key cache via `EngineAdmitEncryptionKey`. The cache entry carries a configurable `cache_ttl_millis` (default 300,000 ms). On shutdown, the engine purges all key cache entries (`EngineShutdownProtectedMaterial`). Key rotation is supported through `EngineRotateEncryptionKey`, which records rotation metadata durably without persisting plaintext.

The `EngineOpenEncryptedFilespace` call gates opening a filespace that was created with encryption. If the required key is not in the cache or has expired, the open is refused.

Protected material versions support legal hold (`legal_hold = true` in `EngineProtectedMaterialPolicySet`). A purge operation that encounters a version under legal hold is refused by retention (`refused_by_retention`).

## Support Bundle Redaction

When a support bundle is generated, the `RedactManagerSupportBundleText` function in `manager_support_bundle.cpp` scans every line of every included file and replaces the values following these sensitive key names with `[redacted]`:

`password`, `passwd`, `secret`, `token`, `private_key`, `credential`, `verifier`, `encryption_key`, `decryption_key`, `key_handle`

In addition, filesystem paths are replaced with `[path-redacted]`. The redaction is applied before any file is written into the support bundle archive. Operators should not rely on the support bundle as a source of credential material.

## Audit Events

The audit subsystem records two event shapes: `EngineEmitAuditEvent` for general events (carrying an `event_class` and `outcome`), and `EngineEmitLifecycleAuditEvent` for lifecycle mutations (carrying an `operation_key`, `diagnostic_code`, and `correlation_uuid`). Lifecycle audit events also track whether a cache invalidation was recorded. All audit events default `redacted = true`; the emitting code must explicitly clear redaction only when it has confirmed the payload contains no protected material.

The bootstrap policy `security.audit` (`security_activity_audit_v1`) enables audit of security events, policy mutations, and database creation. Evidence retention duration is governed by the `evidence.retention` policy (`audit_minimum_v1`).

## Policy Admission

Policies are created as part of database bootstrap and can be mutated only through database commands, not through filesystem policy packs at runtime (`policy_api.hpp`). The `EngineMutatePolicy` API records each mutation into the MGA catalog with an audit event and increments the policy epoch. Filesystem packs are create-time seeds only.

## Refusal Behavior

Every denied access returns a controlled message vector. Raw strings are forbidden; all diagnostic codes reference a registered message key. The `diagnostics.message_vector` bootstrap policy (`canonical_redacted_v1`) mandates redaction and requires a correlation ID on every vector. Operators observing an access refusal should inspect the returned message vector code rather than the raw detail string.

## Operator Checklist

- Verify that the required runtime dependency for each configured provider family is installed before enabling that provider.
- Use `system_actor` principals only when directed to; do not attempt to create them.
- Confirm that `starttls` is set when configuring LDAP. Binds without TLS are refused at the engine level, not just at policy level.
- Review the support bundle redaction list before adding new configuration keys that may contain credentials.
- Do not place raw secret values in parser packets, scripts, or configuration. Use protected material handles instead.

## Related Pages

- [Configuration Reference](configuration_reference.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Language Reference: Security And Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md)
- [Language Reference: Policy, Masking, And Row-Level Security](../Language_Reference/syntax_reference/policy_mask_and_rls.md)
- [Getting Started: Identity, Authentication, And Authorization](../Getting_Started/architecture/identity_authentication_and_authorization.md)
