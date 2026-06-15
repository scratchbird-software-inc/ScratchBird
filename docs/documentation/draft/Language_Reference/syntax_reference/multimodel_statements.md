# Multimodel Statements

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_multimodel`

Related pages: [SELECT Statement](select.md), [FROM And Table Expressions](from.md), [Table Lifecycle](table.md), [Index Lifecycle](index.md), [COPY Streaming Import And Export](copy.md), [Transaction Control](transaction_control.md), [Security And Privileges](security_and_privilege_statements.md), [Refusal Vectors](refusal_vectors.md), [Document, Graph, Vector, And Multimodel Types](../data_types/document_graph_vector_and_multimodel_types.md), [JSON Functions](../functional_reference/sb_json.md), [Vector Functions](../functional_reference/sb_vector.md), and [Spatial Functions](../functional_reference/sb_spatial.md).

## Purpose

SBsql includes dedicated statement families for document, key-value, graph, vector, search, and time-series workloads. These are first-class SBsql surfaces, not separate execution authorities. They parse through SBsql, bind to descriptors and catalog UUIDs, lower to SBLR, execute under the caller's transaction and security context, and must pass the same visibility, authorization, recovery, and diagnostic rules as relational SQL.

Use a dedicated multimodel statement when the operation is primarily about a multimodel command: getting a document by key, patching a document path, scanning key ranges, traversing a graph, ranking vector candidates, running a search, or querying time buckets. Use `SELECT` when the operation is primarily a tabular projection from rowset sources.

The same data can often be reached both ways:

```sql
select d.document_key,
       json_value(d.body, '$.status') as status
from app.document_store d
where json_exists(d.body, '$.status');
```

```sql
document query app.document_store
where path '$.status' exists
return key, path '$.status' as status;
```

Both forms still bind descriptors, enforce authorization, and execute under MGA transaction rules.

## Statement Families

```ebnf
multi_model_op_stmt ::=
      document_op_stmt
    | keyvalue_op_stmt
    | graph_op_stmt
    | vector_op_stmt
    | fulltext_search_query
    | time_series_op_stmt ;
```

SBsql is context sensitive. `DOCUMENT`, `KV`, `GRAPH`, `VECTOR`, `SEARCH`, and `TIMESERIES` are command words inside this statement family. They should not be treated as globally reserved identifiers outside this context.

| Family | Primary use | Typical result shape |
| --- | --- | --- |
| `DOCUMENT` | Document key lookup, document put, patch, remove, query, path projection, and document validation. | One document, one mutation result, or a rowset of key/path/value rows. |
| `KV` | Key-value get, put, delete, increment, TTL, range scan, set/list/map operations, and stream-like key scans. | Value result, mutation result, counter result, or key/value rowset. |
| `GRAPH` | Node, edge, path, traversal, pattern match, shortest path, property mutation, and graph constraint checks. | Node/edge/path rowset or mutation result. |
| `VECTOR` | Vector insert/update, exact or indexed candidate search, rerank, metric inspection, and vector index maintenance requests. | Candidate rowset with distance/score and recheck status. |
| `SEARCH` | Text or structured search, scoring, snippet generation, field filters, and search index refresh requests. | Hit rowset with score, snippet, matched fields, and object identity. |
| `TIMESERIES` | Time-window reads, bucket aggregation, interpolation, gap handling, downsample, retention, and sample mutation. | Sample rowset, bucket rowset, aggregate rowset, or mutation result. |

## Shared Execution Contract

Every multimodel statement follows the same lifecycle:

1. Parse the command family and action.
2. Bind object names to catalog UUIDs under schema-root and sandbox rules.
3. Bind key, path, metric, pattern, timestamp, value, and option expressions to descriptors.
4. Authorize the effective user or agent UUID for the target and operation.
5. Admit an SBLR operation family and result descriptor.
6. Execute under the current transaction and MGA snapshot.
7. Recheck candidate evidence against descriptors, predicates, visibility, and security.
8. Return a typed rowset, typed value, mutation result, or canonical message vector.

Candidate evidence is never final authority. A document-path index, graph adjacency index, vector index, search index, time index, or key-range hint can accelerate work, but final result membership and mutation authority require engine recheck.

## Common Syntax Elements

```ebnf
multimodel_target ::=
    qualified_name ;

multimodel_key ::=
    expression ;

path_expression ::=
      string_literal
    | parameter_ref
    | path_constructor ;

return_clause ::=
    RETURN return_item ("," return_item)* ;

return_item ::=
      identifier
    | expression AS? identifier? ;

multimodel_where_clause ::=
    WHERE predicate ;

