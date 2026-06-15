# Catalog Artifacts And External Git Versioning

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Related pages: [External Git Catalog Versioning (operator workflow)](../../Operations_Administration/external_git_catalog_versioning.md), [Git-Oriented Workflows (concept)](../../Getting_Started/architecture/git_support.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), [Security And Privileges](security_and_privilege_statements.md), [Transactions And Recovery](../core_paradigms/transactions_and_recovery.md).

## Purpose

ScratchBird can export the catalog as a set of content-hashed artifacts that an external Git repository can version, review, and diff over time. This lets a team maintain a Git-tracked history of catalog structure, diff the live catalog against a committed snapshot, and produce a rollback plan — while the engine retains full authority over every catalog change.

Two surfaces exist:

1. **SBsql statements** (`EXPORT CATALOG ARTIFACT`, `IMPORT CATALOG ARTIFACT`) — typed SQL statements accessible from any SBsql client.
2. **Engine artifact API / SBLR opcodes** — three additional operations invoked through the engine internal API or SBLR envelope by tooling. These have no typed SBsql spelling.

SBLR, UUID-backed catalog identity, and MGA transaction authority remain in force across all surfaces. Git never executes against the database and is never an authority.

## SBsql Statements

### EXPORT CATALOG ARTIFACT

Exports the current catalog as a set of `sb.catalog.artifact.v1` rows. Each row describes one catalog object (schema-tree record or API-behavior record).

**SBLR opcode:** `artifact.export_catalog` (`SBLR_ARTIFACT_EXPORT_CATALOG`)
**Engine operation:** `EngineExportCatalogArtifacts`
**Required right:** `right.catalog_read`
**Result shape:** `result.shape.catalog_artifact_rows`
**Recorded under:** `sys.catalog.artifacts`
**SBLR envelope:** `sblr.catalog.mutation.v3`

```sql
export catalog artifact;
```

The statement takes no additional arguments. It emits one result row per visible catalog object plus authority evidence on the result.

#### Export result rows

Each row carries:

| Field | Value |
| --- | --- |
| `artifact_format` | `sb.catalog.artifact.v1` |
| `artifact_kind` | `catalog_object` (schema-tree records) or `api_behavior_record` (all other catalog records) |
| `object_uuid` | Durable UUID identifying the catalog object. |
| `object_kind` | Object type string (e.g. `schema`). |
| `default_name` | Default resolver name of the object. |
| `payload` | Serialized object payload. |
| `identity_authority` | `uuid` |
| `runtime_authority` | `false` |

#### Export result evidence

| Evidence key | Value |
| --- | --- |
| `catalog_artifact_format` | `sb.catalog.artifact.v1` |
| `catalog_artifact_export_count` | Count of exported rows. |
| `git_runtime_authority` | `false` |

The schema covers all visible schema-tree records and all visible API-behavior records, sorted by `object_uuid`.

### IMPORT CATALOG ARTIFACT

Applies a set of catalog artifact rows through the engine. This is the **only admitted route** for applying a catalog change derived from an artifact export or rollback plan. Applying changes through Git or any other path outside the engine API is refused.

**SBLR opcode:** `artifact.import_catalog` (`SBLR_ARTIFACT_IMPORT_CATALOG`)
**Engine operation:** `EngineImportCatalogArtifacts`
**Required right:** `right.catalog_mutate`
**Result shape:** `result.shape.catalog_artifact_status`
**Recorded under:** `sys.catalog.artifacts`
**SBLR envelope:** `sblr.catalog.mutation.v3`

```sql
import catalog artifact;
```

Input rows must use format `sb.catalog.artifact.v1` and supply `object_uuid`, `object_kind`, and (where applicable) `default_name` and `payload`.

#### Import options

