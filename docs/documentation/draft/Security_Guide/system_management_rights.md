# System Management Rights

## Purpose

This page maps the operator-visible administrative tasks — backup, database and
filespace management, agent control, configuration, management surface, metrics
and observability, policy administration, and cluster operations — to the right
identifiers that the engine checks when those tasks are requested.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/security_model.cpp` —
`KnownRights()`, `GroupAllows()`. Backup-specific right checks verified in
`src/engine/internal_api/backup_archive/backup_archive_api.cpp`.
Agent-specific rights verified in `src/core/agents/`.

## Definitions

**Right** — a string identifier that the engine tests via
`SecurityContextHasRight`. Only identifiers present in `KnownRights()` in
`security_model.cpp` are accepted; unknown rights are refused with
`SECURITY.RIGHT.UNKNOWN`.

**OBS_* rights** — the observability and system management authority taxonomy.
These rights are NOT SQL object privileges. They are system-level rights checked
inside engine subsystems (observability, agent runtime, cluster management,
configuration).

**SEC_* rights** — identity, membership, and grant administration rights that
authorize mutations to principals, roles, groups, and the grant table.

**BACKUP_* rights** — backup and restore rights checked in the backup archive
and storage management subsystems.

**MGA_* rights** — multi-generation archive transaction and recovery diagnostic
rights checked in the MGA subsystem.

## Right Taxonomy Overview

```
System management rights
├── Backup and recovery
│   ├── BACKUP_CREATE
│   ├── BACKUP_RESTORE
│   ├── BACKUP_CONTROL
│   └── BACKUP_INSPECT
├── Filespace management
│   └── FILESPACE_LIFECYCLE_CONTROL (storage-layer, not in KnownRights)
├── Agent management
│   ├── OBS_AGENT_STATE_READ
│   ├── OBS_AGENT_EVIDENCE_READ
│   ├── OBS_AGENT_CONTROL
│   ├── OBS_AGENT_POLICY_CONTROL
│   └── OBS_AGENT_OVERRIDE
├── Configuration management
│   ├── OBS_CONFIG_INSPECT
│   └── OBS_CONFIG_CONTROL
├── Management surface
│   ├── OBS_MANAGEMENT_INSPECT
│   └── OBS_MANAGEMENT_CONTROL
├── Metrics and observability
│   ├── OBS_METRICS_READ_SELF
│   ├── OBS_METRICS_READ_ALL
│   ├── OBS_METRICS_READ_FAMILY
│   ├── OBS_METRICS_READ_DATABASE
│   ├── OBS_METRICS_READ_NODE
│   ├── OBS_METRICS_READ_CLUSTER
│   ├── OBS_METRICS_EXPORT
│   ├── OBS_METRICS_EXPORT_READ
│   ├── OBS_METRICS_EXPORT_CONTROL
│   ├── OBS_METRICS_POLICY_INSPECT
│   ├── OBS_METRICS_RETENTION_CONTROL
│   └── OBS_METRICS_POLICY_CONTROL
├── Policy administration
│   └── OBS_POLICY_READ
├── Runtime and session
│   ├── OBS_RUNTIME_SELF
│   └── OBS_RUNTIME_ALL
├── Cluster (provider-gated)
│   ├── OBS_CLUSTER_HEALTH_INSPECT
│   └── OBS_CLUSTER_CONTROL
├── Data movement
│   └── OBS_DATA_MOVEMENT_INSPECT
├── Index diagnostics
│   └── OBS_INDEX_PROFILE_READ
├── Security administration
│   ├── SEC_IDENTITY_ADMIN
│   ├── SEC_MEMBERSHIP_ADMIN
│   ├── SEC_GRANT_ADMIN
│   ├── SEC_AUTH_METRICS_READ
│   └── SEC_REDACTION_POLICY_EDIT (source verified, not in KnownRights)
├── MGA diagnostics and control
│   ├── MGA_TRANSACTION_INSPECT
│   ├── MGA_RECOVERY_INSPECT
│   ├── MGA_CLEANUP_INSPECT
│   ├── MGA_CLEANUP_CONTROL
│   ├── MGA_HORIZON_INSPECT
│   ├── MGA_LINEAGE_INSPECT
│   ├── MGA_FORENSIC_INSPECT
│   └── MGA_METRICS_READ
└── Support and protected material
    ├── SUPPORT_EXPORT
    ├── PROTECTED_MATERIAL_RELEASE
    └── KEY_RELEASE_APPROVE
