# Index Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_index_lifecycle`


## Purpose

Indexes are access structures and optimizer evidence. They never replace row visibility or predicate recheck authority. An index definition must preserve key descriptors, collation, null treatment, uniqueness, included fields, partial predicates, expression dependencies, maintenance policy, and rebuild behavior.

## Complete Lifecycle Model

1. Define the index with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the index only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE INDEX`, `CREATE UNIQUE INDEX` | Creates the durable index UUID, key descriptor set, collation and null-order semantics, predicate or expression dependencies, and maintenance policy. |
| Alter | `ALTER INDEX` | Changes admitted index metadata such as enablement, maintenance policy, rebuild state, storage policy, or visibility to the optimizer. |
| Rename | `RENAME INDEX ... TO ...` | Changes the resolver name only; optimizer evidence, dependency edges, and maintenance state remain bound to the index UUID. |
| Comment | `COMMENT ON INDEX ... IS ...` | Stores authorized descriptive metadata on the index catalog row without changing optimizer authority. |
| Show | `SHOW INDEX ...`, `SHOW INDEXES` | Returns authorized index metadata, readiness, maintenance, and optimizer-eligibility projections. |
| Describe | `DESCRIBE INDEX ...` | Returns the index key descriptors, included columns, predicates, collation/null behavior, uniqueness, readiness, and dependency details visible to the caller. |
| Recreate | `RECREATE INDEX ...` | Replaces the index definition only through a dependency-aware DDL route; existing index evidence cannot be reused as new index truth without rebuild/admission evidence. |
| Drop | `DROP INDEX ...` | Retires the index and invalidates optimizer plans that depended on it. Row visibility and predicate authority remain table/MGA-owned. |

Indexes are evidence for access planning. `SHOW`, `DESCRIBE`, comments, and names never make an index authoritative for final row visibility; executor recheck and MGA visibility still own row truth.

## Index Access Methods And Families

ScratchBird exposes index families as descriptor-bound access methods. Some families are direct physical providers; others are semantic shapes over a provider. All index families are optimizer evidence only. They can produce candidate row locators, order evidence, selectivity estimates, or maintenance state, but final rows still require engine visibility, predicate, and security recheck.

| Family | Key Model | Persistent | Ordering | Uniqueness | Primary Use |
| --- | --- | --- | --- | --- | --- |
| `btree` | ordered key | yes | yes | optional | Default ordered equality, range, prefix, grouping, and order support. |
| `unique_btree` | ordered key | yes | yes | yes | Unique-key enforcement with duplicate preflight, transaction-bound reservation, and MGA finality. |
| `expression` | expression key | yes | yes | optional by backing provider | Indexes a deterministic expression descriptor over a backing access method, normally B-tree. |
| `partial` | predicate-filtered key | yes | yes when backing provider supports it | optional by backing provider | Indexes rows admitted by a stored predicate descriptor. |
| `covering` | covering payload | yes | yes when backing provider supports it | optional by backing provider | Stores non-key included fields for candidate/result acceleration; still requires freshness and visibility checks. |
| `hash` | hashed key | yes | no | optional | Equality lookup where ordering and range scans are not required. |
| `bitmap` | zone summary/candidate set | yes | no | no | Low-cardinality and multi-index candidate-set operations. |
| `brin_zone` | zone summary | yes | no | no | Block/range summary pruning. A positive match is never final row proof. |
| `bloom` | zone summary | yes | no | no | Probabilistic negative pruning. False positives require row predicate recheck. |
| `full_text` | token key | yes | no | no | Text search over tokenized content. |
| `gin` | token key | yes | no | no | PostgreSQL-style generalized inverted profile where admitted. |
| `inverted` | token key | yes | no | no | Native inverted-search segments. |
| `ngram` | token key | yes | no | no | N-gram text search and donor profiles that require n-gram tokenization. |
| `sparse_wand` | token key | yes | no | no | Sparse WAND-style ranked text candidate search. |
| `spatial` | spatial key | yes | no | no | Spatial search over geometry/geography descriptors. |
| `rtree` | spatial key | yes | no | no | R-tree rectangle/bounding-box candidate search. |
| `gist` | spatial key | yes | no | no | PostgreSQL GiST-style profile where admitted, backed by exact predicate recheck. |
| `spgist` | spatial key | yes | no | no | PostgreSQL SP-GiST-style profile where admitted, backed by exact predicate recheck. |
| `vector_exact` | vector key | yes | yes for distance/rank evidence | no | Exact vector candidate production with exact rerank proof. |
| `vector_hnsw` | vector key | yes | yes for approximate rank evidence | no | HNSW approximate vector candidate search; exact rerank is required before final delivery. |
| `vector_ivf` | vector key | yes | yes for approximate rank evidence | no | IVF approximate vector candidate search; training generation and exact rerank proof are required. |
| `columnar_zone` | zone summary | yes | no | no | Columnar or extent-level summary pruning. |
| `document_path` | token key | yes | no | no | JSON/document path candidate search. |
| `graph` | donor-defined | yes | no | no | Graph node, edge, and traversal lookup profiles. |
| `temporary_work` | donor-defined | memory only | no | no | Temporary work tables, hash builds, sort helpers, and execution workspace indexes. |
| `in_memory` | donor-defined | cold-start persisted metadata, memory-primary runtime | no | no | Memory-primary provider where policy admits it. |
| `donor_emulated` | donor-defined | donor-emulated | profile-dependent | profile-dependent | Compatibility mapping only. It must map to a native ScratchBird provider and cannot own finality or visibility. |
| `policy_blocked` | donor-defined | no | no | no | Declared refusal surface for index features blocked by policy or licensing. |

## Default SB B-Tree Contract

`btree` is the default index family when `CREATE INDEX` does not specify another method. A B-tree key descriptor contains one entry per key part.

| Descriptor Field | Default SBsql Behavior |
| --- | --- |
| Key order | Ascending unless the key part states `desc`. |
| Null placement | Explicit descriptor field. If omitted, the active parser profile supplies the default. SBsql should write `nulls first` or `nulls last` when order portability matters. |
| Collation | Text keys use the column or expression collation descriptor. B-tree byte order is not used as text order unless the collation descriptor says so. |
| Uniqueness | `CREATE UNIQUE INDEX` uses `unique_btree` semantics. Null-distinct behavior is a descriptor/profile setting, not a global assumption. |
| Expression keys | Expression text is parsed and bound to expression descriptors. Expression determinism and dependency edges must be admitted before index creation. |
| Partial predicate | Predicate text is parsed and bound to a predicate descriptor. The predicate is rechecked against base rows. |
| Included columns | Included values are covering payload, not key authority. Freshness and visibility must be rechecked. |
| Descending keys | A descending descriptor reverses ordered traversal for that key part. Donor profiles may require a physically distinct descending index if the donor engine exposes that distinction. |
| Plan use | B-tree can support equality lookup, ordered range scan, prefix scan, grouping/order evidence, and uniqueness checks. It cannot make final row visibility decisions. |

## Donor Index Profiles

Each donor parser has its own index profile. The parser should accept only index syntax that the donor engine exposes, then lower that syntax into ScratchBird descriptors that preserve the donor defaults. A Firebird client creating an index gets Firebird index defaults; a PostgreSQL client creating a B-tree gets PostgreSQL defaults; a MySQL client gets MySQL-family defaults.

| Donor Profile | Default Index Behavior |
| --- | --- |
| Firebird | `CREATE INDEX` maps to a Firebird-profile B-tree descriptor. Ascending indexes are the ordinary default. `DESCENDING` creates a descending key descriptor and, where Firebird compatibility requires it, a distinct descending index identity. Firebird expression or computed indexes preserve the Firebird expression descriptor. Firebird null handling, duplicate handling, collation, and selectivity metadata are profile settings, not SBsql defaults. Partial index support is admitted only for Firebird versions whose parser profile exposes it. |
| PostgreSQL | `USING btree` is the ordinary ordered profile when no method is stated. Per-key direction, `NULLS FIRST`/`NULLS LAST`, collation, operator class, expression indexes, partial predicates, included columns, and `NULLS NOT DISTINCT` are preserved where the PostgreSQL parser profile admits them. `USING hash`, `gin`, `gist`, `spgist`, `brin`, and compatible vector/full-text methods map to their matching ScratchBird families. |
| MySQL and MariaDB | Ordinary indexes map to B-tree descriptors with MySQL-family charset/collation and prefix-length behavior. Unique indexes preserve the MySQL-family multiple-NULL behavior where the profile requires it. `FULLTEXT` maps to full-text/inverted providers, and `SPATIAL` maps to spatial/R-tree descriptors where the donor version and storage profile admit them. |
| SQLite | Ordinary indexes map to B-tree descriptors with SQLite collation and affinity behavior. Unique indexes preserve SQLite NULL-distinct behavior. Expression indexes and partial indexes are admitted for SQLite profiles that support them. |
| DuckDB and analytic profiles | Zone, columnar, vector, and ordered descriptors are used according to the donor feature being emulated. Candidate evidence and result ordering remain subject to executor recheck. |
| ClickHouse and columnar profiles | Primary/sorting-key and skip-index features map to ordered or zone-summary descriptors where the parser profile admits them. They are pruning and order evidence, not row-finality authority. |

## Index Option Descriptors

| Option Class | Examples | Contract |
| --- | --- | --- |
| Access method | `using btree`, `using hash`, `using gin`, `using gist`, `using vector_hnsw` | Selects an index family admitted by the active parser profile. |
| Key direction | `asc`, `desc` | Stored per key part and used by optimizer ordering evidence. |
| Null handling | `nulls first`, `nulls last`, `nulls distinct`, `nulls not distinct` | Stored in descriptor/profile fields. Omitted values come from the active parser profile. |
| Collation and operator class | `collate`, PostgreSQL-style operator class where admitted | Bound to descriptor UUIDs/resources; names are not runtime authority. |
| Predicate | `where <predicate>` | Creates a partial-index predicate descriptor. Predicate recheck remains mandatory. |
| Expression | expression key parts | Creates expression dependency edges and requires deterministic expression proof. |
| Included payload | `include (...)` | Covering payload only; not key authority. |
| Prefix length | MySQL-family `column(length)` | Stored as a donor-profile key truncation descriptor. Predicate and equality recheck remain mandatory. |
| Vector metric/options | metric, dimension, `ef`, list count, quantization profile | Must match vector descriptor and provider proof. Approximate providers require exact rerank. |
| Storage/maintenance | fill policy, rebuild policy, residency, online/offline rebuild | Admitted by engine policy and surfaced through `SHOW`/`DESCRIBE` readiness fields. |

## Practical Lifecycle Example

```sql
create index app.orders_customer_submitted_idx
  on app.orders (customer_id, submitted_at desc);

create unique index app.orders_open_unique_idx
  on app.orders (customer_id, order_id)
  where order_state = 'open';

comment on index app.orders_customer_submitted_idx is 'Customer lookup ordered by submission time';
show index app.orders_customer_submitted_idx;
describe index app.orders_customer_submitted_idx;
rename index app.orders_customer_submitted_idx to orders_customer_submitted_desc_idx;
drop index app.orders_customer_submitted_desc_idx;
```

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL must be transactionally visible and rollback-safe.
- Donor parser variants may render donor syntax, but catalog authority remains ScratchBird catalog authority.
- Support and diagnostic surfaces may inspect the object only through authorized projections.

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Statement shape is recognized by the active parser profile. |
| Bind | Names, UUIDs, descriptors, options, and dependencies resolve exactly. |
| Authorize | The effective user or agent UUID is allowed to mutate the object. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Commit | Catalog mutation becomes visible only through MGA transaction finality. |
| Invalidate | Dependent caches, metadata, plans, and projections are refreshed or refused. |
