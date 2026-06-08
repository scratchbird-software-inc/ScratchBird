# Table Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_table_lifecycle`


## Purpose

Tables store descriptor-bound row versions under MGA. A table definition must describe identity, resolver scope, columns, descriptors or domains, defaults, generated expressions, constraints, indexes implied by constraints, storage policy, visibility policy, security policy, and dependency edges.

Altering a table is a catalog mutation. Dropping a table retires object identity and invalidates dependent plans, metadata projections, parser metadata, bridge metadata, and cached descriptors. SQL text is never table authority by itself; the bound SBLR request and catalog UUIDs are the authority the engine verifies.

Related reference pages:

- [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md)
- [Type System Overview](../data_types/type_system_overview.md)
- [Document, Graph, Vector, And Multimodel Types](../data_types/document_graph_vector_and_multimodel_types.md)
- [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md)
- [Index Lifecycle](index.md)
- [Policy, Mask, And Row-Level Security Lifecycle](policy_mask_and_rls.md)
- [Trigger Lifecycle](trigger.md)
- [View Lifecycle](view.md)
- [SELECT Statement](select.md)

## Complete Lifecycle Model

1. Define the table with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and catalog projections that rely on the changed object.
6. Retire or drop the table only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Table Identity And Resolver Scope

A table has at least two identities:

| Identity | Meaning |
| --- | --- |
| Resolver name | The user-visible name used in SQL text, such as `app.orders` or `"Order"`. It is input to name resolution. |
| Durable UUID | The catalog identity used by dependencies, privileges, plans, constraints, storage metadata, and row versions. |

Renaming a table changes the resolver name. It does not change the durable UUID, row versions, privileges, constraint ownership, index ownership, or dependency identity.

