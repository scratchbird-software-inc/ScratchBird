# Multimodel Statements

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_multimodel`


## Purpose

Document, graph, vector, search, time-series, and key-value statements are first-class SBsql surfaces. They share the same transaction, security, descriptor, and SBLR authority model as relational SQL.

Use the dedicated statement family when the requested operation is about a multimodel access pattern rather than a relational projection of the same data.

Example:

```sql
document get app.documents key :document_id;
graph match app.graph using :pattern;
```

## Syntax Productions

```ebnf
nosql_statement         ::= document_statement | graph_statement | vector_statement | search_statement | timeseries_statement | kv_statement ;
```

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

```ebnf
timeseries_statement    ::= "TIMESERIES" timeseries_action timeseries_payload ;
```

```ebnf
kv_statement            ::= "KV" kv_action kv_payload ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
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
| opensearch_mapping_clause | grammar_production | multi_model | yes | rs.sbsql.rowset.v1 |
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
