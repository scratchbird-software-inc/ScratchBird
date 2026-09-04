# Operator workflows

These examples show the published management adapters. They are not alternate
SQL execution, catalog, authorization, transaction, or recovery paths.

## Inspect optimization and authorization

```sql
SELECT *
FROM sys.management.performance_optimization_surface;

SELECT *
FROM sys.management.performance_optimization_config;

SELECT *
FROM sys.management.authorization_decisions;
```

Compare the full generation and epoch tuple before comparing samples. If the
tuple changes, refresh the complete view instead of merging cached rows.

## Show current management state

```sql
SHOW MANAGEMENT;
```

Display the engine message vector, snapshot identity, and finality fields with
the rows. Do not translate a missing value into an affirmative state.

## Inspect and control indexes

```sql
CALL sys.management.index_validate(...);
CALL sys.management.index_backlog(...);
CALL sys.management.index_repair(...);
```

Validation and backlog inspection are read operations. Repair and other
mutating operations require the exact returned authorization route and remain
inside the session transaction. A UI must wait for engine finality and preserve
all warning and refusal messages.

## Prepare a support bundle

```sql
CALL sys.management.prepare_support_bundle(...);
```

The caller supplies only the admitted scope and policy references. The engine
performs authorization, flushing, redaction, completeness checks, and audit
publication. A UI must not add filesystem paths or protected bytes to the
result. It should present redaction and completeness state before offering the
bundle for export.

## Refusal handling

For any workflow, render the canonical diagnostic code, SQLSTATE, ordered
message vector, and finality state together. Security failures must remain
non-disclosing. Transaction, generation, cluster, and resource refusals must not
be retried under a newly fabricated identity; reacquire the appropriate
engine-issued authority first.