| Option | Values | Meaning |
| --- | --- | --- |
| `conflict_policy:<value>` | `reject` (default), `replace` | `reject` refuses if the target UUID is already visible. `replace` overwrites. |
| `uuid_mode:<value>` | `preserve` (default), `remap` | `preserve` keeps `object_uuid` as the target UUID. `remap` uses the `remap_uuid` field from the row. |
| `allow_name_conflict:true` | boolean flag | Suppresses schema-path conflict checking when set. |
| `external_git_policy:enabled` | policy flag | Enables external-git authority-evidence emission on the import result (see below). |
| `allow_external_git_versioning:true` | policy flag | Alternative form of the external-git policy gate. |

The forbidden options `git_runtime_authority:true`, `external_git_direct_authority:true`, and `external_git_direct_apply:true` are refused on import with diagnostic `external_git_authority_forbidden`.

#### Import validation

Each row is validated before any row is applied. The batch commits only after all rows pass. Per-row validation checks:

| Diagnostic | Condition |
| --- | --- |
| `artifact_format_invalid` | Row `artifact_format` is not `sb.catalog.artifact.v1`. |
| `artifact_object_uuid_required` | Row has no `object_uuid`. |
| `artifact_object_kind_required` | Row has no `object_kind`. |
| `artifact_target_uuid_required` | Derived target UUID is empty (can occur with `uuid_mode:remap` and no `remap_uuid`). |
| `artifact_duplicate_uuid_in_batch:<uuid>` | The same target UUID appears more than once in the batch. |
| `artifact_conflict_policy_invalid` | `conflict_policy` option value is not `reject` or `replace`. |
| `artifact_uuid_conflict:<uuid>` | Target UUID already visible and `conflict_policy` is `reject`. |
| `artifact_policy_validation_failed` | Payload contains `policy_status:invalid` or `unsafe_profile:true`. |
| `artifact_parent_schema_not_visible` | An object with `object_kind=schema` references a parent schema UUID that is not visible and not in the current batch. |
| `artifact_schema_path_conflict:<path>` | A schema object's name would conflict with an existing visible name (unless `allow_name_conflict:true` is set). |
| `artifact_rows_required` | The request carries no rows. |
| `artifact_uuid_mode_invalid` | `uuid_mode` option value is not `preserve` or `remap`. |

#### Import result evidence

| Evidence key | Value |
| --- | --- |
| `catalog_artifact_imported` | UUID of each imported object (one evidence entry per object). |
| `catalog_artifact_import_count` | Total count of imported objects. |
| `git_runtime_authority` | `false` |
| `external_git_import_authority` | `authorized_catalog_api_not_git_repository` (emitted when an external-git policy option is present). |
| `mga_transaction_authority` | `local_mga_transaction_inventory` (emitted when an external-git policy option is present). |

Import applies all staged records through `AppendApiBehaviorEvent`, which is an MGA-governed, transactional operation. The entire batch succeeds or fails together.

## Engine Artifact API / SBLR Opcodes

The following three operations are **not typed SBsql statements**. They are invoked through the engine internal API or SBLR envelope — for example by tooling, platform agents, or the external-git integration layer. All three require the policy gate. None of the three applies changes; they produce review artifacts only.

### artifact.external\_git.export\_snapshot

Exports a content-hashed snapshot of the catalog in `sb.external_git.catalog_snapshot.v1` format, suitable for committing into an external Git repository.

**SBLR opcode:** `artifact.external_git.export_snapshot` (`SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT`)
**Engine operation:** `EngineExportExternalGitSnapshot`
**Support level:** `SblrOpcodeSupport::implemented`
**Policy gate required:** yes (see below)

The result contains one manifest row and one object row per catalog object.

#### Snapshot manifest row fields

| Field | Value |
| --- | --- |
| `artifact_format` | `sb.external_git.catalog_snapshot.v1` |
| `snapshot_entry_kind` | `manifest` |
| `snapshot_mode` | `export_snapshot` |
| `database_uuid` | Canonical UUID of the database. |
| `local_transaction_id` | Transaction ID at which the snapshot was taken. |
| `catalog_artifact_format` | `sb.catalog.artifact.v1` |
| `entry_count` | Count of object rows. |
| `identity_authority` | `uuid` |
| `catalog_runtime_authority` | `ScratchBird_catalog_api` |
| `mga_transaction_authority` | `local_mga_transaction_inventory` |
| `git_runtime_authority` | `false` |
| `external_git_repository_authority` | `false` |