Table names resolve through the active schema rules described in [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md). An unqualified table name is resolved relative to the current schema, default schema, authorized schema root, or SBsql search path admitted for the session.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE TABLE` | Creates the durable table UUID, column descriptors, constraints, storage policy bindings, dependency edges, initial security metadata, and metadata visibility rules. |
| Alter | `ALTER TABLE` | Mutates table metadata under the owning transaction. Descriptor changes, data rewrites, dependency rewrites, and revalidation operations must be admitted explicitly. |
| Rename | `RENAME TABLE ... TO ...`, `ALTER TABLE ... RENAME TO ...` | Changes the resolver name only. Durable identity, privileges, dependencies, constraints, indexes, and row versions remain bound to the table UUID. |
| Rename column | `ALTER TABLE ... RENAME COLUMN ... TO ...` | Changes the column resolver name while preserving the column UUID or descriptor identity used by dependencies where that preservation is admitted. |
| Comment | `COMMENT ON TABLE ... IS ...`, `COMMENT ON COLUMN ... IS ...` | Stores authorized descriptive metadata on the catalog row. A `NULL` or empty-comment policy, when admitted, removes or clears the metadata without changing identity. |
| Show | `SHOW TABLE ...`, `SHOW TABLES` | Returns authorized table metadata projections. It must not disclose hidden objects through names, counts, diagnostics, or timing-sensitive detail. |
| Describe | `DESCRIBE TABLE ...` | Returns the authorized column, descriptor, constraint, policy, storage, index, dependency, and relation metadata view for one table. |
| Recreate | `RECREATE TABLE ...` | Performs the admitted drop-and-create lifecycle as one DDL request. It must fail closed if dependencies, privileges, retained data, recovery state, or sandbox rules make replacement unsafe. |
| Drop | `DROP TABLE ... [RESTRICT | CASCADE]` | Retires or removes the table only through dependency-aware, transactionally visible catalog mutation. `RESTRICT` refuses remaining dependents; `CASCADE` requires explicit admitted dependency handling. |

`SHOW` and `DESCRIBE` are inspection surfaces, not shortcuts around authorization. `COMMENT ON`, `RENAME`, `RECREATE`, `ALTER`, and `DROP` are catalog mutations and therefore require the same bind, SBLR admission, security, and MGA commit rules as `CREATE`.

## Create Table Contract

`CREATE TABLE` defines a relation descriptor and its initial catalog dependencies.

```sql
create table app.orders (
  order_id uuid not null,
  customer_id uuid not null,
  submitted_at timestamp with time zone not null default current_timestamp,
  total_amount decimal(18, 2) not null,
  order_state varchar(32) not null default 'open',
  primary key (order_id),
  check (total_amount >= 0)
);
```

A complete table definition includes these parts:

| Part | Required behavior |
| --- | --- |
| Table name | Resolves to a schema branch that the effective user or agent may create in. |
| Table UUID | Created by the catalog layer; not supplied by user SQL text except through admitted administrative restore or migration routes. |
| Column list | Defines column resolver names, ordinals, descriptor identity, nullability, defaults, generation, collation, charset, storage, and dependency metadata. |
| Type descriptors | Bind to canonical descriptors or domains. SBsql spellings lower through SBsql binding rules. |
| Constraints | Define validation and dependency rules. They are catalog objects and may imply index descriptors. |
| Storage policy | Defines ordinary row storage, overflow storage, filespace policy, compression/encryption policy where admitted, and cleanup behavior. |
| Visibility policy | Defines row-level access, masks, projection behavior, and catalog visibility where admitted. |
| Dependency graph | Records dependencies on domains, collations, functions, sequences, parent tables, policies, triggers, indexes, and UDR packages. |

SBsql may accept contextual shorthand, but binding must expand that syntax into explicit descriptors and policy choices before the engine admits the request.

## Column Definition Semantics

A column is a named value slot in a table descriptor. A column definition is not only a type spelling; it also controls null handling, default construction, generated values, collation, security behavior, and storage.

| Column property | Contract |
| --- | --- |
| Name | Resolver label scoped to the table. Case folding and quoting follow SBsql identifier rules. |
| Ordinal | Stable table descriptor position used for metadata projection and rendering. |
| Descriptor | Canonical type descriptor or domain descriptor. See [Type System Overview](../data_types/type_system_overview.md). |
| Nullability | `NOT NULL` is enforced when a row version is constructed or modified. It is not deferred to client code. |
| Default | Bound expression used when an insert omits the column. The expression must be deterministic enough for the admitted default class. |
| Generated expression | Computed expression for virtual or stored generated columns where SBsql admits that feature. |
| Identity or sequence binding | Describes automatic value generation and dependency on a sequence/generator resource where admitted. |
| Character set and collation | Descriptor-owned text comparison, ordering, and rendering policy. |
| Storage class | Inline, overflow, document, vector, spatial, or large-object storage behavior where admitted. |
| Security projection | Column-level grant, mask, protected-value, or redaction behavior where policy admits it. |
| Comment | Authorized descriptive metadata. It does not change execution behavior. |

Example:

```sql
create domain app.email_text as varchar(320)
  check (position('@' in value) > 1);

