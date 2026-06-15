# Standard Roles and Groups

## Purpose

This page documents the built-in roles and groups that ScratchBird seeds during
initial security bootstrap. Understanding these objects — what rights each one
conveys, how they are seeded, and how they can be extended — is necessary before
assigning principals to any of them.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source files: `src/engine/internal_api/security/standard_bundle_api.hpp`,
`standard_bundle_api.cpp`, `security_model.cpp`.

## Definitions

**Standard bundle** — the set of roles, groups, and policies seeded by
`EngineSeedStandardSecurityBundles`. This function is callable only by a
principal holding `SEC_GRANT_ADMIN` or by a caller carrying the internal
`security.bootstrap` trace tag.

**Seeded group** — a group whose name and UUID are materialized into the
catalog during bootstrap. Seeded groups are functional conventions, not
immutable built-ins. An operator with `SEC_GRANT_ADMIN` can add or remove
rights from them.

**Seeded role** — a role seeded during bootstrap that conveys a curated set of
rights. Role rights are materialized from the durable grant table; a role
conveys only what grants are recorded for it.

## Seeding and Bootstrap Model

Source: `standard_bundle_api.cpp` — `EngineSeedStandardSecurityBundles`.

`EngineSeedStandardSecurityBundles` is called during database creation and,
when the bootstrap tag is present, during recovery. It seeds:

| Count | Kind |
|-------|------|
| 12 | Standard groups |
| 5 | Standard roles |
| 10 | Standard policies |

The seed function uses `PersistApiBehaviorRecord` with `allow_existing = true`,
so re-running it on an already-bootstrapped database is idempotent.

The `EngineSeedStandardSecurityBundlesResult` struct carries
`groups_seeded`, `roles_seeded`, and `policies_seeded` counters that reflect
the number of records written or confirmed during the call.

## Standard Groups

The 12 seeded groups are identified by exact name. Each group maps to a set of
rights through the `GroupAllows` function in `security_model.cpp`.

### GROUP: `PUBLIC`

The `PUBLIC` pseudo-group is not assigned rights by `GroupAllows`. It is a
resolver convention for privileges that should apply to every attached session.
Granting a right `TO PUBLIC` creates a grant whose subject kind resolves to
every active principal. Use `PUBLIC` grants only for rights that are genuinely
intended for all callers.

### GROUP: `APP`

For application-tier principals — web servers, API gateways, and service
accounts that connect to the database on behalf of end users.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `CONNECT` | Allow database attachment |
| `OBS_RUNTIME_SELF` | Read own session metrics and runtime state |
| `EVENT_SUBSCRIBE` | Subscribe to event streams |
| `EVENT_PUBLISH` | Publish to event streams |
| `EVENT_DELIVERY_READ` | Read event delivery metadata |
| `EVENT_DELIVERY_ACK` | Acknowledge event delivery |

### GROUP: `DEV`

For developer accounts that need schema visibility and basic self-diagnostics.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `OBS_RUNTIME_SELF` | Read own session runtime state |
| `OBS_METRICS_READ_SELF` | Read own session metrics |
| `VISIBLE` | Assert that named objects are visible in name resolution |
| `DISCOVER` | Assert object discovery within the sandbox |
| `OBS_INDEX_PROFILE_READ` | Read index profiles (development diagnostic) |

Note: Granting `OBS_INDEX_PROFILE_READ` to a `DEV`-group principal triggers a
diagnostic advisory in `grant_api.cpp`. This right is intended as a development
diagnostic and may have performance implications.

### GROUP: `ANL`

For analytics and business intelligence principals that need read access.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `CONNECT` | Allow database attachment |
| `SELECT` | Read table and view rows |
| `VISIBLE` | Named object visibility |
| `DISCOVER` | Object discovery within the sandbox |

`ANL` principals still require explicit `GRANT SELECT ON TABLE ...` grants for
specific objects. Group membership does not bypass object-level grants.

### GROUP: `ETL` and GROUP: `SCH`

For data pipeline and scheduled-batch principals that execute routines and read
metrics.

Rights conveyed (both groups):

| Right | Purpose |
|-------|---------|
| `CONNECT` | Allow database attachment |
| `EXECUTE` | Invoke functions and procedures |
| `OBS_METRICS_READ_FAMILY` | Read metrics for a specific family |

`SCH` is intended for schema-maintenance automation. Both groups require
explicit object grants for the tables, views, and routines they need.

### GROUP: `DBA`

