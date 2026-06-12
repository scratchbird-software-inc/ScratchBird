# Vector Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for vector commands.

Generation task: `ebnf_vector_statement`

## Production

```ebnf
vector_op_stmt ::=
    "VECTOR" vector_action vector_target vector_payload? return_clause? statement_option_list? ;

vector_action ::=
      "SEARCH"
    | "RERANK"
    | "UPSERT"
    | "DELETE"
    | "REBUILD"
    | "DESCRIBE" ;

vector_target ::=
    qualified_name ;

vector_payload ::=
      vector_search_payload
    | vector_rerank_payload
    | vector_mutation_payload ;

vector_search_payload ::=
    "USING" expression vector_metric_clause? multimodel_where_clause? limit_clause? ;

vector_rerank_payload ::=
    "USING" expression "CANDIDATES" expression vector_metric_clause? limit_clause? ;

vector_mutation_payload ::=
    ("KEY" expression)? "VALUE" expression ;

vector_metric_clause ::=
    "METRIC" identifier ;
```

## Meaning

`vector_op_stmt` recognizes vector candidate search, exact rerank, mutation, rebuild, and inspection commands. Vector dimension, element profile, metric, index evidence, and exact-recheck behavior are descriptor owned.

## Used By

| Parent Production |
| --- |
| nosql_statement |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| expression |
| identifier |
| return_clause |
| statement_option_list |
| multimodel_where_clause |
| limit_clause |

## Binding Contract

The target must resolve to a vector-capable descriptor. Query vector dimensions and element profile must match the target descriptor. Approximate candidate evidence must be exact-reranked or carry an admitted exactness proof before final delivery.

## Practical Notes

- Vector indexes are candidate evidence only.
- Metric choice must be admitted for the vector descriptor and index.
- Mutation must update or invalidate vector evidence transactionally.
