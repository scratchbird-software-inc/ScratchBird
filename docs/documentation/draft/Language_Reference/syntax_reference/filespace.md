# Filespace Lifecycle

This page is part of the SBsql Language Reference Manual. It documents filespace creation, roles, placement policy, allocation controls, growth/shrink handling, health and capacity reporting, filespace agents, security policy binding, and refusal behavior.

Generation task: `syntax_reference_filespace_lifecycle`

Related pages: [Database Lifecycle](database.md), [Table Lifecycle](table.md), [Index Lifecycle](index.md), [Security And Privileges](security_and_privilege_statements.md), [Management And Operations](management_and_operations.md), [Refusal Vectors](refusal_vectors.md), and [Filespace Lifecycle EBNF](ebnf/filespace_lifecycle_statement.md).

## Purpose

A filespace is a named storage-placement and allocation policy descriptor. It tells the engine where and how a class of database pages may be allocated, grown, synchronized, checked, fenced, and reported.

A filespace is not a parser-controlled file handle. SQL text can request a filespace descriptor or policy change; storage code owns allocation, page writes, durability, integrity checks, and recovery behavior.

## Filespace Identity

| Concept | Meaning | Authority |
| --- | --- | --- |
| Filespace UUID | Durable identity of the filespace descriptor. | Catalog and engine storage metadata. |
| Filespace name | Resolver name used by DDL and policy binding. | Name registry and security policy. |
| Storage reference | Policy-controlled storage location or placement URI. | Engine storage policy. |
| Filespace role | Intended use such as data, index, overflow, temporary, catalog, or audit storage. | Filespace descriptor and placement policy. |
| Allocation policy | Growth, reserve, quota, extent, and threshold behavior. | Storage manager and filespace policy. |
| Health state | Online, read-only, low-reserve, recovery-required, or retiring status. | Storage checks and operational evidence. |

## Filespace Roles

| Role | Typical Contents | Notes |
| --- | --- | --- |
| `catalog` | Catalog and name-registry pages. | Must be available for normal open and recovery. |
| `data` | Table row pages and row-version storage. | Default role for ordinary table data. |
| `index` | Index pages, index delta pages, and index metadata. | May use a separate placement policy from table data. |
| `overflow` | Large values, document payloads, and overflow chains. | Also used for large logical values when table rows reference external page chains. |
| `temporary` | Session or statement temporary storage. | Must be clearly separated from durable user data policy. |
| `audit` | Audit, diagnostic, or lifecycle evidence where configured. | Protected and redacted according to policy. |
| `archive` | Backup/archive staging or manifest storage where admitted. | Does not replace backup authorization. |
| `default` | Fallback placement for objects without a more specific role. | Should be explicit in production configuration. |

Roles are placement descriptors. They do not grant object privileges by themselves.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE FILESPACE` | Creates a filespace descriptor, role, storage reference, allocation policy, integrity policy, and readiness state. |
| Create agent | `CREATE FILESPACE AGENT` | Registers a policy-controlled agent hook for growth, shrink readiness, or storage lifecycle assistance. |
| Alter | `ALTER FILESPACE` | Changes admitted quota, growth, threshold, read-only, online/offline, maintenance, role, or integrity settings. |
| Attach policy | `ATTACH POLICY ... TO FILESPACE ... ROLE ...` | Binds a placement/security policy to a filespace or role. |
| Rename | `RENAME FILESPACE ... TO ...` | Changes resolver name only; allocation identity and storage references remain engine-owned. |
| Comment | `COMMENT ON FILESPACE ... IS ...` | Stores authorized operator/support metadata. |
| Show list | `SHOW FILESPACES` | Lists authorized filespace descriptors. |
| Show extended | `SHOW FILESPACE EXTENDED` | Returns detailed filespace metadata visible to the caller. |
| Show health | `SHOW FILESPACE <name> HEALTH` | Reports health, fences, low-reserve state, read-only state, and diagnostics. |
| Show capacity | `SHOW FILESPACE <name> CAPACITY` | Reports quota, used/reserved/free values, growth limits, and high-water evidence. |
| Show shrink readiness | `SHOW FILESPACE <name> SHRINK READINESS` | Reports whether pages can be relocated/reclaimed safely. |
| Recreate | `RECREATE FILESPACE` | Replaces descriptor metadata only where no unsafe data movement, dependency, or recovery conflict exists. |
| Drop | `DROP FILESPACE ... [RESTRICT | CASCADE]` | Retires a filespace only after storage dependencies, recovery fences, and policy checks pass. |

## Syntax

```ebnf
filespace_lifecycle_statement ::=
      create_filespace_statement
    | create_filespace_agent_statement
    | alter_filespace_statement
    | attach_filespace_policy_statement
    | show_filespace_statement
    | describe_filespace_statement
    | rename_filespace_statement
    | comment_filespace_statement
    | recreate_filespace_statement
    | drop_filespace_statement ;
