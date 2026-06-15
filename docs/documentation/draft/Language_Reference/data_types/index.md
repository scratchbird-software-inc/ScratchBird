# Data Types Index

The data types chapter explains how SBsql values are described, compared,
stored, converted, and carried across the parser-to-engine boundary. The key
concept throughout is the _descriptor_: a resolved, engine-bound description of
a value's type, character set, precision, collation, domain policy, and
conversion rules. Knowing how descriptors work helps you write queries and
schema definitions that behave predictably under all conditions.

This chapter covers scalar types (numeric, text, temporal, binary), identity and
protected-material types (UUID, secret references), multimodel types (document,
graph, vector, spatial), and the domain and coercion rules that apply across all
of them.

## Pages In This Chapter

| Page | File | Use it for |
| --- | --- | --- |
| Type System Overview | [type_system_overview.md](type_system_overview.md) | Understanding the descriptor model, how type names resolve to descriptors before execution, and why the engine does not infer type behavior from SQL text. |
| Numeric Types | [numeric_types.md](numeric_types.md) | Reference for integer, unsigned integer, decimal, decimal-float, approximate real, and money descriptor families, ranges, and arithmetic rules. |
| Text, Collation, And Charset | [text_collation_and_charset.md](text_collation_and_charset.md) | Reference for character and large-text descriptors, character set rules, collation behavior, comparison, and indexing. |
| Temporal Types | [temporal_types.md](temporal_types.md) | Reference for date, time, timestamp, timestamptz, and interval descriptors, precision rules, timezone policy, and arithmetic. |
| Binary, UUID, And Protected Values | [binary_uuid_and_protected_values.md](binary_uuid_and_protected_values.md) | Reference for byte-string, UUID, and protected-material descriptors, including the security rules that govern protected values. |
| Document, Graph, Vector, And Multimodel Types | [document_graph_vector_and_multimodel_types.md](document_graph_vector_and_multimodel_types.md) | Reference for JSON document, array, vector, spatial, graph, time-series, and key-value descriptor families. |
| Domains, Casts, And Coercion | [domains_casts_and_coercion.md](domains_casts_and_coercion.md) | Understanding how domains wrap descriptors with policy, how explicit casts and assignment coercion work, and when domain identity is preserved or erased. |
| Conversion Matrix | [conversion_matrix.md](conversion_matrix.md) | Quick-reference matrix for which source-to-target conversions are implicit, require explicit casts, are lossy, or are refused. |

## Suggested Reading Order

Read `type_system_overview.md` first — it defines the vocabulary (descriptor,
carrier, domain, coercion) used by every other page in this chapter. Then read
the specific type pages relevant to your work. Finish with
`domains_casts_and_coercion.md` and `conversion_matrix.md` when you need to
understand how values move between types and domains.

Readers debugging a failed cast or an unexpected result type will find
`conversion_matrix.md` the fastest reference.

## Back To The Chapter List

[Language Reference](../README.md)
