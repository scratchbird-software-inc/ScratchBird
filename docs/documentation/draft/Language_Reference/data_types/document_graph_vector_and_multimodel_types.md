# Document, Graph, Vector, And Multimodel Types

This page is part of the SBsql Language Reference Manual. It defines descriptor
behavior for document, collection, vector, spatial, graph, search, time-series,
and key-value values.

Generation task: `data_types_document_graph_vector`

## Purpose

SBsql includes relational, document, graph, vector, search, time-series, and
key-value surfaces. These are not separate engines. They use the same parser,
descriptor, SBLR, catalog, MGA transaction, security, policy, storage, index,
and recovery rules as other SBsql operations.

The common rule is:

> Multimodel indexes and providers produce candidate evidence. They do not own
> final row authority.

Final result delivery still requires engine recheck of descriptor compatibility,
MGA visibility, security, policy, predicate truth, ordering requirements, and
result shape.

## Supported Multimodel Types

| Canonical Type | Family | Payload | Primary Operations | Required Recheck |
| --- | --- | --- | --- | --- |
| `json` | document | Text-preserving document value. | JSON extraction, path predicates, rendering. | Path, scalar type, predicate, MGA, and security. |
| `jsonb` | document | Normalized binary document value. | Containment, path extraction, comparison where admitted, indexing. | Path, scalar type, predicate, MGA, and security. |
| `document` | document | Document payload plus schema/profile metadata. | Document insert, update, query, patch, path indexes. | Document identity, path semantics, MGA, and policy. |
| `array<T>` | collection | Ordered homogeneous elements. | Element access, unnesting, containment, assignment. | Element descriptor and bounds. |
| `multiset<T>` | collection | Unordered homogeneous elements with multiplicity. | Bag operations and aggregate-style element operations. | Element descriptor and multiplicity. |
| `row(...)` | structured | Named or positional field descriptors. | Row constructors, function returns, result descriptors. | Field descriptor and null policy. |
| `record(...)` | structured | Runtime record shape. | Procedural variables, cursor rows, dynamic rowsets. | Shape descriptor and policy. |
| `vector<float32,n>` | vector | `4 * n` bytes plus descriptor metadata. | Vector search and vector functions. | Exact vector, metric, MGA, security, and predicate. |
| `vector<float16,n>` | vector | `2 * n` bytes plus descriptor metadata. | Vector search where fp16 is admitted. | Exact source vector or quantization proof, plus MGA/security. |
| `vector<int8,n>` | vector | `n` bytes plus descriptor metadata. | Quantized vector search where admitted. | Quantization proof, exact rerank where required, MGA/security. |
| `geometry` | spatial | Geometry payload plus spatial reference/profile metadata. | Spatial predicates and indexes. | Exact geometry predicate and visibility. |
| `geography` | spatial | Geodetic payload plus spatial reference/profile metadata. | Geodetic predicates and indexes. | Exact geodetic predicate and visibility. |
| `graph` | graph | Graph container or traversal payload. | Node, edge, path, and traversal operations. | Node/edge/path identity, traversal order, MGA/security. |
| `node` | graph | Node identity and properties. | Node lookup and traversal. | Node identity and visibility. |
| `edge` | graph | Edge identity, endpoints, and properties. | Edge lookup and traversal. | Endpoint, edge identity, and visibility. |
| `path` | graph | Ordered traversal result. | Path queries and graph pattern results. | Traversal order and visibility. |
| `search_document` | search | Tokenized or tokenizable search payload. | Full-text predicates, ranking, highlighting where admitted. | Source document, token profile, ranking policy, MGA/security. |
| `timeseries` | time-series | Time-keyed observations or windows. | Windowing, resampling, interpolation where admitted. | Time key, order, window descriptor, MGA/security. |
| `kv_key`, `kv_value` | key-value | Key/value payload descriptors. | Get, put, delete, iterate, range, sorted-set style operations where admitted. | Key descriptor, value descriptor, ordering/hash, MGA/security. |

## Document Types