statement_option_list ::=
    WITH option ("," option)* ;
```

Names, keys, paths, metrics, query text, patterns, timestamps, and options are parser input only. Binding turns them into descriptor-aware requests. Runtime execution must not treat a raw text path or raw query string as authority.

## Document Statements

Document statements operate on descriptor-bound document containers or document-bearing rowsets.

```ebnf
document_op_stmt ::=
    DOCUMENT document_action document_target document_payload? return_clause? statement_option_list? ;

document_action ::=
      GET
    | PUT
    | PATCH
    | DELETE
    | QUERY
    | VALIDATE ;

document_target ::=
    multimodel_target ;
```

Get a document by key:

```sql
document get app.document_store
key :document_id
return key, document;
```

Insert or replace a document:

```sql
document put app.document_store
key :document_id
value :document_body
with require_valid_profile true;
```

Patch document paths:

```sql
document patch app.document_store
key :document_id
set path '$.status' = 'approved',
    path '$.reviewed_at' = current_timestamp
remove path '$.draft_reason'
return key, version;
```

Query document paths:

```sql
document query app.document_store
where path '$.customer.id' = :customer_id
  and path '$.status' = 'open'
return key,
       path '$.customer.id' as customer_id,
       path '$.total' as order_total;
```

Document semantics:

| Concern | Rule |
| --- | --- |
| Missing versus null | A missing path and a JSON null value remain distinct when the bound operation exposes that distinction. |
| Path binding | Path text is bound to a descriptor-aware path operation before execution. |
| Validation | Profile, schema, required field, type, and protected-material checks occur before commit. |
| Mutation | Puts, patches, and deletes are transactional and rollback-safe. |
| Indexes | Path indexes produce candidate evidence and require final recheck. |
| Result shape | `RETURN` defines a typed result projection; omitted return clauses use the operation default. |

## Key-Value Statements

Key-value statements operate on descriptor-bound key spaces. Keys, values, versions, expiration, and optional collection behavior are explicit descriptors.

```ebnf
keyvalue_op_stmt ::=
    KV kv_action kv_target kv_payload? return_clause? statement_option_list? ;

kv_action ::=
      GET
    | PUT
    | DELETE
    | INCREMENT
    | EXPIRE
    | SCAN
    | LIST
    | SET
    | MAP ;

kv_target ::=
    multimodel_target ;
```

Get and put values:

```sql
kv get app.session_cache
key :session_key
return key, value, expires_at;
```

```sql
kv put app.session_cache
key :session_key
value :session_payload
ttl interval '30 minutes'
return key, version, expires_at;
```

Range scan:

```sql
kv scan app.session_cache
from key 'session:'
to key 'session;'
limit 500
return key, value, version;
```

Atomic counter:

```sql
kv increment app.usage_counter
key :counter_key
by 1
return key, value;
```

Key-value semantics:

| Concern | Rule |
| --- | --- |
| Key descriptor | Keys are typed values, not raw byte authority. |
| Value descriptor | Values bind to the declared descriptor or protected descriptor policy. |
| Conditional mutation | Version, existence, compare, and lease predicates bind before mutation. |
| TTL | Expiration is policy and descriptor controlled; cleanup timing does not change visible transaction semantics. |
| Range scan | Start/end keys, direction, and limit are descriptor-bound. |
| Collection operations | Lists, sets, maps, counters, streams, and geospatial-like projections require explicit descriptor support. |

## Graph Statements

Graph statements operate on graph descriptors, node descriptors, edge descriptors, path descriptors, and property descriptors.

```ebnf
graph_op_stmt ::=
    GRAPH graph_action graph_target graph_payload? return_clause? statement_option_list? ;

graph_action ::=
      MATCH
    | TRAVERSE
    | SHORTEST_PATH
    | CREATE_NODE
    | CREATE_EDGE
    | UPDATE_NODE
    | UPDATE_EDGE
    | DELETE_NODE
    | DELETE_EDGE
    | VALIDATE ;

graph_target ::=
    multimodel_target ;
```

Pattern match:

```sql
graph match app.customer_graph
pattern (:customer)-[:placed]->(:order)
where property('customer_id') = :customer_id
return node('order') as order_node,
       path() as matched_path;