#### Snapshot object row fields

| Field | Value |
| --- | --- |
| `artifact_format` | `sb.external_git.catalog_snapshot.v1` |
| `catalog_artifact_format` | `sb.catalog.artifact.v1` |
| `snapshot_entry_kind` | `object` |
| `snapshot_mode` | `export_snapshot` |
| `object_uuid` | Durable UUID identifying the catalog object. |
| `object_kind` | Object type string. |
| `default_name` | Default resolver name. |
| `payload` | Serialized object payload. |
| `content_hash` | FNV-1a-64 hash of `object_uuid + object_kind + default_name + payload`. |
| `identity_authority` | `uuid` |
| `runtime_authority` | `false` |

Object rows are sorted by `object_uuid`.

#### Export snapshot result evidence

| Evidence key | Value |
| --- | --- |
| `external_git_versioning` | `convenience_snapshot_review_only` |
| `git_runtime_authority` | `false` |
| `external_git_repository_authority` | `false` |
| `catalog_runtime_authority` | `ScratchBird_catalog_api` |
| `mga_transaction_authority` | `local_mga_transaction_inventory` |
| `external_git_snapshot_export_count` | Count of exported object rows. |

### artifact.external\_git.diff\_snapshot

Compares the live catalog to a candidate snapshot supplied in the request rows. Returns diff rows classifying each object.

**SBLR opcode:** `artifact.external_git.diff_snapshot` (`SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT`)
**Engine operation:** `EngineDiffExternalGitSnapshot`
**Support level:** `SblrOpcodeSupport::implemented`
**Policy gate required:** yes (see below)
**Input required:** one or more snapshot rows

#### Diff row fields

| Field | Value |
| --- | --- |
| `artifact_format` | `sb.external_git.catalog_diff.v1` |
| `diff_kind` | See diff kinds table below. |
| `object_uuid` | UUID of the object (current or candidate, whichever is present). |
| `object_kind` | Object type string. |
| `current_hash` | `content_hash` from the live catalog entry, or empty if the object is not in the live catalog. |
| `candidate_hash` | `content_hash` from the candidate snapshot entry, or empty if the object is not in the candidate. |
| `git_runtime_authority` | `false` |
| `requires_authorized_catalog_import` | `true` |
| `mga_transaction_authority` | `local_mga_transaction_inventory` |

#### Diff kinds

| `diff_kind` | Meaning |
| --- | --- |
| `unchanged` | All objects match; a single row with this kind is emitted when there are no differences. |
| `modified` | Object exists in both the live catalog and the candidate but payload or name differs (detected via `object_kind + default_name + payload` signature). |
| `added_in_candidate` | Object exists in the candidate snapshot but not in the live catalog. |
| `removed_from_candidate` | Object exists in the live catalog but not in the candidate snapshot. |

#### Diff result evidence

| Evidence key | Value |
| --- | --- |
| `external_git_versioning` | `convenience_snapshot_review_only` |
| `git_runtime_authority` | `false` |
| `external_git_repository_authority` | `false` |
| `catalog_runtime_authority` | `ScratchBird_catalog_api` |
| `mga_transaction_authority` | `local_mga_transaction_inventory` |
| `external_git_diff_count` | Count of changed objects (0 when the `unchanged` sentinel row is emitted). |

### artifact.external\_git.rollback\_plan

Produces a rollback plan against a target snapshot. Returns one plan row per object that would need action to restore the live catalog to match the target. Does not apply any changes.

**SBLR opcode:** `artifact.external_git.rollback_plan` (`SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN`)
**Engine operation:** `EnginePlanExternalGitRollback`
**Support level:** `SblrOpcodeSupport::implemented`
**Policy gate required:** yes (see below)
**Input required:** one or more snapshot rows (the target state)

#### Rollback plan row fields