create table app.account (
  account_id uuid not null,
  email app.email_text not null,
  display_name varchar(120),
  created_at timestamp with time zone not null default current_timestamp,
  updated_at timestamp with time zone,
  primary key (account_id),
  unique (email)
);
```

## Data Types And Descriptor Families

Table columns may use scalar, structured, protected, and multimodel descriptor families. The exact size, range, coercion, and rendering rules are documented in the data type pages.

| Descriptor family | Examples | Table design notes |
| --- | --- | --- |
| Integer and numeric | `int32`, `int64`, `decimal(18,2)` | Choose exact numeric descriptors for keys, counters, money, and audit values. Use explicit precision and scale for decimals. |
| Floating point | `float32`, `float64` | Use for approximate scientific or ranking values. Do not use where exact decimal equality is required. |
| Text | `char`, `varchar`, `text` | Collation and charset are descriptor-owned and affect comparisons, indexes, and ordering. |
| Binary and UUID | `binary`, `varbinary`, `blob`, `uuid` | UUIDs are preferred for durable object identity exposed to applications. |
| Temporal | `date`, `time`, `timestamp`, `timestamp with time zone`, intervals where admitted | Time-zone, precision, and calendar behavior are descriptor-owned. |
| Boolean | `boolean` | Stores truth values with descriptor-owned null behavior. |
| Domain | `app.email_text` | Centralizes descriptor and validation policy. |
| JSON/document | `json`, `jsonb`, `document` | Stores document payloads with path, missing/null, and index recheck semantics. |
| Vector | `vector<float32,768>`, `vector<int8,1536>` | Stores fixed-dimension embeddings with metric and exact-recheck requirements. |
| Graph | `node`, `edge`, `path`, `graph` where admitted | Stores graph identities or traversal payloads when a graph surface exposes them as table columns. |
| Spatial | `geometry`, `geography` where admitted | Stores spatial payloads with SRID/profile metadata and exact predicate recheck. |
| Protected material | secret references, encrypted/protected descriptors where admitted | Raw secrets must not be embedded in ordinary parser packets or support projections. |

Portable SBsql should spell descriptors explicitly. The resulting catalog descriptor must be unambiguous.

## Constraints

Constraints are catalog objects with their own identity, dependency edges, enforcement timing, and diagnostic behavior.

| Constraint | Behavior |
| --- | --- |
| `PRIMARY KEY` | Defines table row identity for the application model, implies `NOT NULL` on key columns, and usually creates or binds a uniqueness index descriptor. It is not the same as the internal table UUID. |
| `UNIQUE` | Enforces uniqueness according to the SBsql null and collation rules bound into the constraint descriptor. |
| `FOREIGN KEY` | Links child row descriptors to parent key descriptors. Parent lookup, action rules, and timing are enforced under MGA transaction visibility. |
| `CHECK` | Stores a bound predicate descriptor. It must evaluate against the row version being constructed or modified. |
| `NOT NULL` | Enforced as part of row construction and update. It may be represented as column metadata or as a constraint projection. |
| Exclusion or specialized constraints | Admitted only when SBsql and the engine route define their descriptor, index, and recheck rules. |

Foreign key actions include SBsql forms such as `RESTRICT`, `NO ACTION`, `CASCADE`, `SET NULL`, and `SET DEFAULT` where admitted. Unsupported action spellings must fail before execution.

Example:

```sql
create table app.order_item (
  order_item_id uuid not null,
  order_id uuid not null,
  sku varchar(64) not null,
  quantity int32 not null,
  unit_price decimal(18, 2) not null,
  primary key (order_item_id),
  foreign key (order_id) references app.orders (order_id) on delete cascade,
  check (quantity > 0),
  check (unit_price >= 0)
);
```

## Generated Values, Defaults, And Identity

Defaults and generated values are evaluated by the engine route admitted for the table descriptor. They are not client-side conveniences.

| Feature | Contract |
| --- | --- |
| Default expression | Used when an insert omits the column or explicitly requests the default. It must bind to the column descriptor. |
| Identity column | Binds the column to an engine-managed sequence/generator policy where admitted. |
| Generated column | Binds a computed expression to a column descriptor. Stored generated columns write materialized values; virtual generated columns compute values at read time where admitted. |
| Identity or sequence shorthand | Lowered to the ScratchBird descriptor and sequence/generator surface for the table definition. |

Example:

```sql
create table app.invoice (
  invoice_id uuid not null,
  invoice_no int64 generated by default as identity,
  subtotal decimal(18, 2) not null,
  tax_amount decimal(18, 2) not null default 0,
  total_amount decimal(18, 2) generated always as (subtotal + tax_amount) stored,
  primary key (invoice_id),
  unique (invoice_no)
);
```

If SBsql does not admit a particular identity or generated-column form, the statement must return an unsupported or incompatible-surface diagnostic before changing catalog state.

## Storage And Large Values

Tables use row-page storage for ordinary row versions. Large values, document payloads, blobs, long text, vectors, spatial payloads, and other oversized descriptors may use overflow storage when the descriptor and storage policy admit it.

| Storage concern | Required behavior |
| --- | --- |
| Row version | Insert, update, and delete create versioned row state governed by MGA. |
| Inline data | Small fixed and variable fields can be stored inline according to the row descriptor. |
| Overflow values | Large fields must remain reachable from visible row versions and reclaimable after cleanup. |
| Document payloads | Path operations must preserve missing/null semantics and descriptor identity. |
| Vector payloads | Dimension, element profile, metric, and exact-recheck metadata are descriptor-owned. |
| Filespace policy | The table may bind to an admitted storage policy or filespace. User SQL does not directly own operating system file layout. |
| Cleanup | Cleanup may reclaim retired row and overflow versions only after transaction visibility rules prove they are unreachable. |

Physical page-copy backup, repair, verification, and low-level file manipulation are not table DDL features. Administrative maintenance surfaces are separate from table lifecycle syntax.

## NoSQL And Multimodel Table Design

ScratchBird can model SQL and NoSQL data in one catalog, but table design should make the row boundary explicit. Use `CREATE TABLE` when the data needs descriptor-owned rows, constraints, grants, joins, row-level policy, indexes, and MGA visibility. Use dedicated multimodel statements when the operation is primarily a document, graph, vector, search, time-series, or key-value command rather than a table lifecycle change.

| Design pattern | Table shape | Typical use |
| --- | --- | --- |
| Document-in-row | Ordinary key columns plus `json`, `jsonb`, or `document` columns | Application records with flexible attributes, audit envelopes, and imported JSON documents. |
| Key-value projection | Key column, value descriptor column, value payload column, version/TTL columns | Cache-like or map-like data that still needs SQL visibility, grants, joins, and auditing. |
| Graph projection | Node table, edge table, or table columns containing `node`, `edge`, or `path` descriptors where admitted | Graph data that must join with relational tables or be inspected through SQL. |
| Vector search table | Entity key plus `vector<T,n>` embedding column and optional metadata columns | Embedding search with relational filters and exact recheck. |
| Search-backed table | Table plus search index metadata or search-hit projection | Full-text search joined back to authoritative rows. |
| Time-series table | Series identity, timestamp/bucket columns, value columns, tags or document metadata | Metrics, events, sampled values, and time-window queries. |
| Hybrid row | Relational keys and constraints plus document, vector, search, or graph descriptors | Product profiles, event records, observability data, recommendations, and migration workloads. |

Example hybrid table:

```sql
create table app.product_profile (
  product_id uuid not null,
  sku varchar(64) not null,
  profile jsonb not null,
  embedding vector<float32,768>,
  tags jsonb,
  created_at timestamp with time zone not null default current_timestamp,
  primary key (product_id),
  unique (sku),
  check (json_exists(profile, '$.name'))
);
```

The row remains the authority for visibility and security. A JSON path index, vector index, search index, or graph adjacency index can supply candidate evidence, but the executor must still recheck row visibility, authorization, descriptor compatibility, and predicate truth before returning or modifying rows.

Example mixed SQL and NoSQL query over the table:

```sql
select p.product_id,
       p.sku,
       json_value(p.profile, '$.name') as product_name,
       l2_distance(p.embedding, vector(:query_embedding)) as distance
