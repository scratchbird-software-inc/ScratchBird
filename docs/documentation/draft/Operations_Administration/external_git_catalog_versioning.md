# External Git Catalog Versioning

## Purpose

This chapter explains the operator workflow for the external Git catalog versioning feature: how to enable it, how to export a catalog snapshot, how to commit that snapshot to an external Git repository, how to diff the live catalog against a previously committed snapshot, how to read a rollback plan, and how to apply a reconciliation through the engine.

This is a review-and-versioning convenience. It lets a team maintain a Git-tracked history of catalog structure and plan rollbacks from it. It does not change who has authority: Git never executes against the database, and every catalog change must flow through the ScratchBird engine's authorized catalog API.

Related pages: [Catalog Artifacts And External Git (language reference)](../Language_Reference/syntax_reference/catalog_artifacts_and_external_git.md), [Git-Oriented Workflows (concept)](../Getting_Started/architecture/git_support.md), [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md), [Identity, Security, And Policy](identity_security_and_policy.md).

## The Authority Boundary

Before walking through the workflow, it is worth stating the boundary explicitly because it affects every step.

ScratchBird exports the catalog as content-hashed artifacts. An external Git repository can store and version those artifacts. You can diff the live catalog against a snapshot from Git and produce a rollback plan from the diff. But:

- Git is external storage for review artifacts. It has no catalog authority, no transaction authority, and no runtime authority over the database.
- Every result from an external-git operation carries explicit evidence: `git_runtime_authority = false`, `external_git_repository_authority = false`, `catalog_runtime_authority = ScratchBird_catalog_api`, `mga_transaction_authority = local_mga_transaction_inventory`.
- A diff row carries `requires_authorized_catalog_import = true`. A rollback plan row carries `apply_route = authorized_catalog_api` and `plan_runtime_authority = false`.
- The rollback result evidence carries `external_git_rollback_apply_route = authorized_catalog_api_not_git_repository`.
- The only admitted way to apply a catalog change derived from an artifact — whether from an export, a diff, or a rollback plan — is `IMPORT CATALOG ARTIFACT` through the engine under `right.catalog_mutate`.

Any request that tries to claim direct authority by carrying option `git_runtime_authority:true`, `external_git_direct_authority:true`, or `external_git_direct_apply:true` is refused with diagnostic `external_git_authority_forbidden`.

## Step 1: Enable The Policy Gate

The three engine API / SBLR opcode operations (`artifact.external_git.export_snapshot`, `artifact.external_git.diff_snapshot`, `artifact.external_git.rollback_plan`) require an explicit opt-in option on every request.

The request must include one of:

| Option | Form |
| --- | --- |
| `external_git_policy:enabled` | Preferred form. |
| `allow_external_git_versioning:true` | Alternative form. |

Without one of these, the engine refuses with `external_git_policy_required` before performing any catalog read. This is not a configuration file setting — it is a per-request option that the caller (a tool, agent, or platform integration layer) must include every time it invokes an external-git operation.

If you are building tooling, ensure the option is included in every SBLR envelope that invokes one of the three engine API operations. The two SBsql statements (`EXPORT CATALOG ARTIFACT`, `IMPORT CATALOG ARTIFACT`) do not require the policy option, though they still refuse the forbidden authority options.

## Step 2: Export A Catalog Snapshot

To obtain a snapshot suitable for Git storage, invoke `artifact.external_git.export_snapshot` through the engine artifact API or SBLR envelope with the policy option present.

The result contains:

- one **manifest row** (`snapshot_entry_kind = manifest`) carrying metadata: `database_uuid`, `local_transaction_id`, `entry_count`, `catalog_artifact_format`, and authority evidence;
- one **object row** (`snapshot_entry_kind = object`) per visible catalog object, sorted by `object_uuid`.

Each object row carries:

| Field | Content |
| --- | --- |
| `object_uuid` | Durable UUID of the catalog object. |
| `object_kind` | Type of the object (e.g. `schema`). |
| `default_name` | Resolver name. |
| `payload` | Serialized object definition. |
| `content_hash` | FNV-1a-64 hash of `object_uuid + object_kind + default_name + payload`. |
| `artifact_format` | `sb.external_git.catalog_snapshot.v1` |
| `catalog_artifact_format` | `sb.catalog.artifact.v1` |