| Field | Value |
| --- | --- |
| `artifact_format` | `sb.external_git.rollback_plan.v1` |
| `rollback_action` | See rollback actions table below. |
| `object_uuid` | UUID of the object. |
| `object_kind` | Object type string. |
| `default_name` | Default resolver name. |
| `payload` | Serialized object payload. |
| `restore_hash` | Content hash of the object as it appears in the live catalog. |
| `target_hash` | Content hash of the object in the target snapshot (empty when `rollback_action` is `restore_current_catalog_artifact` and the object is absent from the target). |
| `apply_route` | `authorized_catalog_api` |
| `git_runtime_authority` | `false` |
| `plan_runtime_authority` | `false` |

#### Rollback actions

| `rollback_action` | Meaning |
| --- | --- |
| `restore_current_catalog_artifact` | The live catalog object differs from (or is absent from) the target. To reconcile, re-import the object through `IMPORT CATALOG ARTIFACT`. |
| `reject_candidate_only_object_until_authorized_catalog_create` | The target snapshot contains an object that does not exist in the live catalog. The plan records it for review; creating it requires an authorized `IMPORT CATALOG ARTIFACT`. |
| `no_action_required` | The live catalog already matches the target. A single row with this action is emitted when there are no differences. |

#### Rollback plan result evidence

| Evidence key | Value |
| --- | --- |
| `external_git_versioning` | `convenience_snapshot_review_only` |
| `git_runtime_authority` | `false` |
| `external_git_repository_authority` | `false` |
| `catalog_runtime_authority` | `ScratchBird_catalog_api` |
| `mga_transaction_authority` | `local_mga_transaction_inventory` |
| `external_git_rollback_plan_count` | Count of plan rows (0 when the `no_action_required` sentinel row is emitted). |
| `external_git_rollback_apply_route` | `authorized_catalog_api_not_git_repository` |

## Artifact Formats

| Format identifier | Used by |
| --- | --- |
| `sb.catalog.artifact.v1` | `EXPORT CATALOG ARTIFACT` rows; `IMPORT CATALOG ARTIFACT` input rows; embedded `catalog_artifact_format` field in snapshot rows. |
| `sb.external_git.catalog_snapshot.v1` | `artifact.external_git.export_snapshot` result rows (manifest + object). |
| `sb.external_git.catalog_diff.v1` | `artifact.external_git.diff_snapshot` result rows. |
| `sb.external_git.rollback_plan.v1` | `artifact.external_git.rollback_plan` result rows. |

## Policy Gate And Forbidden Options

All three engine API / SBLR opcode operations validate the request before executing. `IMPORT CATALOG ARTIFACT` also validates for forbidden options.

### Required option (policy gate)

The request must carry at least one of:

| Option | Form |
| --- | --- |
| `external_git_policy:enabled` | Preferred form. |
| `allow_external_git_versioning:true` | Alternative form. |

If neither is present, the operation is refused with diagnostic `external_git_policy_required`.

### Forbidden options

The following options are refused on all external-git operations and on `IMPORT CATALOG ARTIFACT` with diagnostic `external_git_authority_forbidden`:

| Option | Meaning of the refusal |
| --- | --- |
| `git_runtime_authority:true` | Claiming runtime authority via Git is not permitted. |
| `external_git_direct_authority:true` | Claiming direct external-git authority is not permitted. |
| `external_git_direct_apply:true` | Claiming the right to apply changes directly through Git is not permitted. |

### Snapshot validation diagnostics

When a candidate or target snapshot is supplied as input rows (diff and rollback plan operations), the engine recomputes the content hash for each object row and validates the snapshot before comparing. The following diagnostics are produced before any comparison work:

| Diagnostic | Condition |
| --- | --- |
| `external_git_snapshot_rows_required` | No rows were supplied to an operation that requires them. |
| `external_git_snapshot_format_invalid` | A row carries an `artifact_format` that is neither `sb.catalog.artifact.v1` nor `sb.external_git.catalog_snapshot.v1`. |
| `external_git_snapshot_object_required` | A non-manifest row has an empty `object_uuid` or `object_kind`. |
| `external_git_snapshot_duplicate_uuid:<uuid>` | The same UUID appears more than once in the submitted snapshot. |
| `external_git_snapshot_hash_mismatch:<uuid>` | A row supplies a `content_hash` that does not match the engine-recomputed hash for that object. |