For database administrator principals. `DBA` conveys broad data access and
diagnostic rights but does not include security administration rights
(`SEC_GRANT_ADMIN`, `SEC_IDENTITY_ADMIN`, `SEC_MEMBERSHIP_ADMIN`).

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `VISIBLE`, `DISCOVER`, `LIST_CHILD` | Full object visibility and traversal |
| `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `EXECUTE` | Full data manipulation |
| `CREATE`, `ALTER`, `DROP` | Schema lifecycle operations |
| `DOMAIN_USE`, `DOMAIN_CAST`, `DOMAIN_METHOD`, `DOMAIN_POLICY_ADMIN`, `DOMAIN_UNMASK` | Domain operations and masking override |
| `UDR_INSPECT`, `UDR_INVOKE` | Inspect and invoke user-defined routines |
| `BACKUP_CREATE`, `BACKUP_RESTORE`, `BACKUP_INSPECT` | Backup creation, restore, and inspection (not full `BACKUP_CONTROL`) |
| `EVENT_ADMIN`, `EVENT_CREATE`, `EVENT_ALTER`, `EVENT_DROP`, `EVENT_SUBSCRIBE`, `EVENT_PUBLISH`, `EVENT_DELIVERY_READ`, `EVENT_DELIVERY_ACK` | Full event lifecycle |
| `OBS_RUNTIME_ALL`, `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE` | Full observability read |
| `OBS_INDEX_PROFILE_READ` | Index profile diagnostics |
| `OBS_CONFIG_INSPECT` | Read configuration state |
| `OBS_CLUSTER_HEALTH_INSPECT` | Read cluster health |
| `OBS_DATA_MOVEMENT_INSPECT` | Inspect data movement operations |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_CLEANUP_INSPECT`, `MGA_CLEANUP_CONTROL`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_METRICS_READ` | MGA diagnostics and cleanup control |

### GROUP: `AUD`

For security audit principals who need to review security events, metrics, and
agent evidence. Does not include write or control rights.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `AUDIT_READ` | Read audit records |
| `OBS_RUNTIME_ALL` | Read all session runtime state |
| `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE`, `OBS_METRICS_READ_CLUSTER`, `OBS_METRICS_EXPORT_READ` | Full metrics read |
| `SEC_AUTH_METRICS_READ` | Read authentication provider metrics |
| `OBS_POLICY_READ` | Read policy objects |
| `OBS_CONFIG_INSPECT` | Read configuration state |
| `OBS_CLUSTER_HEALTH_INSPECT` | Read cluster health |
| `SUPPORT_EXPORT` | Generate support bundles |
| `OBS_AGENT_STATE_READ`, `OBS_AGENT_EVIDENCE_READ` | Read agent state and evidence |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_FORENSIC_INSPECT`, `MGA_METRICS_READ` | MGA audit and forensic read |

### GROUP: `SUP`