```

```ebnf
create_filespace_statement ::=
    CREATE FILESPACE filespace_name filespace_create_options? ;

filespace_create_options ::=
    filespace_create_option+ ;

filespace_create_option ::=
      LOCATION storage_ref
    | ROLE filespace_role
    | MAX SIZE size_literal
    | INITIAL SIZE size_literal
    | GROW BY size_literal
    | RESERVE size_literal
    | LOW RESERVE THRESHOLD size_literal
    | PAGE SIZE integer_literal
    | SYNC sync_policy_name
    | CHECKSUM checksum_policy_name
    | READ ONLY
    | ONLINE
    | POLICY policy_name ;
```

```ebnf
create_filespace_agent_statement ::=
    CREATE FILESPACE AGENT filespace_agent_name
    FOR FILESPACE filespace_name?
    filespace_agent_options? ;

filespace_agent_options ::=
    WITH filespace_agent_option (","? filespace_agent_option)* ;

filespace_agent_option ::=
      TYPE filespace_agent_type
    | POLICY policy_name
    | MAX ACTIONS integer_literal
    | TIMEOUT duration_literal ;
```

```ebnf
alter_filespace_statement ::=
    ALTER FILESPACE filespace_name alter_filespace_action+ ;

alter_filespace_action ::=
      SET LOCATION storage_ref
    | SET ROLE filespace_role
    | SET MAX SIZE size_literal
    | SET GROW BY size_literal
    | SET RESERVE size_literal
    | SET LOW RESERVE THRESHOLD size_literal
    | SET SYNC sync_policy_name
    | SET CHECKSUM checksum_policy_name
    | SET READ ONLY
    | SET READ WRITE
    | SET ONLINE
    | SET OFFLINE
    | SET MAINTENANCE
    | CLEAR MAINTENANCE
    | REQUEST GROWTH filespace_growth_request
    | NOTIFY SHRINK READINESS filespace_shrink_descriptor ;
```

```ebnf
attach_filespace_policy_statement ::=
    ATTACH POLICY policy_name TO FILESPACE filespace_name
    (ROLE role_name)? ;

show_filespace_statement ::=
      SHOW FILESPACES
    | SHOW FILESPACE filespace_name
    | SHOW FILESPACE EXTENDED
    | SHOW FILESPACE filespace_name HEALTH
    | SHOW FILESPACE filespace_name CAPACITY
    | SHOW FILESPACE filespace_name SHRINK READINESS ;

drop_filespace_statement ::=
    DROP FILESPACE filespace_name drop_filespace_options? ;

drop_filespace_options ::=
      RESTRICT
    | CASCADE
    | PRESERVE STORAGE
    | DESTROY STORAGE ;
```

SBsql is context-sensitive. Filespace words are command words in filespace statements and do not need to be globally reserved in unrelated expression positions.

## Create Filespace

`CREATE FILESPACE` creates a descriptor. It does not by itself allocate all future pages, move existing pages, or bypass database storage policy.

Example:

```sql
create filespace primary_data
  location 'policy://storage/primary-data'
  role data
  max size 500 gb
  reserve 25 gb
  low reserve threshold 10 gb
  sync durable_default
  checksum page_crc;
