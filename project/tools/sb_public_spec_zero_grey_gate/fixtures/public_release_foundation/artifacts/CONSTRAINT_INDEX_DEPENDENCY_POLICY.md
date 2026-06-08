# Constraint And Index Dependency Policy

Search key: `PUBLIC_RELEASE_FOUNDATION_CONSTRAINT_INDEX_POLICY`

## Rule

Logical constraints are catalog authority. Backing indexes are support
structures. A backing index may enforce or accelerate a constraint, but it is
not the constraint itself.

## Required Backing Index Behavior

| Constraint Class | Backing Index Requirement |
| --- | --- |
| Primary key | Required unique btree or equivalent unique index family with MGA visibility. |
| Unique key | Required unique index with null and collation behavior recorded in descriptor. |
| Foreign key | Required lookup path on referenced key; child-side index recommended but policy-controlled. |
| Exclusion constraint | Required compatible index family or deterministic refusal diagnostic. |
| Check constraint | No backing index required. |
| Not null | No backing index required. |
| Default/generated value | No backing index required unless paired with key constraint. |

## Build Order

1. Create constraint catalog descriptor.
2. Validate referenced descriptors and backing-index capability.
3. Build or bind support index inside the same transaction where required.
4. Publish constraint visibility only after catalog, index, and evidence are
   durable under MGA rules.
5. Roll back descriptor and backing index together on transaction rollback.

## Acceptance

The `constraint_index_dependency_gate` fails if a key-like constraint can be
marked valid without a required backing index or if an index becomes the source
of truth for logical constraint identity.
