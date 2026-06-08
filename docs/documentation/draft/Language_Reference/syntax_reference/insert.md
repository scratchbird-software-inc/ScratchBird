# INSERT Statement

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_insert`

Related pages: [SELECT Statement](select.md), [WITH And Common Table Expressions](with.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Index Lifecycle](index.md), [Trigger Lifecycle](trigger.md), [Transaction Control](transaction_control.md), [Type System Overview](../data_types/type_system_overview.md), and [Document, Graph, Vector, And Multimodel Types](../data_types/document_graph_vector_and_multimodel_types.md).

## Purpose

`INSERT` constructs one or more new row versions in the active transaction. The statement may supply explicit values, default-only rows, rows produced by a query, or rows projected from an admitted multimodel source. Before any row can become durable and visible, the engine verifies descriptors, defaults, generated values, identity values, domains, constraints, indexes, triggers, policies, storage placement, and authorization.

The parser recognizes the statement and lowers it to SBLR. It does not create row truth. ScratchBird MGA decides transaction identity, row visibility, commit, rollback, cleanup, and recovery.

Use `INSERT` when the target is a rowset target: a table, an updatable view, or a descriptor-bound projection that admits row insertion. Use dedicated multimodel statements when the operation is primarily a document, graph, vector, search, time-series, or key-value command rather than a rowset insertion.

## Syntax

```ebnf
insert_statement ::=
    INSERT INTO insert_target insert_column_list? insert_source returning_clause? ;

insert_target ::=
    qualified_name target_alias? ;

insert_column_list ::=
    "(" insert_column ("," insert_column)* ")" ;

insert_column ::=
      identifier
    | qualified_identifier
    | path_target ;

insert_source ::=
      values_insert_source
    | query_insert_source
    | default_values_source
    | multimodel_insert_source ;

values_insert_source ::=
    VALUES row_constructor ("," row_constructor)* ;

row_constructor ::=
      "(" insert_value ("," insert_value)* ")"
    | ROW "(" insert_value ("," insert_value)* ")" ;

insert_value ::=
      expression
    | DEFAULT ;

query_insert_source ::=
    query_statement ;

default_values_source ::=
    DEFAULT VALUES ;

returning_clause ::=
    RETURNING projection_list ;
```

`path_target` is admitted only for descriptor-bound structured targets, such as a document field exposed as an insertable rowset column. A path name never bypasses the target descriptor.

## Insert Target Families

| Target family | Insert contract |
| --- | --- |
| Base table | Creates ordinary row versions under the table descriptor. |
| Updatable view | Inserts through the view only when the view has an admitted rewrite or `INSTEAD OF` trigger route. |
| Catalog projection | Admitted only for documented administrative surfaces. Ordinary metadata changes should use the specific lifecycle statement for the object. |
| Document projection | Inserts document rows or document-bearing rows only through a descriptor that defines key, body, missing/null behavior, and indexing rules. |
| Key-value projection | Inserts a descriptor-bound key/value row. Raw storage keys are not exposed as parser authority. |
| Graph projection | Inserts node or edge rows only through a graph descriptor that defines identity, labels, properties, and edge endpoints. |
| Vector projection | Inserts vector-bearing rows only when dimension, element type, metric profile, and exact-recheck policy bind successfully. |
| Time-series projection | Inserts event or sample rows only when timestamp, series identity, bucket, and retention descriptors bind successfully. |

If the target does not admit row insertion, the statement must fail before execution.

## Explicit Values

The `VALUES` form supplies one or more row constructors. SBsql supports the compact multi-row form:

```sql
insert into app.my_table (col1, col2, col3)
values
  (:val1, :val2, :val3),
  (:val4, :val5, :val6),
  (:val7, :val8, :val9);