The snapshot covers visible schema-tree records and visible API-behavior records.

Alternatively, you can use the SBsql statement `EXPORT CATALOG ARTIFACT` (requires `right.catalog_read`) to obtain the same catalog objects in `sb.catalog.artifact.v1` format without the external-git snapshot envelope or authority evidence. This form does not require the policy option.

## Step 3: Commit The Snapshot To An External Git Repository

The snapshot rows are your data. Serialization, file layout, and Git commit workflows are your team's responsibility — the engine produces the row data and does not manage the external repository.

Recommended practices for the external repository:

- Store the manifest row and all object rows as files or structured data in a form your team can review and diff in pull requests.
- Include the `content_hash` field for every object row. The engine recomputes the hash and rejects any snapshot row where the supplied hash does not match (`external_git_snapshot_hash_mismatch:<uuid>`), so preserving the hash ensures round-trip integrity.
- Do not strip the authority evidence fields (`git_runtime_authority`, `external_git_repository_authority`, etc.) from stored rows. They serve as a tamper-evident reminder of the boundary.
- Use `object_uuid` as the durable identity for catalog objects. Names can change; UUIDs do not. This means two snapshots of the same database taken before and after a rename will track the rename correctly.
- Keep secrets and protected material out of the repository. Catalog `payload` fields should not contain raw secrets; they carry protected references if secrets are involved.

What you commit to Git is a versioned, diffable history of your catalog structure. You can use ordinary Git tooling — `git diff`, pull requests, blame — to review catalog changes over time.

## Step 4: Diff The Live Catalog Against A Committed Snapshot

To compare the current live catalog against a snapshot you previously committed, invoke `artifact.external_git.diff_snapshot` through the engine API or SBLR envelope with the policy option and the snapshot rows as input.

The engine:

1. Validates the policy gate.
2. Parses and validates the submitted snapshot rows (format, presence of `object_uuid` and `object_kind`, uniqueness of UUIDs, hash integrity).
3. Takes a fresh snapshot of the live catalog.
4. Compares the two sets by `object_uuid`, using `object_kind + default_name + payload` as the comparison signature.
5. Returns diff rows.

#### Diff kinds

| `diff_kind` | Meaning |
| --- | --- |
| `unchanged` | Live catalog and candidate snapshot match entirely. A single sentinel row with this kind is returned. |
| `modified` | The object exists in both but its signature differs (the definition changed). |
| `added_in_candidate` | The object is in the submitted candidate snapshot but not in the live catalog. |
| `removed_from_candidate` | The object is in the live catalog but not in the submitted candidate snapshot. |

Every diff row carries `requires_authorized_catalog_import = true`. The diff is a review output; it does not apply anything.

The result evidence includes `external_git_diff_count` (the count of changed objects) and the full authority evidence block (`git_runtime_authority = false`, `external_git_repository_authority = false`, `catalog_runtime_authority = ScratchBird_catalog_api`, `mga_transaction_authority = local_mga_transaction_inventory`).

## Step 5: Read A Rollback Plan

If you want to understand what changes are needed to restore the live catalog to a previously committed state, invoke `artifact.external_git.rollback_plan` with the target snapshot rows as input.

The engine produces one plan row per object that requires action. Plan rows use format `sb.external_git.rollback_plan.v1` and carry:

| Field | Content |
| --- | --- |
| `rollback_action` | Action required (see table below). |
| `object_uuid` | UUID of the object. |
| `object_kind` | Object type. |
| `default_name` | Resolver name. |
| `payload` | Serialized object definition. |
| `restore_hash` | Hash of the live-catalog version of the object. |
| `target_hash` | Hash of the target snapshot version (empty when the object is absent from the target). |
| `apply_route` | `authorized_catalog_api` |
| `git_runtime_authority` | `false` |
| `plan_runtime_authority` | `false` |

#### Rollback actions