```

## Backup and Recovery Rights

Checked in: `backup_archive_api.cpp`, `storage_management_api.cpp`,
`security_model.cpp`.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Create a logical or physical backup | `BACKUP_CREATE` | Required to initiate any backup |
| Restore from a backup | `BACKUP_RESTORE` | Required to initiate any restore |
| Control backup scheduling and retention | `BACKUP_CONTROL` | Supersedes `BACKUP_CREATE` and `BACKUP_RESTORE` in admission checks |
| Inspect backup catalog and coverage | `BACKUP_INSPECT` | Read-only; does not permit create or restore |
| Any backup administrative operation | `SYS_BACKUP` | Superuser-equivalent backup right; not in `KnownRights()` — do not use in grants |

Source excerpt (admission logic from `backup_archive_api.cpp`):
```
// Create admission: BACKUP_CREATE || BACKUP_CONTROL || SYS_BACKUP
// Restore admission: BACKUP_RESTORE || BACKUP_CONTROL || SYS_BACKUP
```

`BACKUP_CONTROL` is the canonical operator backup right. A principal holding
`BACKUP_CONTROL` passes both create and restore admission checks. `BACKUP_CREATE`
and `BACKUP_RESTORE` are narrower alternatives for separation of duties.

Note: `FILESPACE_LIFECYCLE_CONTROL` is used in the storage management layer for
filespace create, drop, attach, detach, archive, and tier operations. It is
checked via `SecurityContextHasRight` in `storage_management_api.cpp` but does
not appear in `KnownRights()` at the security model layer. This right should be
treated as an internal storage-layer right; do not attempt to grant it through
the standard grant surface.

## Agent Control Rights

Checked in: `src/core/agents/` (agent action dispatch, approval, safety,
governance files).

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read agent state and health | `OBS_AGENT_STATE_READ` | Read-only; no control |
| Read agent evidence and traces | `OBS_AGENT_EVIDENCE_READ` | Audit and diagnostic read |
| Send control signals to agents | `OBS_AGENT_CONTROL` | Start, stop, pause, and resume admitted agents |
| Approve agent actions requiring approval | `OBS_AGENT_ACTION_APPROVE` | Verified in agents directory; not in `KnownRights()` — may be checked at the agent dispatch layer before the security model layer |
| Cancel agent actions | `OBS_AGENT_ACTION_CANCEL` | Verified in agents directory; not in `KnownRights()` |
| Override agent safety limits | `OBS_AGENT_OVERRIDE` | Use only for controlled operational override |
| Administer agent execution policy | `OBS_AGENT_POLICY_CONTROL` | Policy and governance for agent execution |
| Read agent recommendations | `OBS_AGENT_RECOMMENDATION_READ` | Verified in agents directory; not in `KnownRights()` |
| Read agent audit events | `OBS_AGENT_AUDIT_READ` | Verified in agents directory; not in `KnownRights()` |
| Internal agent authority | `OBS_AGENT_INTERNAL` | Verified in agents directory; not in `KnownRights()` — reserved for engine-internal use |

Rights verified in `KnownRights()` and available for standard grants:
`OBS_AGENT_STATE_READ`, `OBS_AGENT_EVIDENCE_READ`, `OBS_AGENT_CONTROL`,
`OBS_AGENT_POLICY_CONTROL`, `OBS_AGENT_OVERRIDE`.

Rights found in `src/core/agents/` sources but not in `KnownRights()`:
`OBS_AGENT_ACTION_APPROVE`, `OBS_AGENT_ACTION_CANCEL`,
`OBS_AGENT_RECOMMENDATION_READ`, `OBS_AGENT_AUDIT_READ`,
`OBS_AGENT_INTERNAL`. These may be checked at the agent dispatch layer
independently of the `KnownRights()` set; they should not be used in standard
`GRANT` statements.

## Configuration Management Rights

Checked in: storage management, observability, and configuration subsystems.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read current configuration | `OBS_CONFIG_INSPECT` | View configuration state; does not allow mutation |
| Apply configuration changes | `OBS_CONFIG_CONTROL` | Allows configuration mutations; requires audit trail |

`OBS_CONFIG_CONTROL` is a significant operational right. Configuration changes
can affect all sessions on the node. Assign it only to principals in `OPS` or
`ROLE_OPERATOR`.

## Management Surface Rights

The management surface covers session management, connection admission, and
operator command dispatch.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read management surface state | `OBS_MANAGEMENT_INSPECT` | View sessions, admission state, and surface metadata |
| Control sessions and management commands | `OBS_MANAGEMENT_CONTROL` | Terminate sessions, apply admission changes, dispatch operator commands |
| Administer manager admission policy | `MANAGER_ADMISSION_ADMIN` | Administer the manager admission rules |

Note: `OBS_SESSION_ALL` appears in `manager_control.cpp` as an alias that is
mapped to `OBS_RUNTIME_ALL`. Do not use `OBS_SESSION_ALL` in grant statements;
use `OBS_RUNTIME_ALL` instead.

## Metrics and Observability Rights

ScratchBird has a granular metrics hierarchy. Read rights are scoped by
breadth; export and policy rights control the metrics pipeline.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read own session metrics | `OBS_METRICS_READ_SELF` | Minimal; safe for application accounts |
| Read metrics for a specific family | `OBS_METRICS_READ_FAMILY` | Scoped to a metric family |
| Read metrics for a specific database | `OBS_METRICS_READ_DATABASE` | Scoped to one database |
| Read metrics for a specific node | `OBS_METRICS_READ_NODE` | Node-wide read |
| Read cluster-level metrics | `OBS_METRICS_READ_CLUSTER` | Requires cluster path |
| Read all metrics | `OBS_METRICS_READ_ALL` | Broad read; for `OPS`, `AUD`, `DBA` groups |
| Read authentication provider metrics | `SEC_AUTH_METRICS_READ` | Security-sensitive; scoped to auth subsystem |
| Export metrics to external sinks | `OBS_METRICS_EXPORT` | Allows metrics export pipeline |
| Read export configuration and output | `OBS_METRICS_EXPORT_READ` | Read-only export pipeline visibility |
| Control metrics export configuration | `OBS_METRICS_EXPORT_CONTROL` | Administer export destinations and format |
| Inspect metrics policy | `OBS_METRICS_POLICY_INSPECT` | Read metrics policy settings |
| Control metrics retention policy | `OBS_METRICS_RETENTION_CONTROL` | Set retention periods and purge schedules |
| Administer metrics policy | `OBS_METRICS_POLICY_CONTROL` | Full policy write for metrics pipeline |

## Policy Read Right

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read policy objects | `OBS_POLICY_READ` | Read durable policy catalog entries |

Additional policy lifecycle rights — `OBS_POLICY_APPLY`, `OBS_POLICY_APPROVE`,
`OBS_POLICY_DELETE`, `OBS_POLICY_EDIT_DRAFT`, `OBS_POLICY_ROLLBACK`,
`OBS_POLICY_SIMULATE`, `OBS_POLICY_VALIDATE`, `OBS_POLICY_READ_BODY` — are
verified in `src/core/agents/` sources but are not in `KnownRights()`. They
may be enforced at the agent dispatch layer. Do not use them in standard
`GRANT` statements.

## Runtime and Session Rights

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read own session runtime state | `OBS_RUNTIME_SELF` | Minimal; safe for all attached sessions |
| Read all session runtime state | `OBS_RUNTIME_ALL` | Broad; for `OPS`, `AUD`, `DBA` groups |

## Cluster Rights (Provider-Gated)

Cluster rights require a cluster authority path. Operations that check
`require_cluster_authority = true` in `EngineAuthorizeRequest` will fail if the
cluster path is absent, even if the right is granted.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read cluster health and status | `OBS_CLUSTER_HEALTH_INSPECT` | Available without full cluster control |
| Inspect cluster topology | `OBS_CLUSTER_TOPOLOGY_INSPECT` | Verified in agents directory; not in `KnownRights()` |
| Apply cluster control operations | `OBS_CLUSTER_CONTROL` | Full cluster command dispatch; requires cluster authority |

`OBS_CLUSTER_CONTROL` is gated on cluster authority availability. Granting it
to a principal on a standalone (non-cluster) node has no practical effect but
does not cause an error.

## Data Movement Inspection

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Inspect data movement operations | `OBS_DATA_MOVEMENT_INSPECT` | Read-only; covers replication, migration, and ETL pipelines |

## Index Profile Diagnostics

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read index profiles | `OBS_INDEX_PROFILE_READ` | Development diagnostic; advisory warning when granted to `DEV` group members |

## Security Administration Rights

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Create, alter, disable principals | `SEC_IDENTITY_ADMIN` | Identity lifecycle mutations |
| Manage role and group membership | `SEC_MEMBERSHIP_ADMIN` | Add or remove membership edges |
| Issue and revoke grants | `SEC_GRANT_ADMIN` | Required to call `EngineGrantRight` and `EngineRevokeRight` |
| Administer authentication providers | `AUTH_PROVIDER_ADMIN` | Add, alter, and remove provider registrations |
| Read authentication provider metrics | `SEC_AUTH_METRICS_READ` | Security-sensitive metrics |
| Administer UDR trust policy | `UDR_TRUST_ADMIN` | Control which UDR modules are trusted |
| Administer manager admission | `MANAGER_ADMISSION_ADMIN` | Control manager admission rules |

`SEC_GRANT_ADMIN` is the most consequential security administration right; a
principal holding it can expand or contract any principal's privileges.

## MGA Transaction and Recovery Rights

The MGA (multi-generation archive) subsystem has its own right taxonomy for
transaction inspection, recovery diagnostics, and cleanup control.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Inspect transaction history | `MGA_TRANSACTION_INSPECT` | Read-only transaction log inspection |
| Inspect recovery state | `MGA_RECOVERY_INSPECT` | Read recovery log and state |
| Inspect cleanup horizon | `MGA_CLEANUP_INSPECT` | Read GC horizon and cleanup metadata |
| Apply cleanup operations | `MGA_CLEANUP_CONTROL` | Trigger GC and cleanup within policy |
| Inspect history horizon | `MGA_HORIZON_INSPECT` | Read version horizon and snapshot state |
| Inspect transaction lineage | `MGA_LINEAGE_INSPECT` | Read causal transaction chain |
| Read forensic transaction data | `MGA_FORENSIC_INSPECT` | Restricted forensic read for audit investigations |
| Read MGA metrics | `MGA_METRICS_READ` | MGA subsystem performance metrics |

## Support and Protected Material Rights

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Generate support bundles | `SUPPORT_EXPORT` | Support bundle includes redacted diagnostics only |
| Release protected material for a purpose | `PROTECTED_MATERIAL_RELEASE` | Requires matching release policy to also be satisfied |
| Approve encryption key release | `KEY_RELEASE_APPROVE` | Key management approval step |
| Read audit records | `AUDIT_READ` | General audit record read |
| Administer audit policy | `AUDIT_ADMIN` | Audit policy lifecycle |

## Consolidated Operator Task Reference

| Operator Task | Minimum Required Right(s) |
|---------------|--------------------------|
| Create backup | `BACKUP_CREATE` or `BACKUP_CONTROL` |
| Restore backup | `BACKUP_RESTORE` or `BACKUP_CONTROL` |
| Inspect backup catalog | `BACKUP_INSPECT` |
| Full backup administration | `BACKUP_CONTROL` |
| Read agent state | `OBS_AGENT_STATE_READ` |
| Control agents | `OBS_AGENT_CONTROL` |
| Override agent safety | `OBS_AGENT_OVERRIDE` |
| Administer agent policy | `OBS_AGENT_POLICY_CONTROL` |
| Read configuration | `OBS_CONFIG_INSPECT` |
| Apply configuration | `OBS_CONFIG_CONTROL` |
| Read management surface | `OBS_MANAGEMENT_INSPECT` |
| Control management surface | `OBS_MANAGEMENT_CONTROL` |
| Read own metrics | `OBS_METRICS_READ_SELF` |
| Read all metrics | `OBS_METRICS_READ_ALL` |
| Export metrics | `OBS_METRICS_EXPORT` |
| Read cluster health | `OBS_CLUSTER_HEALTH_INSPECT` |
| Apply cluster control | `OBS_CLUSTER_CONTROL` (+ cluster authority) |
| Inspect data movement | `OBS_DATA_MOVEMENT_INSPECT` |
| Manage identities | `SEC_IDENTITY_ADMIN` |
| Manage grants | `SEC_GRANT_ADMIN` |
| Manage auth providers | `AUTH_PROVIDER_ADMIN` |
| Inspect MGA transactions | `MGA_TRANSACTION_INSPECT` |
| Apply MGA cleanup | `MGA_CLEANUP_CONTROL` |
| Generate support bundle | `SUPPORT_EXPORT` |
| Approve key release | `KEY_RELEASE_APPROVE` |

## Rights Not in KnownRights()

The following right identifiers appear in source code outside
`security_model.cpp` but are absent from `KnownRights()`:

| Identifier | Location | Status |
|-----------|----------|--------|
| `FILESPACE_LIFECYCLE_CONTROL` | `storage_management_api.cpp` | Storage-layer check; not grantable via standard GRANT |
| `SYS_BACKUP` | `backup_archive_api.cpp` | Backup superuser; internal use |
| `OBS_AGENT_ACTION_APPROVE` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_ACTION_CANCEL` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_RECOMMENDATION_READ` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_AUDIT_READ` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_INTERNAL` | `src/core/agents/` | Internal engine use |
| `OBS_CLUSTER_TOPOLOGY_INSPECT` | `src/core/agents/` | Agent dispatch layer |
| `OBS_POLICY_APPLY`, `OBS_POLICY_APPROVE`, `OBS_POLICY_DELETE`, `OBS_POLICY_EDIT_DRAFT`, `OBS_POLICY_ROLLBACK`, `OBS_POLICY_SIMULATE`, `OBS_POLICY_VALIDATE`, `OBS_POLICY_READ_BODY` | `src/core/agents/` | Agent dispatch layer |
| `OBS_SUPPORT_BUNDLE_READ` | `src/core/agents/` | Agent dispatch layer |
| `SEC_REDACTION_POLICY_EDIT` | `auth_provider_plugin_api.cpp` | Plugin API internal |
| `SEC_EXPORT_POLICY_APPROVE` | `auth_provider_plugin_api.cpp` | Plugin API internal |

Do not attempt to grant these identifiers through the standard `GRANT`
statement; the `IsKnownSecurityRight` check in `grant_api.cpp` will refuse the
operation for any right not in `KnownRights()`.

## Invariants

- Only rights in `KnownRights()` are accepted by `IsKnownSecurityRight`. Grants
  referencing unknown rights are refused with `SECURITY.RIGHT.UNKNOWN`.
- The system is fail-closed. The default authorization decision is `"deny"`.
  A right must have an active matching grant record; absence of a grant is not
  permission.
- Cluster rights require cluster authority path availability even when the
  right is granted.
- `OBS_SESSION_ALL` is an alias for `OBS_RUNTIME_ALL` in the manager control
  layer. Use `OBS_RUNTIME_ALL` in grant statements.

## Related Pages

- [standard_roles_and_groups.md](standard_roles_and_groups.md) — which groups
  and roles convey which rights
- [grants_and_privileges.md](grants_and_privileges.md) — how rights are
  granted and evaluated
- [security_model_overview.md](security_model_overview.md) — authorization
  decision model
- [Language Reference: Security and Sandboxing](../Language_Reference/core_paradigms/security_and_sandboxing.md)