```

Creation records:

- filespace UUID and name;
- storage reference;
- role;
- maximum and initial size policy;
- growth increment and low-reserve thresholds;
- sync and integrity policies;
- online/read-only/maintenance state;
- policy bindings;
- dependency and compatibility metadata.

Invalid storage references, unsupported page sizes, incompatible sync policies, or unauthorized placement policies are refused before the descriptor becomes usable.

## Alter Filespace

`ALTER FILESPACE` changes descriptor policy. It does not move or rewrite allocated pages unless the action explicitly admits a storage operation.

Examples:

```sql
alter filespace primary_data set max size 750 gb;
alter filespace primary_data set read only;
alter filespace primary_data set read write;
alter filespace primary_data set low reserve threshold 15 gb;
```

| Action Family | Contract |
| --- | --- |
| Quota and growth | May update future allocation limits. Must refuse values below currently allocated or reserved space unless a shrink plan exists. |
| Online/offline | Controls admission. Offline must refuse if active dependencies cannot be drained safely. |
| Read-only/read-write | Read-only fences new writes while preserving reads and recovery access where policy admits. |
| Maintenance | Allows controlled verify, relocation, or repair actions. Ordinary allocation may be fenced. |
| Location change | Updates future placement only unless an explicit relocation plan is admitted. |
| Sync/checksum policy | Applies according to compatibility rules. Unsafe weakening requires explicit policy authority. |

## Filespace Agents

A filespace agent is a policy-controlled hook that assists storage lifecycle decisions. It is not storage authority by itself.

```sql
create filespace agent primary_growth_agent
for filespace primary_data
with
  type growth,
  policy primary_growth_policy,
  timeout 30 seconds;