Document descriptors own path semantics.

| Concern | Rule |
| --- | --- |
| Missing versus null | Missing path and JSON null are distinct where the operator requires it. |
| Object key order | Preserved or normalized according to descriptor. |
| Scalar extraction | Extracted scalars bind to target descriptors through explicit or assignment conversion. |
| Path expressions | Bind to descriptor-aware operations; path text is not execution authority. |
| Patch/update | Validates target path, mutation policy, and resulting document descriptor. |
| Indexing | Document indexes produce candidate rows and path evidence; executor recheck remains mandatory. |

Example:

```sql
select document_id,
       cast(payload->>'created_at' as timestamp(6) with time zone) as created_at
from app.ingest_document
where payload @? '$.status == "ready"';
```

The exact path operator surface is descriptor-bound. Invalid paths, unsupported
path features, and scalar conversion failures return diagnostics.

## Collections And Structured Values

Collections and structured values are descriptor shapes.

| Type | Rule |
| --- | --- |
| `array<T>` | Ordered and position-addressable. Element descriptor applies to every element. |
| `multiset<T>` | Unordered and multiplicity-aware. Equality and containment are descriptor-owned. |
| `row(...)` | Fixed field shape used for row constructors and result descriptors. |
| `record(...)` | Runtime field shape used for procedural values and dynamic rowsets. |

Element nullability, domain validation, masking, and protected-material rules
apply to elements and fields.

## Vector Types

Vector descriptors must state:

- element profile;
- dimension;
- metric;
- provider family;
- normalization requirements;
- quantization profile where used;
- exact-recheck requirements;
- index generation;
- metric/resource epoch;
- result ordering rule.

| Provider Family | Contract |
| --- | --- |
| Exact vector | Computes exact candidates and still returns candidate evidence only. |
| Approximate graph-style vector index | Must exact-rerank or provide proof that exact rerank occurred before final result delivery. |
| Inverted-file style vector index | Must carry training generation, descriptor epoch, metric epoch, and exact-rerank proof. |
| Quantized vector | Must carry quantization profile and exact-source or admitted quantization proof. |

Example:

```sql
vector search app.embedding_index
using :query_vector
limit 10;
```

The query vector parameter must bind to the index vector descriptor or an
admitted conversion. Dimension mismatch is a bind diagnostic.

## Spatial Types

Spatial descriptors define coordinate profile, spatial reference identity,
dimensionality, indexing behavior, and exact-recheck requirements.

| Concern | Rule |
| --- | --- |
| Spatial reference | Must be part of the descriptor or explicit operation. |
| Geometry validity | Invalid shapes are refused or repaired only by explicit admitted functions. |
| Bounding-box indexes | Candidate evidence only. Exact predicate recheck is mandatory. |
| Geodetic behavior | Descriptor-owned. Do not infer planar behavior from spelling alone. |

## Graph Types

Graph descriptors define node identity, edge identity, endpoint descriptors,
property descriptors, traversal shape, and path result ordering.

| Concern | Rule |
| --- | --- |
| Node identity | UUID or descriptor-owned graph identity. |
| Edge identity | Edge UUID plus endpoint identity and direction where relevant. |
| Traversal order | Part of the operation descriptor when stable order is required. |
| Path result | Ordered sequence of node/edge descriptors. |
| Security | Each visible node, edge, property, and path element remains policy-controlled. |

Graph indexes and traversal providers are candidate sources only. They do not
own row visibility, security, or transaction finality.

## Search Types

Search descriptors own tokenization, language/profile, ranking, normalization,
highlighting, and index behavior.

| Concern | Rule |
| --- | --- |
| Token profile | Descriptor-owned and versioned. |
| Ranking | Operation-owned result descriptor. |
| Highlighting | Rendering operation that must not reveal protected material. |
| Search index | Candidate evidence only unless exact proof and policy admit final result. |
| Ordering | Ranking order is part of the result contract. |

## Time-Series Types

Time-series descriptors define time key, value descriptor, series identity,
bucket/window behavior, ordering, gap policy, interpolation policy, and result
shape.

