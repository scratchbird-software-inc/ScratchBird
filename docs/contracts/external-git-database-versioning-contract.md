# External Git Database Versioning Contract

ScratchBird external Git database versioning is a review and workflow convenience
surface. It exports deterministic catalog artifact snapshots that a team can put
under an external Git repository, review, diff, and use to generate rollback
plans.

Git is never ScratchBird runtime authority.

## Authority Boundary

- ScratchBird UUIDs remain object identity authority.
- ScratchBird catalog APIs remain catalog mutation authority.
- ScratchBird MGA transaction inventory remains commit, rollback, visibility,
  and recovery authority.
- External Git repositories are evidence and review material only.
- Applying any Git-derived material must route through an authorized ScratchBird
  catalog API under an active transaction.
- Direct Git checkout, Git history, Git timestamps, Git commit ordering, or Git
  branches must not decide catalog visibility or transaction finality.

## Engine Operations

The public engine artifact API exposes these SBLR operations:

- `artifact.external_git.export_snapshot`
  - opcode: `SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT`
  - exports `sb.external_git.catalog_snapshot.v1` rows.
- `artifact.external_git.diff_snapshot`
  - opcode: `SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT`
  - compares a candidate snapshot with the currently visible catalog.
- `artifact.external_git.rollback_plan`
  - opcode: `SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN`
  - emits rollback plan rows that must be applied through authorized catalog
    APIs.

All three operations require security context, database path, and transaction
context. The caller must pass `external_git_policy:enabled` or
`allow_external_git_versioning:true`.

## Validation

Snapshot rows carry UUID identity, object kind, default name, payload, and a
deterministic content hash. Diff and rollback-plan routes reject duplicate UUIDs,
invalid snapshot formats, missing object identity, and content-hash mismatches.

The import route rejects `git_runtime_authority:true`,
`external_git_direct_authority:true`, and `external_git_direct_apply:true`.

## Proof Coverage

The public CTest suite covers this contract in:

- `database_lifecycle_backup_restore_export_admin_gate_conformance`
- `sbsql_sblr_final_cleanup_b003_engine_api_route_conformance`