| `rollback_action` | Meaning | Remediation |
| --- | --- | --- |
| `restore_current_catalog_artifact` | The live object differs from or is absent from the target. | Import the live-catalog version via `IMPORT CATALOG ARTIFACT` to re-establish it in the target state, or use the target's payload to import the desired version. |
| `reject_candidate_only_object_until_authorized_catalog_create` | The target snapshot has an object the live catalog does not. | The object must be created through an authorized `IMPORT CATALOG ARTIFACT` if the team decides to recreate it. |
| `no_action_required` | Live catalog already matches the target. A single sentinel row with this action is returned. | No action needed. |

The rollback plan result evidence includes `external_git_rollback_apply_route = authorized_catalog_api_not_git_repository` and `external_git_rollback_plan_count`.

The plan is advisory. It tells you what the engine would need applied to reconcile the live catalog to the target. It does not apply those changes itself.

## Step 6: Apply A Reconciliation Through The Engine

To act on a diff or a rollback plan, use `IMPORT CATALOG ARTIFACT` — the only admitted route for applying catalog artifact changes. This requires `right.catalog_mutate`.

```sql
import catalog artifact;
```

Supply the input rows using format `sb.catalog.artifact.v1`. Key import options:

| Option | Default | Notes |
| --- | --- | --- |
| `conflict_policy:reject` | Default | Refuses if the target UUID is already visible. Use when you expect new objects only. |
| `conflict_policy:replace` | — | Overwrites an existing object with the same UUID. Use when reconciling modified objects. |
| `uuid_mode:preserve` | Default | Uses the `object_uuid` from each row as the target UUID. Correct for round-tripping existing catalog objects. |
| `uuid_mode:remap` | — | Uses the `remap_uuid` field as the target UUID. Use only when intentionally remapping identity. |
| `allow_name_conflict:true` | — | Suppresses schema-path conflict checking. Use only when you have independently verified that name conflicts are safe. |
| `external_git_policy:enabled` | — | Not required for import, but when present causes the result to emit `external_git_import_authority = authorized_catalog_api_not_git_repository`. |

When importing a reconciliation derived from a rollback plan:

- For `restore_current_catalog_artifact` rows where the live object needs to be replaced with the target version: use `conflict_policy:replace` and supply the target payload.
- For `reject_candidate_only_object_until_authorized_catalog_create` rows: decide whether to create the object; if yes, import it as a new object with `conflict_policy:reject`.
- For objects that should remain as they are: omit them from the import batch.

The import validates all rows before applying any of them and applies the batch atomically through MGA-governed transactions. If any row fails validation, the entire batch is refused with a per-row diagnostic. On success, the result evidence includes `catalog_artifact_import_count` and one `catalog_artifact_imported = <uuid>` entry per imported object.

## Snapshot Validation: What The Engine Checks On Input

When you submit snapshot rows for diff or rollback plan, the engine validates the rows before comparing. Understanding these checks helps diagnose submission problems:

| Diagnostic | Condition |
| --- | --- |
| `external_git_snapshot_rows_required` | No rows were submitted. |
| `external_git_snapshot_format_invalid` | A row has an `artifact_format` that is not `sb.catalog.artifact.v1` or `sb.external_git.catalog_snapshot.v1`. |
| `external_git_snapshot_object_required` | A non-manifest row has an empty `object_uuid` or `object_kind`. |
| `external_git_snapshot_duplicate_uuid:<uuid>` | The same UUID appears more than once in the submitted rows. |
| `external_git_snapshot_hash_mismatch:<uuid>` | A row supplies a `content_hash` that does not match the engine-recomputed hash. |

Hash mismatches indicate that the snapshot row was altered after the engine produced it. This can happen if rows were edited in the Git repository, if serialization changed whitespace or encoding in `payload` or `default_name`, or if the `content_hash` field was not preserved faithfully. To diagnose, re-export the snapshot from the same database state and compare.

Manifest rows (`snapshot_entry_kind = manifest`) are silently skipped; only object rows are compared.

## Complete Workflow Summary

