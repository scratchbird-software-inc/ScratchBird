# Security Model Overview

## Purpose

This page describes the layered security model that ScratchBird applies to
every operation. Understanding the model is a prerequisite for configuring
authentication providers, writing grant policies, or diagnosing access
refusals. The invariants documented here are implemented in
`src/engine/internal_api/security/security_model.hpp` and enforced throughout
the engine's API surface.

## Definitions

**Principal** — any entity that can hold identity within the engine. Principals
include users, services, and system actors. Each principal has a durable UUID
identity recorded in the catalog. Names are resolver inputs only; the UUID is
the authority.

**Provider** — a plugin that supplies authentication evidence. Providers do not
grant rights. They supply evidence that the engine evaluates to produce a
`ConnectionSecurityContextRecord`.

**Claim** — a piece of evidence supplied by a provider (for example: a subject
identity, a set of group names, or a credential kind). Claims are normalized by
the engine; they are not treated as authority on their own.

**Materialization** — the process of evaluating the catalog's durable grant,
role, and group records against a principal UUID to produce an
`EngineMaterializedAuthorizationContext`. Authorization is materialized from
catalog policy, not inferred from claims.

**Fail-closed** — the invariant that when any required evidence is missing,
stale, ambiguous, or contradicted by an explicit denial, the engine refuses the
operation rather than guessing or defaulting to allow. This is a
source-documented invariant. The default security posture policy profile
(`deny_by_default`) reflects it.

**Epoch** — a monotonically increasing counter that tracks the generation of
security state. Three epochs are maintained: `security_epoch` (principal,
role, and group mutations), `policy_epoch` (policy and grant mutations), and
`catalog_generation_id` (catalog generation). Caches must be invalidated when
their observed epoch diverges from the current epoch.

**Security context** — the `ConnectionSecurityContextRecord` that is created at
authentication time and accompanies every request. It carries:
`effective_user_uuid`, `authority_uuid`, `policy_epoch`, `security_epoch`,
`active_roles`, `effective_groups`, `authorization_trace_tags`,
`external_provider_evidence`, `cache_expiry`, `transaction_start_policy`,
`disclosure_policy`, and `audit_policy_ref`.

## The Three-Layer Model

Source: `authentication_api.hpp`, `authorization_api.hpp`, `deep_enforcement_api.hpp`.

```
Layer 1: Authentication
  Client presents credential evidence
  Engine calls EngineAuthenticate → produces ConnectionSecurityContextRecord
  A failed authentication returns SECURITY.AUTHENTICATION.FAILED (or a more
  specific code). No further processing occurs.

Layer 2: Authorization
  Engine calls MaterializeDurableAuthorizationContext for the principal UUID
  Engine calls EvaluateMaterializedAuthorization for the requested right
  and target UUID
  Default decision is "deny" (fail-closed)
  Explicit denial wins over allow

Layer 3: Deep Enforcement
  Engine calls EngineEvaluateDeepSecurity for each executor/storage operation
  Answers: admitted, authorized, visible, masked, rls_applied, audit_written,
  side_effect_permitted
  This API is not a parser hook; it executes inside the engine after the
  parser has translated the statement.
```

Parser routes do not grant authority. A parser accepted on a given route still
passes through the full engine authorization chain before any data is read or
written.

## Principal Kinds

Source: `security_principal_lifecycle.hpp` — `EngineSecurityPrincipalRecord`.

| Kind | Lifecycle states | Notes |
|------|-----------------|-------|
| `user` | `active`, `disabled` | Interactive or application accounts identified by name |
| `service` | `active`, `disabled` | Non-interactive workloads, background jobs, API integrations |
| `system_actor` | Internal only | Engine subsystems; cannot be created by operators |

The `principal_kind` field defaults to `"user"`. A disabled principal fails
authentication regardless of credential validity (diagnostic:
`SECURITY.PRINCIPAL_DISABLED`). The engine also normalizes `"enabled"` as an
alias for `"active"` and `"disable"` as an alias for `"disabled"` during
mutations.

## Roles and Groups

Source: `security_principal_lifecycle.hpp` — `EngineSecurityRoleRecord`,
`EngineSecurityGroupRecord`.

**Roles** are owned by a principal (`owner_principal_uuid`) and may be granted
to other principals. A role's active privilege set is computed by materializing
the grants attached to its UUID.

**Groups** carry an `external_authority_ref` field. This field links the group
to an identity provider's group claim, enabling provider-sourced group
membership to flow into the engine's authorization model. An empty
`external_authority_ref` means the group is locally managed.

## Authorization Records

Source: `security_model.hpp` — `DurableAuthorizationState`.

The durable authorization state consists of:

| Record | Purpose |
|--------|---------|
| `DurableAuthorizationPrincipalRecord` | A principal's active status and security epoch |
| `DurableAuthorizationRoleRecord` | A role's active status and security epoch |
| `DurableAuthorizationGroupRecord` | A group's active status and security epoch |
| `DurableAuthorizationMembershipRecord` | An edge from a member (any kind) to a parent (role or group) |
| `DurableAuthorizationGrantRecord` | A right granted or denied to a subject for a target; carries `deny` flag |
| `DurableAuthorizationPolicyRecord` | A policy object with `requires_runtime_recheck` flag |

