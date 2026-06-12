# Select Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_select_statement`


## Production

```ebnf
select_statement        ::= "SELECT" select_modifier? projection_list from_clause? where_clause? group_by_clause? having_clause? window_clause? order_by_clause? limit_clause? ;
```

## Meaning

`select_statement` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| query_statement |
| with_clause |

## Child Productions

| Child Production |
| --- |
| from_clause |
| group_by_clause |
| having_clause |
| limit_clause |
| order_by_clause |
| projection_list |
| select_modifier |
| where_clause |
| window_clause |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
