# Information Projection Naming Decision

Search key: `PUBLIC_RELEASE_FOUNDATION_INFORMATION_NAMING`

## Decision

This execution_plan must not create competing user-facing catalog authorities.

For the current canonical spec baseline:

- `sys.catalog` is the low-level UUID catalog authority.
- `sys.catalog_readable` is the ScratchBird readable projection family for
  non-standard friendly catalog views.
- `sys.information` is the canonical recursive user-facing information
  projection family.
- `sys.information_schema` is a legacy SQL client synonym object for
  `sys.information`. It has its own synonym object UUID under parent `sys`, but
  resolution must dereference it to the same final schema UUID as
  `sys.information`. It must not create a second schema root, second view tree,
  second cache path, or second invalidation path.

The trailing `_schema` name component is redundant in ScratchBird's recursive
system schema tree. It exists only for older SQL client compatibility where a
driver or reference dialect expects `INFORMATION_SCHEMA` spelling.

## View Rules

- User-facing projection views resolve display names through the resolver.
- Projection views apply security filtering before row emission.
- Projection views do not expose raw UUIDs where the controlling spec forbids
  UUID exposure.
- Canonical ScratchBird information views live under `sys.information`.
- Legacy `sys.information_schema` references reconcile through SQL object
  synonym dereference to the same `sys.information` schema UUID before child
  object lookup continues.
- SQL-standard views use standard spelling, 1-based ordinals, and `YES`/`NO`
  booleans.
- ScratchBird-specific readable views live under `sys.information`.

## Acceptance

The `information_projection_naming_gate` fails if implementation creates
parallel projection roots with different data sources, embeds human-readable
names in authority tables, treats `sys.information_schema` as canonical
authority, resolves `sys.information_schema` to a final schema UUID different
from `sys.information`, treats the synonym object UUID as a child parent, or
bypasses resolver/security filtering.

Detailed SQL object synonym product behavior is controlled by
`public_contract_snapshot`.
The execution_plan artifact `artifacts/SYNONYM_OBJECT_SEMANTICS.md` records execution
evidence requirements only.