```

Agent responsibilities can include:

- requesting additional capacity;
- reporting low-reserve conditions;
- preparing shrink-readiness evidence;
- coordinating page relocation requests;
- producing diagnostics for support bundles.

The engine still owns allocation, page movement, recovery, and integrity. Agent failure must return a diagnostic or quarantine state; it must not silently allocate pages or mark storage healthy.

## Growth, Capacity, And Shrink Readiness

Filespace capacity is policy and evidence, not just a number.

| Field | Meaning |
| --- | --- |
| Maximum size | Upper allocation limit under current policy. |
| Initial size | Initial allocation target where preallocation is admitted. |
| Used bytes/pages | Pages currently allocated to objects. |
| Reserved bytes/pages | Space reserved for growth, recovery, or safety margin. |
| Free bytes/pages | Space available under current policy. |
| Low reserve threshold | Level at which low-reserve diagnostics or growth requests begin. |
| Growth increment | Unit requested during auto-growth or agent-assisted growth. |
| Shrink readiness | Whether pages can be relocated or reclaimed without breaking references, snapshots, or recovery. |

Examples:

```sql
show filespaces;
show filespace primary_data capacity;
show filespace primary_data health;
show filespace primary_data shrink readiness;
```

Shrink readiness must consider active transactions, snapshots, page references, overflow chains, indexes, temporary allocations, recovery fences, and object dependencies. A filespace can be low on capacity but still not shrink-ready.

## Health And Fencing

| Health State | Meaning | Required Behavior |
| --- | --- | --- |
| `online` | Filespace can accept admitted allocations. | Allocate according to policy. |
| `read_only` | Reads are allowed, new writes are fenced. | Refuse writes with a storage-policy diagnostic. |
| `low_reserve` | Reserve threshold has been crossed. | Emit diagnostics and growth request evidence where admitted. |
| `growth_required` | Future allocation requires growth. | Request growth or refuse writes fail-closed. |
| `maintenance` | Controlled storage operations are in progress. | Fence ordinary allocation according to policy. |
| `recovery_required` | Integrity or lifecycle evidence requires recovery. | Fence unsafe access and expose authorized diagnostics. |
| `retiring` | Filespace is being drained or dropped. | Refuse new dependencies and preserve existing references until safe. |
| `offline` | Filespace is not available for ordinary access. | Refuse dependent operations unless recovery/diagnostic policy admits them. |

## Object Placement

Objects can bind to filespaces directly or through database defaults and placement policy.

| Object Class | Placement Behavior |
| --- | --- |
| Tables | Use table-specific filespace, schema/database default, or placement policy. |
| Indexes | May use table placement or an index-specific filespace. |
| Overflow values | Use overflow role or table/filespace policy. |
| Temporary objects | Use temporary role and session policy. |
| Catalog objects | Use catalog role and bootstrap policy. |
| Audit/diagnostic data | Use audit role where configured and redacted. |

Changing a default filespace affects future bindings. Existing object descriptors keep their stored placement until explicitly altered or moved through an admitted storage operation.

## Security And Policy Binding

Filespace operations require storage and catalog authority.

```sql
attach policy placement_policy to filespace primary_data role app_writer;
```

| Concern | Contract |
| --- | --- |
| Visibility | Users see only authorized filespace metadata. |
| Placement rights | Creating or moving objects into a filespace requires placement permission. |
| Administrative rights | Alter/drop/agent operations require storage administration authority. |
| Protected material | Storage references and support details are redacted where policy requires. |
| Role policy | A policy may apply to the filespace generally or to a specific role binding. |

## SHOW And DESCRIBE

Expected fields include:

- filespace UUID;
- name;
- role;
- storage reference class, redacted where needed;
- online/read-only/maintenance state;
- quota, used, reserved, free, and high-water evidence;
- page size and allocation unit where visible;
- growth policy and low-reserve threshold;
- sync and checksum policy;
- health diagnostics;
- shrink-readiness status;
- dependent object counts;
- active filespace agents;
- policy bindings and authorization epoch;
- recovery-required state and diagnostic reference.

`SHOW FILESPACE EXTENDED` is intended for broader operational inspection. `SHOW FILESPACE <name> HEALTH`, `CAPACITY`, and `SHRINK READINESS` are focused projections.

## Drop And Recreate

`DROP FILESPACE` is safe only when no required pages, object descriptors, recovery references, or active snapshots depend on the filespace.

| Option | Contract |
| --- | --- |
| `RESTRICT` | Refuse if any dependency remains. |
| `CASCADE` | Requires explicit cascade policy and must still preserve storage correctness. |
| `PRESERVE STORAGE` | Retires catalog metadata while leaving storage content according to policy. |
| `DESTROY STORAGE` | Requires explicit destructive authority and safe-drop evidence. |

`RECREATE FILESPACE` combines replacement semantics and must refuse if it would hide unsafe data movement, dependency breakage, or recovery uncertainty.

## Failure Modes

| Failure | Required Behavior |
| --- | --- |
| Unknown or hidden filespace | Return not-found or hidden-object diagnostic according to policy. |
| Unauthorized placement | Refuse before creating or changing descriptors. |
| Storage reference denied | Refuse without parser-side file access. |
| Quota below used/reserved space | Refuse unless an admitted shrink/relocation plan exists. |
| Active dependency blocks drop | Refuse or require explicit cascade policy; never orphan pages. |
| Low reserve | Emit health/capacity evidence and request growth where admitted. |
| Agent unavailable | Return diagnostic or quarantine evidence; do not treat presence of an agent descriptor as successful action. |
| Recovery required | Fence unsafe reads/writes and expose only authorized diagnostics or repair routes. |
| Sync/integrity weakening denied | Refuse unless policy explicitly permits the downgrade. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every filespace lifecycle statement shape is recognized by SBsql. |
| Bind | Filespace names, roles, policies, storage references, and agent names resolve exactly. |
| Authorize | Effective user or agent has the required storage/catalog/security authority. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Store | Filespace descriptor, role, allocation policy, integrity policy, and dependencies persist by UUID. |
| Allocate | Storage code, not the parser, performs allocation and growth. |
| Report | `SHOW`/`DESCRIBE` projections match storage evidence and redact protected fields. |
| Fence | Low-reserve, read-only, maintenance, offline, recovery, and drop states fail closed where required. |
| Recover | Crash/restart never leaves silent orphan pages, broken filespace bindings, or false health status. |
