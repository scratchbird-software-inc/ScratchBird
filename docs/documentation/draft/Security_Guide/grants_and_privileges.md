# Grants and Privileges

## Purpose

This page explains the grant and revoke model in ScratchBird: how privileges
are granted to principals, how the grant table is materialized into an
authorization context, how explicit denials work, and how definer versus
invoker context affects privilege evaluation.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/grant_api.hpp`,
`grant_api.cpp`, `authorization_api.hpp`, `security_model.hpp`,
`security_model.cpp`, `security_principal_lifecycle.hpp`.

For syntax reference, see:
- [Language Reference: Security and Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md)

That page documents the full SBsql `GRANT` and `REVOKE` syntax. This page
covers the underlying engine mechanics.

## Definitions

**Privilege** — a right granted to a grantee for a specific target object.
Stored as an `EngineSecurityPrivilegeGrantRecord` (or
`DurableAuthorizationGrantRecord` in the materialized model). The `privilege`
field carries the right name (must be in `KnownRights()`); the `grant_effect`
field defaults to `"allow"` but can be `"deny"` for explicit denials.

**Grantee** — the subject of the grant. The `grantee_kind` field on
`EngineSecurityPrivilegeGrantRecord` defaults to `"principal"`. Roles and
groups also accept grants; the `DurableAuthorizationGrantRecord` carries a
`subject_kind` field that can be `"principal"`, `"role"`, or `"group"`.

**Grant option** — the ability to re-grant a privilege. Expressed as `WITH
GRANT OPTION` in SBsql. Not separately tracked at the engine API level; the
engine checks that the grantor holds `SEC_GRANT_ADMIN`.

**Admin option** — the ability to administer a role membership edge. Expressed
as `WITH ADMIN OPTION` in SBsql for role grants.

**Definer context** — a callable routine that runs with the security context of
its definer principal rather than the caller's context. The
`EngineSecurityDefinerRightsCacheRecord` caches definer-privilege evaluations.

**Invoker context** — a callable routine that runs with the caller's security
context. The default for routines unless explicitly declared otherwise.

## Grant API

Source: `grant_api.cpp`.

```cpp
// Requires SEC_GRANT_ADMIN on the target object
EngineGrantRightResult  EngineGrantRight(const EngineGrantRightRequest&);
EngineRevokeRightResult EngineRevokeRight(const EngineRevokeRightRequest&);
```

Both functions require the calling principal to hold `SEC_GRANT_ADMIN` on the
target object. If the caller lacks this right, both functions return a security
failure with `SECURITY.AUTHORIZATION.DENIED` and detail `SEC_GRANT_ADMIN`.

A security context must be present (`security_context_present = true`).
Requests without a security context are refused with
`SECURITY.AUTHENTICATION.REQUEST_INVALID`.

`EngineGrantRight` additionally validates that the right string is in
`KnownRights()`. Granting an unknown right returns
`SECURITY.AUTHORIZATION.DENIED` with detail `unknown_right:<name>`.

Advisory diagnostics:
- Granting `OBS_INDEX_PROFILE_READ` to a `DEV` group member emits a
  `SB_ENGINE_API_DEV_ONLY_RIGHT_WARNING` advisory (non-fatal).
- Granting `DOMAIN_UNMASK` to a `DEV` group member emits the same advisory.

## Grantee Model

Grants can be issued to four principal kinds, reflected in `grantee_kind` or
`subject_kind`:

| Grantee Kind | Description |
|-------------|-------------|
| `principal` | A specific user or service principal identified by UUID |
| `role` | A role object; principals holding the role inherit the grant |
| `group` | A group object; principals in the group inherit the grant |
| `public` | The `PUBLIC` pseudo-principal; all attached sessions |

Granting to a role or group causes the right to be inherited by all principals
that are currently members of that role or group, as determined at
authorization materialization time.

Granting to `PUBLIC` should be used only for rights that are genuinely
intended for every session, such as `CONNECT` on a database meant to be
publicly accessible.

## Grant Targets

A grant target binds to an object UUID, not to a display name. The target class
determines which privileges are valid. The full list of target classes accepted
in the `GRANT` surface:

| Target Class | Example Rights |
|-------------|---------------|
| `DATABASE` | `CONNECT`, `CREATE SCHEMA`, `BACKUP`, `RESTORE`, `MANAGE SECURITY` |
| `SCHEMA` | `USAGE`, `CREATE TABLE`, `CREATE FUNCTION` |
| `TABLE` | `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `TRUNCATE` |
| `COLUMN` | `SELECT`, `INSERT`, `UPDATE` |
| `VIEW` | `SELECT`, `ALTER`, `DROP` |
| `MATERIALIZED VIEW` | `SELECT`, `REFRESH` |
| `SEQUENCE` | `USAGE`, `SELECT`, `UPDATE` |
| `FUNCTION` | `EXECUTE` |
| `PROCEDURE` | `EXECUTE` |
| `TRIGGER` | `ALTER`, `DROP`, `ENABLE`, `DISABLE` |
| `DOMAIN` | `USAGE`, `ALTER`, `DROP` |
| `TYPE DESCRIPTOR` | `USAGE`, `ALTER`, `DROP` |
| `POLICY`, `MASK`, `RLS` | `APPLY`, `ALTER`, `DROP`, `ENABLE`, `DISABLE` |
| `FILESPACE` | `USAGE`, `CREATE OBJECT`, `ALTER`, `DROP` |
| `BRIDGE` | `CONNECT`, `IMPORT`, `EXPORT`, `REPLICATE` |
| `SYSTEM` | Policy-defined administrative privileges |