```

The same form can be written on one line:

```sql
insert into app.my_table (col1, col2, col3)
values (:val1, :val2, :val3), (:val4, :val5, :val6);
```

Each parenthesized row constructor must produce the same number of values as the target column list. Values are assigned by ordinal: the first expression maps to the first target column, the second expression maps to the second target column, and so on.

```sql
insert into app.customer (
  customer_id,
  customer_name,
  account_state
)
values (
  :customer_id,
  :customer_name,
  'active'
)
returning customer_id, account_state;
```

Every supplied expression is bound against the target column descriptor. Parameter descriptors must be known before execution. If a value cannot be coerced safely to the target descriptor, the statement fails before row construction for that row.

Multiple row constructors are one statement. Constraint timing, trigger behavior, `RETURNING`, and error handling follow statement-level rules unless a policy-admitted bulk mode states otherwise.

```sql
insert into app.order_state_ref (state_code, display_order)
values
  ('open', 10),
  ('held', 20),
  ('closed', 30);
```

## DEFAULT And Generated Values

`DEFAULT` requests the target column default. Omitting a column has the same effect when the column has a default or generated policy. `DEFAULT VALUES` requests a row built entirely from defaults, generated values, identity values, and nullable columns.

```sql
insert into app.audit_event default values
returning event_id, created_at;
```

| Feature | Required behavior |
| --- | --- |
| Omitted nullable column | Stores `NULL` unless a default or generation policy supplies a value. |
| Omitted `NOT NULL` column | Requires a default, identity, or generated value; otherwise the statement fails. |
| `DEFAULT` keyword | Binds to the column default or generation policy. It is not a literal value. |
| Identity value | Allocated through the engine-managed identity or sequence policy admitted for the table. |
| Generated column | Computed by the engine route. `GENERATED ALWAYS` columns reject explicit non-default input. |
| Domain default/check | The domain descriptor participates in default construction and validation. |

Defaults and generated values are evaluated by the engine under the statement transaction. They are not supplied by client-side rendering.

## INSERT From Query

The query form inserts rows produced by a `SELECT`, `WITH`, `VALUES`, or other rowset-producing query statement.

```sql
insert into app.invoice_archive (
  invoice_id,
  customer_id,
  archived_at,
  invoice_total
)
select invoice_id,
       customer_id,
       current_timestamp,
       total_amount
from app.invoice
where invoice_state = 'closed'
  and closed_at < :archive_before;