```

Traversal:

```sql
graph traverse app.service_graph
from node :start_node
over edge_type 'depends_on'
depth 1 to 4
return node_id, edge_id, path_depth;
```

Shortest path:

```sql
graph shortest_path app.route_graph
from node :from_node
to node :to_node
over edge_type 'connects'
return path(), path_cost();
```

Graph semantics:

| Concern | Rule |
| --- | --- |
| Identity | Nodes, edges, and paths are descriptor-bound values with catalog and row identity. |
| Direction | Edge direction is part of the bound graph operation. |
| Properties | Property reads and writes bind to declared property descriptors. |
| Traversal order | Stable ordering exists only when the operation descriptor defines it or an outer `ORDER BY` is used. |
| Deletion | Edge and node deletion must obey referential, graph-constraint, and visibility rules. |
| Indexes | Adjacency, property, path, or spatial evidence requires final recheck. |

## Vector Statements

Vector statements operate on vector-bearing descriptors and vector index evidence.

```ebnf
vector_op_stmt ::=
    VECTOR vector_action vector_target vector_payload? return_clause? statement_option_list? ;

vector_action ::=
      SEARCH
    | RERANK
    | UPSERT
    | DELETE
    | REBUILD
    | DESCRIBE ;

vector_target ::=
    multimodel_target ;
```

Vector search:

```sql
vector search app.product_embedding
using vector(:query_embedding)
metric l2
limit 20
return object_uuid, distance, recheck_status;
```

Filtered search:

```sql
vector search app.product_embedding
using vector(:query_embedding)
metric cosine
where category_id = :category_id
limit 50
return object_uuid, distance;
```

Rerank candidates:

```sql
vector rerank app.product_embedding
using vector(:query_embedding)
candidates :candidate_set
metric dot
limit 20
return object_uuid, score, exact_rank;
```

Vector semantics:

| Concern | Rule |
| --- | --- |
| Dimension | Query vector and stored vector dimensions must match the bound descriptor. |
| Element profile | Element type, quantization, and normalization are descriptor owned. |
| Metric | Metric choice must be admitted for the target descriptor and index. |
| Approximate evidence | Approximate candidate sets must be exact-reranked or carry an admitted exactness proof before final delivery. |
| Filters | Relational, document, or property filters still require final row recheck. |
| Mutation | Upsert/delete updates vector-bearing rows and invalidates or updates vector index evidence transactionally. |

## Search Statements

Search statements operate on descriptor-bound search indexes or search projections.

```ebnf
fulltext_search_query ::=
    SEARCH search_target search_payload return_clause? statement_option_list? ;

search_target ::=
    multimodel_target ;
```

Text search:

```sql
search app.product_search
for :query_text
where language_tag = 'en'
limit 25
return object_uuid, score, snippet;
```

Fielded search:

```sql
search app.article_search
for :query_text
fields title, summary, body
where published_at >= :start_at
return object_uuid, score, matched_fields;
```

Search semantics:

| Concern | Rule |
| --- | --- |
| Analyzer profile | Tokenization, normalization, stemming, and language behavior are descriptor/profile owned. |
| Query text | Query text is input to a bound search operation, not executable authority. |
| Score | Score is ranking evidence and does not bypass row visibility or authorization. |
| Snippet | Snippet rendering must obey protected-material and redaction policy. |
| Refresh | Search refresh or rebuild requests are transactional operation requests where admitted. |
| Ordering | Use explicit ordering or the statement's result descriptor when stable rank order is required. |

## Time-Series Statements

Time-series statements operate on timestamped samples, events, buckets, rollups, and retention policies.

```ebnf
time_series_op_stmt ::=
    TIMESERIES timeseries_action timeseries_target timeseries_payload? return_clause? statement_option_list? ;

timeseries_action ::=
      QUERY
    | INSERT
    | DELETE
    | DOWNSAMPLE
    | RETAIN
    | GAPFILL
    | DESCRIBE ;

timeseries_target ::=
    multimodel_target ;
```

Window query:

```sql
timeseries query app.metric_sample
between :start_at and :end_at
where metric_name = 'cpu.user'
bucket interval '1 minute'
aggregate avg(value) as avg_value, max(value) as max_value
return series_id, bucket_start, avg_value, max_value;
```

Sample insert:

```sql
timeseries insert app.metric_sample
series :series_id
at :sample_at
value :sample_value
return series_id, sample_at, version;
```

Gap fill:

```sql
timeseries gapfill app.metric_sample
between :start_at and :end_at
bucket interval '5 minutes'
method previous
return bucket_start, value, gapfill_status;
```

Time-series semantics:

| Concern | Rule |
| --- | --- |
| Time descriptor | Timestamp type, precision, timezone handling, and calendar policy are descriptor owned. |
| Window bounds | Bounds are typed expressions; inclusive/exclusive behavior is operation defined. |
| Bucket descriptor | Bucket size, alignment, gap handling, and interpolation are bound before execution. |
| Ordering | Stable time order requires the statement descriptor or explicit `ORDER BY` when converted to a rowset. |
| Retention | Retention and downsample are policy-bound mutation requests and must be rollback-safe. |
| Late samples | Late arrival behavior is policy controlled and must preserve transaction visibility. |

## Mixed SQL And Multimodel Flow

Dedicated multimodel statements can return rowsets that are consumed by `WITH` or exposed through descriptor-bound views where admitted.

```sql
with ranked_products as (
  vector search app.product_embedding
  using vector(:query_embedding)
  metric l2
  limit 50
  return object_uuid, distance
)
select p.product_id,
       p.display_name,
       ranked_products.distance
