# With Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_with_statement`


## Production

```ebnf
with_statement          ::= "WITH" "RECURSIVE"? cte_list select_statement ;
```

## Meaning

`with_statement` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

`RECURSIVE` is contextual. The current admitted recursive route lowers bounded values-backed recursive CTEs to the `values_recursive_cte` SBLR payload and `SBLR_QUERY_PLAN_OPERATION`; broader recursive-reference forms must be rejected until a matching execution route is admitted.

## Used By

| Parent Production |
| --- |
| query_statement |

## Child Productions

| Child Production |
| --- |
| cte_list |
| select_statement |

## Recursive Values Route

```ebnf
recursive_values_cte    ::= "WITH" "RECURSIVE" identifier column_alias_list?
                            "AS" "(" values_source "UNION" "DISTINCT"? values_source ")"
                            select_statement ;
```

This production documents the implemented exact route, not the full theoretical recursive SQL space. `UNION ALL`, search/cycle clauses, and recursive terms that read the CTE's previous iteration need explicit future SBLR and engine admission.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