See [Language Reference: Security and Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md#privilege-classes) for the authoritative list of privileges per object class.

## Durable Grant Records

Source: `security_principal_lifecycle.hpp` —
`EngineSecurityPrivilegeGrantRecord`.

A durable grant record carries:

| Field | Type | Purpose |
|-------|------|---------|
| `grant_uuid` | UUID | Stable grant identity |
| `grantee_uuid` | UUID | Subject of the grant |
| `grantee_kind` | string | `"principal"`, `"role"`, or `"group"` |
| `target_object_uuid` | UUID | Object the grant applies to |
| `target_object_kind` | string | Object class (e.g., `"table"`, `"schema"`) |
| `privilege` | string | Right name |
| `grantor_principal_uuid` | UUID | Principal who issued the grant |
| `grant_effect` | string | `"allow"` (default) or `"deny"` |
| `security_generation` | uint64 | Security epoch at grant creation |
| `revoked` | bool | True when the grant has been revoked |

Revoked grants are retained in the record store for audit purposes but are
not included in materialized authorization contexts.

## Authorization Materialization

Source: `security_model.cpp` — `MaterializeDurableAuthorizationContext`,
`EvaluateMaterializedAuthorization`.

When the engine needs to authorize an operation, it calls
`MaterializeDurableAuthorizationContext` with the principal UUID and the
caller's observed epoch values. The materialization process:

1. Verifies that the principal is present and active in the durable state.
2. Walks the membership graph (principal → roles → groups) to collect all
   effective subjects, detecting and refusing cycles.
3. For each effective subject, collects all active `DurableAuthorizationGrantRecord`
   records whose right is in `KnownRights()`.
4. Collects all active `DurableAuthorizationPolicyRecord` records for effective
   subjects.
5. Validates epoch consistency: grants and policies whose `security_epoch`
   does not match the state's `security_epoch` cause `SECURITY.CONTEXT.EXPIRED`.

The result is an `EngineMaterializedAuthorizationContext` carrying:
- `effective_subjects` — the principal plus all transitively reachable active
  roles and groups
- `grants` — the collected `DurableAuthorizationGrantRecord` set
- `policies` — the collected `DurableAuthorizationPolicyRecord` set

Authorization is then evaluated by `EvaluateMaterializedAuthorization`:

1. Explicit deny check: if any grant for the required right on the target has
   `deny = true`, the decision is `"deny"` and the function returns immediately.
2. Allow check: if at least one grant for the required right on the target has
   `deny = false`, `allowed` is set to true.
3. Policy check: if a policy matching the right and target has `deny = true`,
   the decision is `"deny"`. If a policy has `requires_runtime_recheck = true`,
   the decision becomes `"allow_recheck_required"`.
4. Final decision: `"allow"` if `allowed` is true and no deny was triggered.
   `"deny"` in all other cases.

The default decision when no matching grant is found is `"deny"`. This is the
fail-closed invariant.

## Explicit Denial

Explicit denial is the mechanism by which an `allow` grant can be overridden.
A `DurableAuthorizationGrantRecord` with `deny = true` causes
`EvaluateMaterializedAuthorization` to return `"deny_explicit"` before any
allow grant is evaluated.

This means: if a principal belongs to a group that has been granted a right,
but that specific principal also has an explicit deny record for the same right
and target, the denial wins.

The SQL form is not directly shown in the LR grant page syntax, but the
`grant_effect` field in the engine records admits `"deny"`. Operators
implementing explicit denial should confirm the supported SBsql surface for
their version.

## Revoke Semantics

Source: `grant_api.cpp` — `EngineRevokeRight`.

`EngineRevokeRight` sets `revoked = true` on the matching durable grant record.
The revocation is written through `PersistApiBehaviorRecord` and is subject to
MGA transaction finality: the revocation is visible after commit and is
reversed by rollback.

Revocation advances `security_generation` and returns a
`cache_invalidation_epoch`. Callers that cache authorization results must
validate their cached epoch via `EngineSecurityValidatePolicyCache` before
continuing to use cached data. A stale cache is refused with
`SECURITY.POLICY.CACHE_STALE`.

`REVOKE GRANT OPTION FOR` removes the re-grant ability while preserving the
privilege itself. `REVOKE ADMIN OPTION FOR` removes role administration
authority while preserving membership.

`RESTRICT` refuses revocation if dependent grants would become invalid.
`CASCADE` removes dependent grants through an explicit cascade plan.

## Grant Option and Admin Option

`WITH GRANT OPTION` on a privilege grant means the grantee can re-grant the
same privilege to another principal. The engine enforces this by checking
`SEC_GRANT_ADMIN` at re-grant time — the grantee's authority to re-grant
is not broader than the grantor's effective authority.

`WITH ADMIN OPTION` on a role grant means the grantee can administer the role
membership edge (add or remove members). This requires `SEC_MEMBERSHIP_ADMIN`.

Grantable authority is never broader than the grantor's effective authority.
The engine does not allow a principal to grant a right they do not themselves
hold.

## Definer vs. Invoker Context

Source: `security_principal_lifecycle.hpp` —
`EngineSecurityDefinerRightsCacheRecord`, `EngineSecurityPrimeDefinerRightsCache`,
`EngineSecurityValidateDefinerRightsCache`.

**Invoker context** (default): The callable routine executes with the caller's
materialized authorization context. Every access inside the routine is checked
against the caller's grants and policies.

**Definer context**: The routine executes with the security context of its
declared definer principal UUID. The
`EngineSecurityDefinerRightsCacheRecord` caches the definer's effective
privileges for a target object and privilege name. The cache entry carries a
`policy_generation` and a `cache_key`; callers must validate the cache using
`EngineSecurityValidateDefinerRightsCache` before using cached definer rights.

A definer-context routine does not bypass RLS or masks unless the definer's
context explicitly admits it. The engine rechecks definer authority at each
execution.

## Privilege Resolution Order

For each protected operation, the engine resolves effective privileges in this
order:

1. Authenticate and bind the effective principal UUID.
2. Establish sandbox root, attached database, current schema, and active role
   set.
3. Resolve the target name or UUID under sandbox and metadata visibility rules.
4. Collect direct grants to the principal.
5. Collect grants inherited through active roles and admitted groups via the
   membership graph traversal.
6. Apply object ownership policy where applicable.
7. Apply object-class privilege rules.
8. Apply column, element, row-level, mask, protected-material, bridge, and
   system policies.
9. Apply explicit deny or refusal policy. **Denial wins over allow.**
10. Produce an admitted operation descriptor or a canonical refusal message
    vector.

Authorization is rechecked at execution. A prepared statement that was valid
when prepared can be refused later if security state, schema state, policy
state, or object state changed.

## Epoch and Cache Management

Source: `security_principal_lifecycle.hpp`.

Every mutation to principals, roles, groups, memberships, grants, or policies
increments `security_generation` or `policy_generation` and returns a
`cache_invalidation_epoch` in the mutation result.

Callers that cache authorization results must call
`EngineSecurityValidatePolicyCache` with their observed
`policy_generation` and `cache_invalidation_epoch` before using cached data.
If the epoch has advanced, the cache is refused with `SECURITY.POLICY.CACHE_STALE`
and must be rebuilt.

Prepared statements, query plans, metadata projections, and driver metadata
caches must also be invalidated when the security epoch advances.

## Transaction Behavior

Grant and revoke operations are catalog mutations subject to MGA transaction
finality:

| Event | Outcome |
|-------|---------|
| Grant issued, commit | Right becomes effective from the new security epoch |
| Grant issued, rollback | Right is not visible; prior state is restored |
| Revoke issued, commit | Right is removed from the new security epoch; dependent caches are invalidated |
| Revoke issued, rollback | Revocation is not visible; prior grant state is restored |
| Crash before commit | Recovery restores the pre-grant or pre-revoke state |
| Crash after commit | Recovery exposes the committed grant state |

## Diagnostic Codes

Relevant diagnostic codes for grant and privilege operations:

| Code | Meaning |
|------|---------|
| `SECURITY.ACCESS_DENIED` | General access denial |
| `SECURITY.PRIVILEGE.DEFAULT_DENY` | No matching grant; default-deny applied |
| `SECURITY.PRIVILEGE.GRANT_NOT_VISIBLE` | Grant referenced but not visible to this context |
| `SECURITY.GRANT_INVALID` | Grant is malformed or references unknown objects |
| `SECURITY.AUTHORIZATION.DENIED` | Authorization decision was "deny" |
| `SECURITY.RIGHT.UNKNOWN` | Right name not in `KnownRights()` |
| `SECURITY.POLICY.CACHE_STALE` | Cached authorization data is stale |
| `SECURITY.POLICY.STALE` | Policy epoch has advanced |
| `SECURITY.AUTHORIZATION.MEMBERSHIP_CYCLE` | Role or group membership graph contains a cycle |

## Invariants

- The default authorization decision is `"deny"`. A right must have an active
  matching grant record.
- Explicit denial wins over allow. A deny record short-circuits the allow check.
- Visibility is the intersection of MGA visibility and materialized security
  policy. A grant does not make an object visible if it is hidden by sandbox,
  metadata policy, or recovery state.
- The system is fail-closed. Missing, stale, or ambiguous authorization
  evidence refuses the operation.
- Grant mutations advance the security epoch and invalidate dependent caches,
  prepared statements, and metadata projections.

## Related Pages

- [standard_roles_and_groups.md](standard_roles_and_groups.md) — seeded roles
  and groups that receive grants
- [system_management_rights.md](system_management_rights.md) — OBS_* and
  system right identifiers
- [domain_and_column_security.md](domain_and_column_security.md) — column-level
  grants, domain rights, masking
- [Language Reference: Security and Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md)
- [security_model_overview.md](security_model_overview.md) — three-layer
  authorization model
