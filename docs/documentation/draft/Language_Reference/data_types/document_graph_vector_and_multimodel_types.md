# Document, Graph, Vector, And Multimodel Types

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_document_graph_vector`


## Purpose

SBsql includes document, graph, vector, search, time-series, and key-value statement families. These surfaces are not separate engines. They lower through the same parser, descriptor, SBLR, catalog, transaction, and security rules as relational statements.

Vector operations must preserve dimension, element profile, metric resource, and exact recheck authority. Document and graph operations must preserve path semantics, missing/null behavior, and record identity. Search and time-series operations must preserve result ordering and stream semantics when their surface requires it.

Example:

```sql
vector search app.embedding_index using :query_vector limit 10;
```

## Supported Multimodel Types

| Canonical Type | Descriptor Family | Payload | Primary Operations | Required Recheck |
| --- | --- | --- | --- | --- |
| `json` | `json_document` | Text-preserving JSON document | JSON extraction, path predicates, rendering | Predicate and type recheck when an index produces candidates. |
| `jsonb` | `json_document` | Normalized binary JSON/document representation | Path extraction, containment, indexing, comparison where admitted | Predicate and type recheck when an index produces candidates. |
| `document` | `json_document` | Document payload plus schema/profile metadata | Document insert/update/query, document path indexes | Document identity, path, security, and MGA recheck. |
| `array<T>` | descriptor overlay | Ordered homogeneous elements | Element access, containment, unnesting, SBsql array compatibility | Element descriptor and bounds recheck. |
| `multiset<T>` | descriptor overlay | Unordered homogeneous elements | Bag operations and aggregate-like element operations | Element descriptor and multiplicity recheck. |
| `vector<float32,n>` | `vector` | `4 * n` bytes plus descriptor metadata | Exact vector search and vector functions | Exact source vector, MGA, security, and metric recheck. |
| `vector<float16,n>` | `vector` | `2 * n` bytes plus descriptor metadata | Vector search where provider admits fp16 | Exact source vector, MGA, security, and metric recheck. |
| `vector<int8,n>` | `vector` | `n` bytes plus descriptor metadata | Quantized vector search where provider admits int8 | Exact source vector or admitted quantization proof plus MGA/security recheck. |
| `geometry` | spatial/document descriptor | Geometry payload plus SRID/profile metadata | Spatial predicates and spatial indexes | Exact geometry predicate recheck. |
| `geography` | spatial/document descriptor | Geodetic geometry payload plus SRID/profile metadata | Geodetic predicates and indexes where surfaced | Exact geodetic predicate recheck. |
| `graph` | `graph` | Graph container or traversal payload | Node/edge/path operations | Node identity, edge identity, traversal, MGA, and security recheck. |
| `node` | `graph` | Node identity and properties | Node lookup and traversal | Node identity and visibility recheck. |
| `edge` | `graph` | Edge identity, endpoints, and properties | Edge lookup and traversal | Edge endpoint, identity, and visibility recheck. |
| `path` | `graph` | Ordered traversal result | Path queries and graph pattern results | Traversal order and visibility recheck. |

## Vector Descriptor Rules

Vector descriptors must state element profile, dimension, metric resource, provider family, and exact-recheck requirements.

| Provider Family | Contract |
| --- | --- |
| `vector_exact` | Computes exact candidates and still returns candidate evidence only. Final rows require MGA, security, and predicate recheck. |
| `vector_hnsw` | Uses HNSW approximate candidate search. It must exact-rerank or provide proof that exact rerank has occurred before final result delivery. |
| `vector_ivf` | Uses IVF-style approximate candidate search. It must carry training generation, descriptor epoch, metric epoch, and exact-rerank proof. |

No vector provider is row-authoritative. A vector index can accelerate candidate selection, but it cannot own transaction finality, row visibility, security, predicate truth, or result ordering unless the executor has performed the required rechecks.

## Document And Graph Rules

| Rule | Behavior |
| --- | --- |
| Missing versus null | `json`/`jsonb` and document path operations distinguish a missing path from a path with JSON null where the operator surface requires it. |
| Path identity | Path expressions bind to descriptor-aware operations. A text path is not authority after binding. |
| Graph traversal order | Traversal result order is part of the operation descriptor when the surface requires stable order. |
| Index use | Document, spatial, graph, and vector indexes produce candidate evidence. Executor recheck remains mandatory. |
| metadata rendering | SBsql parsers may expose JSON, spatial, graph, vector, or document commands only for the SBsql features they support. Cross-SBsql behavior is not inferred. |

## Syntax Productions

```ebnf
document_statement      ::= "DOCUMENT" document_action document_payload ;
```

```ebnf
graph_statement         ::= "GRAPH" graph_action graph_payload ;
```

```ebnf
vector_statement        ::= "VECTOR" vector_action vector_payload ;
```

```ebnf
search_statement        ::= "SEARCH" search_action search_payload ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| vector_literal | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| graph_pattern | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_hash_verb | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| vector_search_body | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| time_series_window_expr | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| vector_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| graph_constraint_stmt | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_geo_verb | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| graph_traversal_stmt | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| fulltext_mapping_clause | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| vector_metric | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| timeseries_clause | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_verifiable_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| json_kv_pair | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| time_series_op_stmt | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| fulltext_search_body | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_stream_blob_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_reference_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_geo_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| vector_rerank_clause | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| vector_filter | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| vector_op_stmt | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_iter_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
| kv_sorted_set_op | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
