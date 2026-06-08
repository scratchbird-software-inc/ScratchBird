# From Clause EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_from_clause`


## Production

```ebnf
from_clause ::=
    "FROM" table_expression ;

table_expression ::=
      table_reference ("," table_reference)*
    | joined_table ;

table_reference ::=
      relation_reference
    | derived_table
    | table_function_reference
    | values_table
    | multimodel_table_reference
    | bridge_table_reference ;

relation_reference ::=
    qualified_name table_alias? ;

derived_table ::=
    "(" query_statement ")" table_alias column_alias_list? ;

table_function_reference ::=
    "TABLE" "(" function_call ")" table_alias? column_alias_list? ;

values_table ::=
    "VALUES" row_constructor ("," row_constructor)* table_alias column_alias_list? ;

joined_table ::=
    table_reference join_clause+ ;

join_clause ::=
      join_type? "JOIN" table_reference join_condition
    | "CROSS" "JOIN" table_reference
    | "LATERAL" "JOIN" table_reference join_condition? ;

join_type ::=
      "INNER"
    | "LEFT" "OUTER"?
    | "RIGHT" "OUTER"?
    | "FULL" "OUTER"? ;

join_condition ::=
      "ON" predicate
    | "USING" "(" identifier ("," identifier)* ")" ;

table_alias ::=
    "AS"? identifier ;

column_alias_list ::=
    "(" identifier ("," identifier)* ")" ;
```

## Meaning

`from_clause` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| select_statement |
| query_statement |
| table_function_reference |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| query_statement |
| function_call |
| row_constructor |
| predicate |
| identifier |
| multimodel_table_reference |
| bridge_table_reference |

## Binding Contract

`FROM` produces a rowset descriptor for the current query block. Every source must bind to a visible catalog object, CTE, derived query, table-valued function, values descriptor, multimodel projection, or bridge descriptor. Joins can reorder physically only when the optimizer proves that descriptor, predicate, null-extension, authorization, and transaction-visibility semantics are preserved.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
- `FROM` does not define result ordering. Use `ORDER BY` for deterministic presentation order.
- Candidate sources such as indexes, search hits, vectors, graph traversal, and bridge rows are evidence only until final row checks pass.
