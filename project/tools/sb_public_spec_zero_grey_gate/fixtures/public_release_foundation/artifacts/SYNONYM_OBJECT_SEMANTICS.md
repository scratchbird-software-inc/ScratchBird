# SQL Object Synonym Semantics

Search key: `PUBLIC_RELEASE_FOUNDATION_SYNONYM_OBJECT_SEMANTICS`

Product behavior authority:
`public_contract_snapshot`.

This artifact records execution_plan evidence requirements. It must not supersede the
canonical contract.

## Decision

Synonyms are first-class SQL objects. They are not extra name rows and they are
not a second implementation branch for the target object.

A synonym has its own object UUID, parent object UUID, lifecycle, owner, policy,
catalog generation, transaction visibility, dependency edge, and resolver name.
It points to another SQL object UUID. The target may itself be a synonym, but
resolution must stop after at most five synonym hops and must fail on cycles.

The final resolved object acts as the effective object for execution and child
placement. The synonym is only part of the resolution route.

## Required Catalog Shape

The implementation must add a durable synonym descriptor family equivalent to
`sys.catalog.synonym` with these authority fields:

- synonym object UUID
- parent object UUID
- target object UUID
- target object class
- lifecycle state
- creator transaction and catalog generation
- dependency edge from synonym object UUID to target object UUID
- policy and owner fields required by the common catalog descriptor model

The synonym display name is stored only through the identity resolver. The
target object's display names remain the target object's resolver rows. The
synonym descriptor must not duplicate target names or target metadata.

## Name Determination Rules

For each parent object and language determination scope:

- No two SQL objects may have equivalent deterministic names under the same
  parent for the same language and identifier profile.
- The database default language is always checked as a conflict domain.
- A non-default language name must not resolve to the same deterministic lookup
  key as another visible object under the same parent when fallback to the
  default language would make resolution ambiguous.
- A synonym name conflicts the same way as any other SQL object name.
- These rules apply before catalog rows are published and rollback must remove
  provisional synonym rows and resolver rows under MGA.

## Resolution Rules

Resolution of a path segment first finds a SQL object under the current parent.
If that object is a synonym:

1. Append the synonym object UUID to the resolution chain.
2. Load the synonym target object UUID and class through catalog authority.
3. Check visibility and authorization for the synonym and final target.
4. Continue resolving through the target object.
5. Stop after five synonym hops with a stable depth diagnostic.
6. Fail immediately on a repeated synonym object UUID in the same chain.

The bound identity returned to execution uses the final target object UUID and
resolved object class. Diagnostic and audit evidence must retain the synonym
chain used for the bind.

## Parent Remap Rules

When a parent path resolves through a synonym to a schema, package, table, or
other parent-capable object, creation and child lookup use the final target
object as the true parent.

Example:

```sql
create table sys.information_schema.mytable (...);
```

If `sys.information_schema` is a synonym to `sys.information`, then `mytable` is
created under the `sys.information` schema UUID. It is not created under the
synonym object UUID.

## Required `sys.information_schema` Behavior

`sys.information` is the true schema. `sys.information_schema` is a synonym SQL
object under parent `sys` that points to `sys.information`.

Both paths must resolve to the same final schema UUID for child lookup:

- `sys.information.myview`
- `sys.information_schema.myview`

The synonym object has its own object UUID, but it is never used as the parent
for children created through the synonym path.

## Acceptance

The synonym gates fail if implementation:

- represents SQL object synonyms only as additional name rows
- creates a second schema branch for `sys.information_schema`
- gives children created through a synonym the synonym object UUID as parent
- permits name conflicts under the same parent and language determination scope
- follows more than five synonym hops
- misses a synonym cycle
- resolves execution against the synonym object instead of the final target
- omits dependency evidence from synonym object UUID to target object UUID