from app.product_profile p
where json_value(p.profile, '$.status') = 'active'
order by distance, p.product_id
limit 20;
```

Table DDL should not hide an unbounded or untyped NoSQL store behind a generic blob unless that is truly the intended application contract. Prefer explicit descriptors, generated projections, check constraints, and indexes for fields that participate in predicates, joins, ordering, or authorization.

## Index And Optimizer Relationship

Tables can have explicit indexes and indexes implied by constraints. Indexes accelerate access and enforce some constraints, but they are not final row authority.

| Relationship | Contract |
| --- | --- |
| Primary key index | May be created or bound as part of primary key admission. Its null, uniqueness, and collation behavior follow the table descriptor. |
| Unique index | May enforce a unique constraint. Null treatment must match the constraint descriptor. |
| Foreign key support index | May be required or recommended for parent/child lookups; enforcement still uses engine constraint authority. |
| Document path index | Produces candidates for JSON/document predicates. Missing/null and type behavior must be rechecked. |
| Vector index | Produces candidates or rankings. Exact metric and row visibility must be rechecked. |
| Search index | Produces score or match evidence. The joined table row remains authoritative. |
| Graph/spatial index | Produces traversal, adjacency, or spatial candidates. Exact predicate and visibility recheck remain mandatory. |

See [Index Lifecycle](index.md) for access method families, null ordering, uniqueness, collation, and evidence rules.

## Alter Table Contract

`ALTER TABLE` changes table metadata through an admitted catalog mutation. The engine must verify that the change is compatible with existing data, dependent objects, transaction state, security policy, and recovery state.

Common alter operations:

| Operation | Required checks |
| --- | --- |
| Add column | Descriptor, default, generated expression, nullability, storage, dependency, and existing-row materialization policy. |
| Drop column | Dependency checks for views, indexes, constraints, triggers, policies, grants, generated expressions, and catalog projections. |
| Rename column | Dependency and compatibility checks; durable identity should be preserved where the operation admits preservation. |
| Alter type/domain | Explicit conversion path, lossiness policy, collation/charset compatibility, index rebuild need, and existing-row validation. |
| Set/drop default | Expression binding and dependency refresh. Dropping a default does not rewrite existing rows. |
| Set/drop `NOT NULL` | Existing visible rows must satisfy `NOT NULL` before it is added. Dropping `NOT NULL` must preserve dependent constraint semantics. |
| Add/drop constraint | Full constraint binding, validation, dependency updates, optional index creation/drop, and rollback safety. |
| Validate constraint | Scans admitted row versions under a consistent transaction view and records validation state only on commit. |
| Attach policy | Binds row-level security, masks, or visibility policy to the table where admitted. |
| Change storage policy | Requires data movement, rebuild, or future-write-only admission depending on the policy. |

Examples:

```sql
alter table app.orders
  add column fulfilled_at timestamp with time zone;