```text
1. Include external_git_policy:enabled in every engine API request.

2. Invoke artifact.external_git.export_snapshot.
   - Result: manifest row + one object row per catalog object.
   - Each object row carries object_uuid, object_kind, default_name, payload, content_hash.

3. Store the snapshot rows in your external Git repository.
   - Commit them for review, diffing, and history.
   - Preserve content_hash fields faithfully.

4. (Later) Invoke artifact.external_git.diff_snapshot with stored rows as input.
   - Result: diff rows classified as unchanged / modified / added_in_candidate / removed_from_candidate.
   - Each diff row carries requires_authorized_catalog_import = true.

5. (If rollback is needed) Invoke artifact.external_git.rollback_plan with target rows as input.
   - Result: plan rows with rollback_action, object data, restore_hash, target_hash.
   - Result evidence: external_git_rollback_apply_route = authorized_catalog_api_not_git_repository.

6. Apply via IMPORT CATALOG ARTIFACT (right.catalog_mutate required).
   - Input rows in sb.catalog.artifact.v1 format.
   - Choose conflict_policy:reject or conflict_policy:replace as appropriate.
   - Batch commits atomically through MGA.
   - Never apply via Git directly; there is no engine-supported path to do so.
```

## Diagnostics Reference

| Diagnostic | Produced by | Meaning |
| --- | --- | --- |
| `external_git_policy_required` | All three engine API operations | Request did not include `external_git_policy:enabled` or `allow_external_git_versioning:true`. |
| `external_git_authority_forbidden` | All three engine API operations; `IMPORT CATALOG ARTIFACT` | Request included a forbidden option (`git_runtime_authority:true`, `external_git_direct_authority:true`, or `external_git_direct_apply:true`). |
| `external_git_snapshot_rows_required` | diff\_snapshot, rollback\_plan | No input rows supplied. |
| `external_git_snapshot_format_invalid` | diff\_snapshot, rollback\_plan | An input row has an unrecognized `artifact_format`. |
| `external_git_snapshot_object_required` | diff\_snapshot, rollback\_plan | A non-manifest input row is missing `object_uuid` or `object_kind`. |
| `external_git_snapshot_duplicate_uuid:<uuid>` | diff\_snapshot, rollback\_plan | The same UUID appears more than once in the input. |
| `external_git_snapshot_hash_mismatch:<uuid>` | diff\_snapshot, rollback\_plan | The supplied `content_hash` for an object does not match the engine-recomputed hash. |
| `artifact_format_invalid` | `IMPORT CATALOG ARTIFACT` | An input row does not carry `artifact_format = sb.catalog.artifact.v1`. |
| `artifact_object_uuid_required` | `IMPORT CATALOG ARTIFACT` | An input row has no `object_uuid`. |
| `artifact_object_kind_required` | `IMPORT CATALOG ARTIFACT` | An input row has no `object_kind`. |
| `artifact_uuid_conflict:<uuid>` | `IMPORT CATALOG ARTIFACT` | Target UUID already visible and `conflict_policy` is `reject`. |
| `artifact_policy_validation_failed` | `IMPORT CATALOG ARTIFACT` | Row payload contains `policy_status:invalid` or `unsafe_profile:true`. |
| `artifact_schema_path_conflict:<path>` | `IMPORT CATALOG ARTIFACT` | A schema object's name conflicts with an existing visible name. |
| `artifact_parent_schema_not_visible` | `IMPORT CATALOG ARTIFACT` | A schema object's parent UUID is not visible and not in the batch. |
| `artifact_rows_required` | `IMPORT CATALOG ARTIFACT` | No input rows supplied. |

## Security And Privilege Notes

| Concern | Rule |
| --- | --- |
| Export right | `EXPORT CATALOG ARTIFACT` requires `right.catalog_read`. |
| Import right | `IMPORT CATALOG ARTIFACT` requires `right.catalog_mutate`. This is a catalog mutation and should be granted only to operators who are authorized to change the catalog. |
| Engine API operations | Invoked through the engine internal API or SBLR envelope, not through SBsql text. Access controls are those of the invoking tool or agent. |
| Protected material | Catalog payload fields contain protected references, not raw secrets. Do not strip protection references when storing snapshot rows. |
| Sandbox | Import applies objects within the caller's sandbox root. Objects that would violate sandbox policy are refused. |
| Disclosure | Snapshot rows may contain schema structure details. Treat the Git repository as an appropriate level of confidentiality for that information. |