| Concern | Rule |
| --- | --- |
| Time key | Temporal descriptor controls comparison and ordering. |
| Window | Window descriptor owns boundary inclusion, timezone, and precision. |
| Resampling | Requires explicit policy for fill, interpolation, and missing values. |
| Ordering | Stable time ordering must be part of the result contract. |

## Key-Value Types

Key-value descriptors define key shape, value shape, hash/order behavior, TTL or
expiry policy where admitted, and iteration behavior.

| Concern | Rule |
| --- | --- |
| Key descriptor | Controls equality, hashing, ordering, and range iteration. |
| Value descriptor | Controls stored value, domain checks, masks, and protected material. |
| Iteration | Ordering must be descriptor-defined for range or sorted operations. |
| TTL/expiry | Policy-owned and transaction-aware where admitted. |

## Index And Provider Recheck Rule

Every multimodel access path follows this rule:

1. The index or provider returns candidate evidence.
2. The executor rechecks the source row or object under the active transaction.
3. The executor rechecks descriptor compatibility.
4. Security, masking, row-level policy, and protected-material policy are
   applied.
5. The result shape and ordering contract are validated.
6. Only then is the row, document, graph path, vector result, search hit,
   time-series sample, or key-value item returned.

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Document path invalid | Bind or execution diagnostic according to when path is known. |
| Missing path used as scalar | Diagnostic or null/missing result according to operator descriptor. |
| Vector dimension mismatch | Bind diagnostic. |
| Unsupported vector metric | Bind/admission diagnostic. |
| Approximate index lacks exact-recheck proof | Refusal before final result delivery. |
| Invalid geometry | Diagnostic unless explicit repair function is used. |
| Graph traversal cycle or limit exceeded | Diagnostic or partial result according to operation contract. |
| Search profile missing | Bind/admission diagnostic. |
| Time-series ordering ambiguous | Refusal for operations requiring stable order. |
| Key descriptor mismatch | Bind diagnostic. |
| Protected value appears in multimodel rendering | Denied or redacted result. |

## Syntax Productions

```ebnf
multimodel_type         ::= document_type
                          | collection_type
                          | vector_type
                          | spatial_type
                          | graph_type
                          | search_type
                          | timeseries_type
                          | key_value_type ;
```

```ebnf
document_type           ::= "json"
                          | "jsonb"
                          | "document" ;
```

```ebnf
collection_type         ::= "array" "<" data_type ">"
                          | "multiset" "<" data_type ">"
                          | row_type
                          | record_type ;
```

```ebnf
vector_type             ::= ("vector" | "embedding") "<" vector_element_type "," dimension ">" ;
vector_element_type     ::= "float32" | "float16" | "int8" ;
```

```ebnf
spatial_type            ::= "geometry" | "geography" ;
graph_type              ::= "graph" | "node" | "edge" | "path" ;
```

## Related Pages

- [Type System Overview](type_system_overview.md)
- [Conversion Matrix](conversion_matrix.md)
- [Domains, Casts, And Coercion](domains_casts_and_coercion.md)
- [Multimodel Statements](../syntax_reference/multimodel_statements.md)
- [Index Lifecycle](../syntax_reference/index.md)
- [Projection](../syntax_reference/projection.md)
- [Where](../syntax_reference/where.md)

## Verification Checklist

The multimodel proof suite should demonstrate:

- document missing/null behavior matches descriptor rules;
- document path indexes require executor recheck;
- collection element descriptors and null policies are enforced;
- vector dimensions, element profiles, and metrics are enforced;
- approximate vector indexes cannot return final rows without exact recheck or
  admitted proof;
- spatial indexes use candidate evidence plus exact predicate recheck;
- graph traversals preserve node, edge, path, order, and visibility rules;
- search ranking and ordering match result contracts;
- time-series windows preserve time key, boundary, and ordering semantics;
- key-value range and hash operations use descriptor-owned key behavior;
- protected material is redacted or denied across all multimodel result shapes.