For ScratchBird support personnel or site reliability engineers with
read-only diagnostic access. A subset of `AUD`.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `OBS_RUNTIME_ALL` | Read all session runtime state |
| `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE`, `OBS_METRICS_READ_CLUSTER`, `OBS_METRICS_EXPORT_READ` | Full metrics read |
| `OBS_MANAGEMENT_INSPECT` | Read management surface |
| `OBS_CLUSTER_HEALTH_INSPECT` | Read cluster health |
| `SUPPORT_EXPORT` | Generate support bundles |
| `OBS_AGENT_STATE_READ` | Read agent state |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_METRICS_READ` | MGA diagnostics read |

`SUP` has fewer rights than `OPS`: it cannot control anything, cannot alter
configuration, and cannot manage backup operations.

### GROUP: `OPS`

For operations principals who manage the running database. `OPS` conveys broad
operational control including backup, cluster, configuration, and agent
management.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `OBS_RUNTIME_ALL` | Read all session runtime state |
| `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE`, `OBS_METRICS_READ_CLUSTER`, `OBS_METRICS_EXPORT_READ` | Full metrics read |
| `OBS_MANAGEMENT_INSPECT`, `OBS_MANAGEMENT_CONTROL` | Read and control management surface |
| `OBS_CONFIG_INSPECT`, `OBS_CONFIG_CONTROL` | Read and control configuration |
| `OBS_CLUSTER_HEALTH_INSPECT`, `OBS_CLUSTER_CONTROL` | Read and control cluster |
| `OBS_DATA_MOVEMENT_INSPECT` | Inspect data movement |
| `OBS_METRICS_EXPORT`, `OBS_METRICS_EXPORT_CONTROL` | Export metrics and control export settings |
| `SUPPORT_EXPORT` | Generate support bundles |
| `BACKUP_CREATE`, `BACKUP_RESTORE`, `BACKUP_CONTROL`, `BACKUP_INSPECT` | Full backup lifecycle |
| `OBS_AGENT_STATE_READ`, `OBS_AGENT_EVIDENCE_READ`, `OBS_AGENT_CONTROL`, `OBS_AGENT_POLICY_CONTROL`, `OBS_AGENT_OVERRIDE` | Full agent management |
| `MANAGER_ADMISSION_ADMIN` | Administer manager admission policy |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_CLEANUP_INSPECT`, `MGA_CLEANUP_CONTROL`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_METRICS_READ` | MGA operations and cleanup control |

### GROUP: `SEC`

For security administration principals. `SEC` conveys identity, grant, and
policy management but not broad data access.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `SEC_IDENTITY_ADMIN` | Create, alter, and drop principals |
| `SEC_MEMBERSHIP_ADMIN` | Manage role and group membership |
| `SEC_GRANT_ADMIN` | Issue and revoke grants |
| `POLICY_ADMIN` | Administer security policies |
| `AUDIT_READ` | Read audit records |
| `AUDIT_ADMIN` | Administer audit policy |
| `AUTH_PROVIDER_ADMIN` | Administer authentication providers |
| `UDR_TRUST_ADMIN` | Administer UDR trust policy |
| `MANAGER_ADMISSION_ADMIN` | Administer manager admission policy |
| `UDR_MANAGE`, `UDR_INSPECT`, `UDR_INVOKE` | UDR lifecycle |
| `BACKUP_CREATE`, `BACKUP_RESTORE`, `BACKUP_CONTROL`, `BACKUP_INSPECT` | Full backup lifecycle |
| `PROTECTED_MATERIAL_RELEASE`, `KEY_RELEASE_APPROVE` | Protected material release and key approval |
| `EVENT_ADMIN`, `EVENT_CREATE`, `EVENT_ALTER`, `EVENT_DROP`, `EVENT_SUBSCRIBE`, `EVENT_PUBLISH`, `EVENT_DELIVERY_READ`, `EVENT_DELIVERY_ACK` | Full event lifecycle |
| `SUPPORT_EXPORT` | Generate support bundles |
| `OBS_CONFIG_INSPECT`, `OBS_MANAGEMENT_INSPECT` | Read configuration and management |
| `OBS_METRICS_POLICY_INSPECT`, `OBS_METRICS_POLICY_CONTROL` | Metrics policy administration |
| `SEC_AUTH_METRICS_READ`, `OBS_POLICY_READ` | Security and policy read |
| `OBS_AGENT_EVIDENCE_READ`, `OBS_AGENT_POLICY_CONTROL`, `OBS_AGENT_OVERRIDE` | Agent security evidence and policy |
| `MGA_LINEAGE_INSPECT`, `MGA_FORENSIC_INSPECT`, `MGA_METRICS_READ` | MGA forensic read |

### GROUP: `ROOT`

`ROOT` is the all-rights group. A principal in `ROOT` is allowed any known
right. This is implemented in `GroupAllows` as a blanket `return true`.

Membership in `ROOT` must be treated as equivalent to full database
superuser access. It bypasses all group-level and role-level right checks.
Assign principals to `ROOT` only through deliberate, audited, and documented
processes. The engine does not prevent membership assignment from principals
holding `SEC_MEMBERSHIP_ADMIN`, so organizational controls are the only
guard.

## Standard Roles

The 5 seeded roles are seeded with display names but their privilege grants are
materialized from the durable grant table. The rights shown here reflect the
trace-tag fallback logic in `security_model.cpp`
(`SecurityContextHasRight`) and should be confirmed against the durable
grant records in a running database.

### ROLE: `ROLE_APP_RUNTIME`

Intended for application service accounts. No rights are conveyed by the role
record alone in the seeded state; explicit grants must be issued to this role
for the object classes the application needs.

### ROLE: `ROLE_DBA`

Intended as a broad DBA role. No rights are conveyed by the role record alone
in the seeded state. Operators typically grant this role the rights that match
the `DBA` group or a narrower subset.

### ROLE: `ROLE_SECURITY_ADMIN`

The trace-tag fallback in `security_model.cpp` maps this role to
`SEC_IDENTITY_ADMIN`, `SEC_MEMBERSHIP_ADMIN`, `SEC_GRANT_ADMIN`, and
`POLICY_ADMIN`. In a fully bootstrapped database, corresponding durable grants
should be present. Do not activate this role for principals that do not require
identity and grant administration authority.

### ROLE: `ROLE_AUDIT_READER`

The trace-tag fallback maps this role to `AUDIT_READ`, `MGA_LINEAGE_INSPECT`,
and `MGA_FORENSIC_INSPECT`. In a fully bootstrapped database, corresponding
durable grants should be present.

### ROLE: `ROLE_OPERATOR`

The trace-tag fallback maps this role to `OBS_MANAGEMENT_CONTROL`,
`OBS_CONFIG_CONTROL`, `OBS_CLUSTER_CONTROL`, and `MGA_CLEANUP_CONTROL`. In a
fully bootstrapped database, corresponding durable grants should be present.

## Standard Policies

The 10 seeded policy names are:

| Policy Name | Purpose (from source) |
|-------------|----------------------|
| `revoke_all_default` | Revoke-all default behavior policy |
| `bootstrap_handoff` | Bootstrap-to-runtime authority handoff |
| `external_group_sync` | External provider group synchronization |
| `stale_security_context` | Stale-context handling policy |
| `observability_control_baseline` | Baseline observability control policy |
| `audit_evidence_required` | Audit-before-success policy |
| `protected_material_purpose_bound` | Purpose-bound protected material release policy |
| `udr_trust` | UDR trust policy |
| `manager_admission` | Manager admission policy |
| `cluster_security_fail_closed` | Cluster security fail-closed policy |

Policy content is established at creation time. Policies seeded here exist as
catalog objects that other configuration can reference by name.

## External Group Synchronization

Source: `external_group_api.cpp` — `EngineSyncExternalGroups`,
`EngineExplainMembership`.

A group with a non-empty `external_authority_ref` field in its
`EngineSecurityGroupRecord` is linked to an identity provider group. The
`external_group_sync` policy governs how provider-supplied group claims are
materialized into the engine's authorization context.

Synchronization requires:

1. A known authentication provider family that supports `supports_group_query`
   or `supports_authz_claims`. Verified families with group query support:
   `ldap_ad` (full transitive group expansion and membership path explain),
   `remote_security_database`, `cluster_security`.
2. An `external_group` name and an `internal_group_uuid` passed via
   `EngineSyncExternalGroupsRequest`.
3. The `cluster_security` provider requires `cluster_authority_available` to
   be true; if the cluster path is absent, the call fails with
   `PROCESS.CLUSTER_PATH_ABSENT`.

Materialized external group membership flows into the
`EngineMaterializedAuthorizationContext` via `effective_subjects`, which means
rights granted to the internal group become available to the principal
whose claim matched the external group name.

`EngineExplainMembership` returns a human-readable explanation of the
membership path. This is supported only for providers with
`supports_membership_path_explain` (currently verified: `ldap_ad`) or when
`synchronized_graph_evidence` is set to true.

## Extending the Standard Bundles Safely

The standard groups and roles are starting points, not mandates. Safe
extension patterns:

1. **Create narrower groups** for specific application tiers rather than adding
   principals directly to `OPS`, `SEC`, or `ROOT`. This limits blast radius if
   a principal is compromised.

2. **Grant minimum necessary rights** to custom roles. The `KnownRights()` set
   in `security_model.cpp` is the authoritative enumeration; only rights in
   that set are accepted by `IsKnownSecurityRight`.

3. **Do not remove the standard policies** (the 10 seeded policy names).
   Removing `audit_evidence_required`, `protected_material_purpose_bound`, or
   `cluster_security_fail_closed` disables important safety behaviors.

4. **Separate DBA and SEC membership**. Principals in `SEC` can alter grants
   and identities but have limited data access. Principals in `DBA` have broad
   data access but cannot alter security policy. Keeping them separate
   implements the least-privilege and separation-of-duties goals.

5. **Audit `ROOT` membership** regularly. `ROOT` membership bypasses all
   group-level and role-level right checks and must be treated as a superuser
   assignment.

## Invariants

- Visibility is the intersection of MGA visibility and materialized security
  policy. A seeded group conveys no rights unless the object-level grants also
  admit the operation.
- The system is fail-closed. A principal in a named group still fails
  authorization if the object-level grant is absent or an explicit deny is
  present.
- Seeded group and role names are display identifiers. Durable identity is
  always the UUID recorded at seeding time.

## Related Pages

- [security_model_overview.md](security_model_overview.md) — three-layer model
  and epoch machinery
- [system_management_rights.md](system_management_rights.md) — OBS_* and
  authority right taxonomy
- [grants_and_privileges.md](grants_and_privileges.md) — GRANT/REVOKE model
- [Language Reference: Security and Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md)
- [Language Reference: Security and Sandboxing](../Language_Reference/core_paradigms/security_and_sandboxing.md)