Manifest rows (`snapshot_entry_kind = manifest`) are silently skipped during snapshot parsing; they are not treated as object entries.

## Authority Evidence

Every result from an external-git operation carries the following evidence. These values are enforced by the engine, not advisory labels.

| Evidence key | Value | Meaning |
| --- | --- | --- |
| `external_git_versioning` | `convenience_snapshot_review_only` | This output is a review and versioning convenience only. |
| `git_runtime_authority` | `false` | Git never executes and has no runtime authority over the database. |
| `external_git_repository_authority` | `false` | An external Git repository has no catalog authority. |
| `catalog_runtime_authority` | `ScratchBird_catalog_api` | Catalog authority belongs to the ScratchBird catalog API. |
| `mga_transaction_authority` | `local_mga_transaction_inventory` | Transaction finality authority belongs to the local MGA transaction inventory. |
| `identity_authority` | `uuid` | Object identity is UUID-based, not name-based. |

Diff rows additionally carry `requires_authorized_catalog_import = true`. Rollback plan rows carry `apply_route = authorized_catalog_api` and `plan_runtime_authority = false`. The rollback result evidence carries `external_git_rollback_apply_route = authorized_catalog_api_not_git_repository`.

These invariants mean that any catalog change — whether planned from a diff, derived from a rollback plan, or sourced from any other artifact — must flow through `IMPORT CATALOG ARTIFACT` under `right.catalog_mutate`. There is no path by which an external Git repository or any option on a request can bypass this.

## Content Hash

The `content_hash` on every object row is a stable FNV-1a-64 hash of the concatenated fields `object_uuid + "\n" + object_kind + "\n" + default_name + "\n" + payload`. The engine recomputes the hash when it receives a snapshot for diff or rollback, and it refuses with `external_git_snapshot_hash_mismatch:<uuid>` if the supplied hash differs. This ensures that a snapshot row in a Git repository cannot be silently altered without the engine detecting the discrepancy.

## Related Surface Rows

| Surface | Kind | Opcode | SBLR opcode constant | Result shape |
| --- | --- | --- | --- | --- |
| `EXPORT CATALOG ARTIFACT` | SBsql statement | `artifact.export_catalog` | `SBLR_ARTIFACT_EXPORT_CATALOG` | `result.shape.catalog_artifact_rows` |
| `IMPORT CATALOG ARTIFACT` | SBsql statement | `artifact.import_catalog` | `SBLR_ARTIFACT_IMPORT_CATALOG` | `result.shape.catalog_artifact_status` |
| export snapshot | engine API / SBLR | `artifact.external_git.export_snapshot` | `SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT` | — |
| diff snapshot | engine API / SBLR | `artifact.external_git.diff_snapshot` | `SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT` | — |
| rollback plan | engine API / SBLR | `artifact.external_git.rollback_plan` | `SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN` | — |

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `EXPORT CATALOG ARTIFACT` and `IMPORT CATALOG ARTIFACT` are recognized by SBsql. Engine API operations are invoked through the SBLR envelope. |
| Policy gate | Request carries `external_git_policy:enabled` or `allow_external_git_versioning:true` for engine API operations. |
| Forbidden options | None of `git_runtime_authority:true`, `external_git_direct_authority:true`, or `external_git_direct_apply:true` are present. |
| Authorize | The caller holds `right.catalog_read` for export; `right.catalog_mutate` for import. |
| Snapshot validation | Submitted snapshot rows satisfy format, presence, uniqueness, and hash requirements before comparison or planning begins. |
| Import validation | Each row passes format, UUID, kind, conflict-policy, policy-payload, and schema-path checks. |
| Execute | Export operations read the catalog without mutation. Import applies all staged records or fails atomically. |
| Finalize | Import commits through MGA finality. |
| Evidence | All results carry the authority-evidence fields listed above. |