from ranked_products
join app.product p on p.object_uuid = ranked_products.object_uuid
where p.is_active = true
order by ranked_products.distance, p.product_id;
```

The multimodel route supplies a rowset descriptor. The relational query still performs its own binding, authorization, predicate checks, and ordering.

## Mutation And Transaction Rules

Multimodel mutations are ordinary transactional mutations:

- `DOCUMENT PUT`, `DOCUMENT PATCH`, `DOCUMENT DELETE`, `KV PUT`, `KV DELETE`, `GRAPH CREATE_*`, `GRAPH UPDATE_*`, `VECTOR UPSERT`, and time-series sample changes become visible only when the transaction commits.
- Rollback restores the prior visible state.
- Index evidence is updated, retired, or invalidated transactionally.
- Cleanup must not remove document values, key-value versions, graph elements, vector payloads, search evidence, or time-series samples reachable from a visible row version.
- A failed statement must not publish partial results as durable state.

## Security And Sandboxing

Multimodel statements must obey the same effective-user and schema-root rules as SQL statements.

| Rule | Contract |
| --- | --- |
| Object visibility | A target hidden by schema root, sandbox, or privilege policy must not bind. |
| Field visibility | Protected paths, properties, vector payloads, snippets, keys, and sample values require explicit permission. |
| Mutation privilege | Update/delete/create operations require privilege on the target and affected descriptor surface. |
| Metadata disclosure | Diagnostics, counts, snippets, scores, paths, and object existence can be redacted. |
| External input | Query strings, vectors, paths, graph patterns, and stream payloads are untrusted inputs. |
| Message vectors | Refusals must use canonical diagnostic classes. |

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Target not found or hidden | Object resolution or sandbox denied. |
| Missing operation privilege | Authorization denied. |
| Path, key, pattern, metric, or bucket descriptor unsupported | Descriptor or operation unsupported. |
| Query vector dimension mismatch | Vector descriptor mismatch. |
| Search analyzer unavailable | Search profile unavailable. |
| Graph pattern invalid | Graph pattern binding failure. |
| Time window invalid | Temporal descriptor or bounds error. |
| Protected value requested | Protected-material or redaction refusal. |
| Index evidence stale | Recheck required or operation fenced. |
| Recovery-required state | Operation fenced until recovery action completes. |

## Proof Expectations

The multimodel proof suite should include:

- parse and SBLR-route checks for every action in every statement family;
- document missing/null path behavior, validation, patch, put, delete, and query;
- key-value get/put/delete/increment/expire/scan with version and TTL rules;
- graph node, edge, traversal, pattern, shortest path, property, and constraint behavior;
- vector dimension, metric, candidate, exact-rerank, filter, mutation, and index-maintenance behavior;
- search query, analyzer profile, scoring, snippet redaction, field filtering, and refresh behavior;
- time-series sample insertion, window reads, bucket aggregation, gap fill, retention, late sample behavior, and ordering;
- mixed SQL plus multimodel rowset flow through `WITH`, `FROM`, `JOIN`, `WHERE`, `GROUP BY`, and `ORDER BY`;
- sandbox, privilege, protected-material, recovery-fenced, resource-pressure, and malformed-input refusals;
- commit, rollback, close, reopen, and crash-recovery proof for multimodel mutations and index evidence.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | Each multimodel command family and action is recognized by SBsql. |
| Bind | Targets, keys, paths, vectors, patterns, timestamps, options, descriptors, and result shapes resolve. |
| Authorize | Effective user or agent UUID may perform the requested operation. |
| Admit | SBLR operation family and result descriptor are accepted by the engine verifier. |
| Execute | Rows, documents, keys, nodes, vectors, search hits, and samples obey MGA visibility and security. |
| Recheck | Candidate evidence is rechecked before result delivery or mutation finality. |
| Commit | Durable changes become visible only through transaction finality. |
| Diagnose | Refusals return canonical message vectors without leaking protected material. |