alter table app.orders
  add constraint orders_total_nonnegative
  check (total_amount >= 0);

alter table app.orders
  alter column order_state set default 'open';

alter table app.orders
  rename column submitted_at to accepted_at;
```

An alter that requires a data rewrite must be atomic with respect to MGA visibility. Readers must see either the old committed table descriptor or the new committed descriptor, never a half-rewritten state.

## Drop, Recreate, Rename, Comment, Show, And Describe

Table lifecycle includes more than create and alter.

### Drop

`DROP TABLE` removes or retires the table descriptor through a transactionally visible catalog mutation.

```sql
drop table app.orders restrict;
```

`RESTRICT` refuses the drop when dependent objects remain. `CASCADE`, where admitted, must enumerate and authorize every dependent object it will remove or rewrite. A drop must not silently remove objects outside the user's authorized schema branch.

### Recreate

`RECREATE TABLE` is an explicit replacement operation.

```sql
recreate table app.orders_archive (
  order_id uuid not null primary key,
  archived_at timestamp with time zone not null
);
```

It is not a blind overwrite. The operation must pass the same dependency, privilege, data-retention, sandbox, and recovery checks that would apply to a separate drop and create.

### Rename

```sql
rename table app.orders to orders_live;
```

Renaming changes name resolution. It must invalidate dependent name caches while preserving durable identity and dependency UUIDs.

### Comment

```sql
comment on table app.orders is 'Orders accepted by the application workflow';
comment on column app.orders.total_amount is 'Order total in the account currency';
```

Comments are metadata. They must be authorized, transactional, and visible only through permitted metadata projections.

### Show And Describe

```sql
show tables in schema app;
show table app.orders;
describe table app.orders;
```

`SHOW` returns lists or compact object projections. `DESCRIBE` returns a detailed projection for one object. Both must apply object visibility, catalog projection, schema-root, and redaction rules.

## Transaction And Visibility Semantics

Table DDL and table DML both participate in MGA, but at different catalog and row layers.

| Action | MGA behavior |
| --- | --- |
| `CREATE TABLE` | Table descriptor becomes visible only after the creating transaction commits. |
| `ALTER TABLE` | Descriptor mutation becomes visible only on commit. Existing readers keep a valid descriptor view or are fenced according to transaction rules. |
| `DROP TABLE` | Object retirement becomes visible only on commit. Rollback restores the previous committed catalog state. |
| `INSERT` | Creates a row version visible according to transaction isolation after commit. |
| `UPDATE` | Creates a new row version and retires the previous version according to MGA rules. |
| `DELETE` | Retires the visible row version; cleanup can reclaim only when safe. |
| Constraint validation | Must use a transactionally consistent view and commit the validation result atomically. |

DDL cannot bypass recovery fencing. If the database is in recovery-required or uncertain state, table mutations must fail closed until the engine admits the state transition.

## Security, Sandboxing, And Catalog Projection

Tables are protected by catalog security, schema-root rules, object visibility policy, and optional row/column policies.

| Rule | Behavior |
| --- | --- |
| Schema root | A restricted session sees only the authorized schema branch unless authorized catalog views project more. |
| Object grants | `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `REFERENCES`, `TRIGGER`, and DDL privileges are distinct where SBsql exposes them. |
| Row policy | Row-level security policies filter or deny rows according to materialized authorization. |
| Column masks | Masks and protected-value policies control projection of sensitive columns. |
| Catalog views | Compatibility catalog tables may be granted authority to inspect outside the sandbox and project safe results to the user. |
| Support diagnostics | Diagnostics must identify refusals without leaking hidden object names or secret values. |

