# Persistent Format And Migration Policy

Search key: `PUBLIC_RELEASE_FOUNDATION_PERSISTENT_FORMAT_POLICY`

## Scope

This policy covers durable changes made by this execution_plan:

- `sys.catalog` table descriptors, common columns, resolver rows, dependency
  edges, diagnostics, and projection metadata.
- Constraint and key catalog descriptors plus backing-index links.
- Checkpoint records, dirty-object manifests, recovery evidence, backup
  manifests, delta manifests, restore points, and PITR replay metadata.
- Page durability markers, generation fields, and allocation-map publication
  rules.

## Rules

- Every durable structure added by this execution_plan must have a format identifier,
  schema version, validation routine, and diagnostic for unsupported versions.
- Upgrade is explicit and transactional. A partially upgraded database must open
  in restricted diagnostic/repair mode or fail closed.
- Downgrade is not automatic. Opening a newer format with older code must emit a
  deterministic refusal diagnostic.
- Repair may classify structures as rebuildable, quarantined, or fatal. Repair
  may not invent missing catalog identity or missing transaction finality.
- Backup, delta, and PITR metadata are derived from MGA evidence. They are not
  recovery authority and cannot replace transaction inventory or page state.
- No WAL, redo log, donor transaction log, or CDC stream may become the durable
  recovery authority.

## Required Version Classes

| Class | Required Fields |
| --- | --- |
| Catalog format | catalog format version, descriptor set version, name resolver version, dependency graph version |
| Constraint format | constraint descriptor version, backing index contract version, enforcement semantics version |
| Recovery format | checkpoint version, dirty manifest version, operation envelope version, transaction inventory version |
| Backup format | manifest version, database UUID, filespace UUID set, coverage range, checksum/signature profile |
| Delta format | source backup UUID, timeline/fork UUID, contiguous segment range, coverage proof, gap classification |
| PITR format | restore point UUID, target transaction/timestamp/name, coverage proof, replay profile |

## Acceptance

The `persistent_format_migration_policy_gate` passes only when every durable
structure added by this execution_plan has version, validation, upgrade/refusal, and
diagnostic behavior in the implementation and tests.