```

The binder checks that the query result shape can be assigned to the target column list:

| Check | Required outcome |
| --- | --- |
| Column count | Query result count must equal the target column count. |
| Descriptor compatibility | Each query result descriptor must assign to the corresponding target descriptor. |
| Collation and charset | Text values must bind to compatible descriptor rules. |
| Structured values | Document, vector, graph, spatial, and protected descriptors must match their target policy. |
| Ordering | Insert order is not a durable row order unless a later query specifies `ORDER BY`. |
| Snapshot | Source rows are read through the transaction snapshot and target rows are written under the same transaction unless an admitted route states otherwise. |

If the query reads from the same table it writes to, the engine uses MGA visibility and conflict rules to avoid parser-side finality assumptions.

## Multimodel Row Insertion

`INSERT` can be used with multimodel data when the target exposes a rowset descriptor. The row remains the unit of transaction visibility, security, and cleanup.

Document-bearing row:

```sql
insert into app.product_profile (
  product_id,
  profile,
  tags
)
values (
  :product_id,
  json_object('name' value :name, 'state' value 'active'),
  json_array(:tag_a, :tag_b)
);
```

Vector-bearing row:

```sql
insert into app.product_embedding (
  product_id,
  embedding,
  model_name
)
values (
  :product_id,
  vector(:embedding_values),
  :model_name
);
```

Time-series row:

```sql
insert into app.metric_sample (
  series_id,
  sample_at,
  metric_name,
  metric_value
)
values (
  :series_id,
  :sample_at,
  'cpu.user',
  :metric_value
);
```

Structured values are descriptor-bound. A JSON path, vector payload, graph identity, or key-value payload is not accepted as raw storage authority.

## Updatable Views

An `INSERT` into a view is admitted only when SBsql can bind an unambiguous write route.

```sql
insert into app.open_orders (
  order_id,
  customer_id,
  total_amount
)
values (
  :order_id,
  :customer_id,
  :total_amount
);
```

The route may be a view rewrite or an `INSTEAD OF INSERT` trigger. The engine still verifies base table privileges, view privileges, row policy, constraints, and trigger behavior. If the view cannot be proven insertable, the statement fails before writing data.

## Constraints, Triggers, And Indexes

`INSERT` participates in the full table lifecycle contract.

| Mechanism | Insert behavior |
| --- | --- |
| `NOT NULL` | Refuses row construction when the final value is null. |
| `CHECK` | Evaluates the bound predicate against the new row version. |
| `UNIQUE` and primary key | Reserves and verifies key values through the admitted uniqueness/index route. |
| Foreign key | Verifies parent visibility and action timing under the transaction snapshot and policy. |
| Row policy | Applies insert policy and any `WITH CHECK` style predicate admitted by the policy surface. |
| Before trigger | May validate, derive, or adjust admitted values before final row construction. |
| After trigger | Runs after row construction and constraint timing rules admit the row event. |
| Index maintenance | Creates index entries as part of the same transaction. Index entries are evidence, not row authority. |
| Search/vector/document indexes | May receive maintenance entries, but final query delivery still requires exact row recheck. |

If any required check fails, the statement fails according to the active transaction policy. In autocommit mode the engine rolls back the statement transaction.

## RETURNING

`RETURNING` projects values from the inserted row after defaults, identity values, generated values, allowed trigger changes, and descriptor coercions have been applied.

```sql
insert into app.ticket (ticket_subject, ticket_body)
values (:subject, :body)
returning ticket_id, created_at, ticket_state;
```

`RETURNING` is a result projection. It does not prove commit finality. In an explicit transaction, the inserted rows are still uncommitted until `COMMIT` succeeds. In autocommit mode, the delivered success result must be consistent with the engine transaction outcome.

## Transaction And Visibility Rules

`INSERT` creates row versions owned by the active transaction.

| State | Visibility |
| --- | --- |
| Active transaction | The inserting transaction may see its own admitted row versions according to its isolation profile. |
| Concurrent transaction | Other transactions cannot see the inserted rows until commit visibility rules admit them. |
| Commit succeeds | New row versions become visible to later valid snapshots. |
| Rollback succeeds | Inserted row versions are non-visible and eligible for cleanup when safe. |
| Crash/restart | Recovery uses durable MGA transaction inventory and row-version metadata to classify the outcome. |

Cleanup and physical reclamation are separate engine decisions. A successful insert does not require immediate physical compaction or index cleanup.

## Diagnostics And Refusals

Common refusal classes include:

| Condition | Expected diagnostic class |
| --- | --- |
| Target not found or hidden by sandbox | Object resolution or sandbox denied. |
| Missing insert privilege | Authorization denied. |
| Target is not insertable | Unsupported or incompatible surface. |
| Column count mismatch | Descriptor mismatch. |
| Value cannot coerce to target descriptor | Type conversion refused. |
| `NOT NULL`, domain, or check failure | Constraint violation. |
| Duplicate key | Unique constraint violation. |
| Foreign key parent missing or invisible | Referential constraint violation. |
| Generated column supplied illegally | Generated value violation. |
| Storage or overflow allocation refused | Resource or storage refusal. |
| Recovery-required state | Operation fenced until recovery action completes. |

Diagnostics should identify the affected statement, target, column or constraint where disclosure policy permits it.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `INSERT` statement shape is recognized by SBsql. |
| Bind | Target, columns, source descriptors, parameters, defaults, and `RETURNING` descriptors resolve. |
| Authorize | Effective user or agent UUID may insert into the target and read any source query. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Construct | Defaults, generated values, identity values, coercions, and structured descriptors are applied. |
| Enforce | Constraints, triggers, policies, and index maintenance run through engine-owned routes. |
| Finalize | Row visibility follows MGA commit or rollback finality. |
| Render | `RETURNING` and diagnostics expose only authorized information. |