SBsql may expose broader administrative or tree-wide views only to users or agents with explicit authorization.

## Practical Lifecycle Example

```sql
create table app.orders (
  order_id uuid not null,
  customer_id uuid not null,
  submitted_at timestamp with time zone not null,
  total_amount decimal(18, 2) not null,
  order_state varchar(32) not null,
  primary key (order_id)
);

alter table app.orders
  add column fulfilled_at timestamp with time zone;

comment on table app.orders is 'Orders accepted by the application workflow';

show table app.orders;
describe table app.orders;
rename table app.orders to orders_live;
recreate table app.orders_archive (
  order_id uuid not null primary key,
  archived_at timestamp with time zone not null
);
drop table app.orders_live restrict;
```

## Failure Modes

| Failure | Required diagnostic behavior |
| --- | --- |
| Ambiguous table name | Refuse binding before execution and identify the ambiguity through an authorized diagnostic. |
| Unauthorized schema | Refuse without revealing hidden schema contents. |
| Unsupported table option | Return unsupported or incompatible-surface diagnostics; do not ignore the clause silently. |
| Unsafe type conversion | Refuse unless an explicit admitted conversion and rewrite path exists. |
| Existing data violates new constraint | Refuse the constraint addition or validation and report the authorized failing condition. |
| Dependency remains on drop | `RESTRICT` refuses; `CASCADE` requires explicit dependency handling and authorization. |
| Sandbox escape | Refuse and return a sandbox-denied diagnostic. |
| Recovery fenced | Refuse catalog mutation until recovery state admits writes. |
| Overflow or storage policy refused | Refuse before creating partial row or catalog state. |
| Secret or protected material exposure | Redact or refuse according to policy; never expose raw secrets through table metadata. |

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL must be transactionally visible and rollback-safe.
- NoSQL and multimodel table designs are still descriptor-bound row designs; dedicated multimodel commands are separate surfaces.
- Support and diagnostic surfaces may inspect the object only through authorized projections.

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Statement shape is recognized by SBsql. |
| Bind | Names, UUIDs, descriptors, options, and dependencies resolve exactly. |
| Authorize | The effective user or agent UUID is allowed to mutate the object. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Validate descriptors | Column types, domains, defaults, generated expressions, collations, and storage choices are explicit and compatible. |
| Validate data | Existing visible rows satisfy new constraints, nullability, and conversion rules before metadata is committed. |
| Validate multimodel fields | Document paths, vector dimensions, graph identities, spatial descriptors, search metadata, and key-value projections have descriptor-owned semantics. |
| Commit | Catalog mutation becomes visible only through MGA transaction finality. |
| Invalidate | Dependent caches, metadata, plans, and projections are refreshed or refused. |
| Recheck | Index, search, vector, graph, spatial, and document evidence is rechecked against row visibility, security, descriptors, and predicates. |
| Rollback | A failed or rolled-back DDL operation leaves no partial descriptor, row, overflow, index, policy, or dependency state. |