Materialization is requested via `DurableAuthorizationMaterializeRequest`,
which carries the principal UUID and the caller's observed epoch values. The
result is `DurableAuthorizationMaterializeResult`, which carries the
`EngineMaterializedAuthorizationContext`. If any epoch has advanced, the result
reflects the current catalog state.

## Authorization Decision

Source: `security_model.hpp` — `MaterializedAuthorizationDecision`.

```
authorized  — the principal holds the required right for the target
denied      — an explicit deny record was present
policy_recheck_required — a policy with requires_runtime_recheck was matched
decision    — "deny" (default), "allow", or "deny_explicit"
```

The default decision is `"deny"`. A decision of `"allow"` requires at least one
grant record to match and no deny record to override it.

## Security and Policy Epochs

Source: `security_principal_lifecycle.hpp`.

Each mutation to principals, roles, groups, memberships, grants, or policies
increments `security_generation` or `policy_generation` and returns a
`cache_invalidation_epoch`. Callers that cache authorization results must
validate their cached epoch via `EngineSecurityValidatePolicyCache` before
using cached data. A stale cache is refused (`SECURITY.POLICY.CACHE_STALE`).

## Offline and Stale Provider Behavior

Source: `security_model.hpp` — `SecurityAuthorityDescriptor`.

The `offline_behavior` field on a `SecurityAuthorityDescriptor` defaults to
`"deny_new_connections"`. This means that when the configured provider is
unreachable, new connection attempts are denied rather than bypassed. This is
a fail-closed behavior at the authority level.

The `AuthProviderPolicy` struct carries `stale_behavior = "deny"` and
`cache_bounds = "deny_when_expired"` as defaults. A provider whose evidence
cache has expired will deny authentication rather than serving stale claims.

## Diagnostic Codes

Source: `security_principal_lifecycle.hpp` — inline constants.

Key diagnostic codes emitted by the security principal lifecycle:

| Code | Meaning |
|------|---------|
| `SECURITY.PRINCIPAL.DATABASE_PATH_REQUIRED` | Operation requires a database path |
| `SECURITY.PRINCIPAL.MGA_TRANSACTION_REQUIRED` | Principal mutation requires an MGA transaction |
| `SECURITY.PRINCIPAL.AUTHORITY_REQUIRED` | No authority context was present |
| `SECURITY.PRINCIPAL.AUTHORITY_BYPASS_REFUSED` | An attempt to bypass authority was refused |
| `SECURITY.PRINCIPAL_INVALID` | The principal UUID is unknown or invalid |
| `SECURITY.PRINCIPAL_DISABLED` | The principal exists but is disabled |
| `SECURITY.PRINCIPAL.DUPLICATE` | A principal with this identity already exists |
| `SECURITY.ROLE_INVALID` | The role UUID is unknown or invalid |
| `SECURITY.GROUP_INVALID` | The group UUID is unknown or invalid |
| `SECURITY.GRANT_INVALID` | The grant is malformed or refers to unknown objects |
| `SECURITY.ACCESS_DENIED` | Access was denied by the authorization model |
| `SECURITY.PRIVILEGE.DEFAULT_DENY` | No matching grant was found; default-deny applied |
| `SECURITY.PRIVILEGE.GRANT_NOT_VISIBLE` | A grant was referenced but is not visible to this context |
| `SECURITY.POLICY_MISSING` | A required policy object is absent |
| `SECURITY.POLICY.STALE` | The policy epoch has advanced; recheck required |
| `SECURITY.POLICY.CACHE_STALE` | Cached authorization data has become stale |
| `SECURITY.AUDIT.EVIDENCE_REQUIRED` | An audit-before-success obligation was not met |
| `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` | A call path attempted to return plaintext protected material |

## Where Each Layer Lives in Source

| Concern | Source file |
|---------|------------|
| Security model types and epoch machinery | `security_model.hpp`, `security_model.cpp` |
| Authentication entry point | `authentication_api.hpp`, `authentication_api.cpp` |
| Authorization materialization | `authorization_api.hpp`, `authorization_api.cpp` |
| Deep enforcement (executor/storage) | `deep_enforcement_api.hpp`, `deep_enforcement_api.cpp` |
| Principal, role, group lifecycle | `security_principal_lifecycle.hpp`, `security_principal_lifecycle.cpp` |
| Provider model and registry | `auth_provider_model.hpp`, `auth_provider_model.cpp` |
| Live evidence validation per family | `auth_provider_live_adapter.hpp`, `auth_provider_live_adapter.cpp` |
| Cryptographic primitives | `security_crypto_policy.hpp`, `security_crypto_policy.cpp` |
| Plugin trust and manager admission | `plugin_trust_api.hpp`, `plugin_trust_api.cpp` |
| Identity create/alter | `identity_api.hpp`, `identity_api.cpp` |

## Related Pages

- [authentication_and_providers.md](authentication_and_providers.md) — how a provider is declared, trusted, and invoked
- [auth_plugin_families.md](auth_plugin_families.md) — per-family reference for all 18 plugin families
- [security_policies_and_crypto.md](security_policies_and_crypto.md) — policy-pack model and cryptographic policy
- [Operations and Administration: Identity, Security, and Policy](../Operations_Administration/identity_security_and_policy.md)
- [Language Reference: Security and Sandboxing](../Language_Reference/core_paradigms/security_and_sandboxing.md)
