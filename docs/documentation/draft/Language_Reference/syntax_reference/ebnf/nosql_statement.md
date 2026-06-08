# Multimodel Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for multimodel command routing without treating SQL text as engine authority.

Generation task: `ebnf_nosql_statement`

## Production

```ebnf
nosql_statement ::=
      document_statement
    | kv_statement
    | graph_statement
    | vector_statement
    | search_statement
    | timeseries_statement ;
```

## Meaning

`nosql_statement` is the SBsql grammar dispatcher for document, key-value, graph, vector, search, and time-series command families. The production recognizes command shape only. Binding must still resolve catalog UUIDs, descriptors, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| native_statement |
| statement |

## Child Productions

| Child Production |
| --- |
| document_statement |
| kv_statement |
| graph_statement |
| vector_statement |
| search_statement |
| timeseries_statement |

## Binding Contract

Each child statement must produce a typed result descriptor: a rowset, scalar value, mutation result, or message vector. Candidate evidence from indexes, graph traversal, vector search, text search, time indexes, and key ranges must be rechecked by the engine before final result delivery or mutation finality.

## Practical Notes

- Multimodel command words are contextual.
- A multimodel statement is not a separate engine or separate transaction authority.
- Mutating statements are transactional and rollback-safe.
- Protected material, snippets, paths, keys, scores, object identity, and counts can be redacted by policy.
