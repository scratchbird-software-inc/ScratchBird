# Cte EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_cte`


## Production

```ebnf
cte                     ::= identifier column_alias_list? "AS" "(" query_statement ")" ;
```

## Meaning

`cte` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

When used under `WITH RECURSIVE`, the CTE may lower through the bounded values-backed recursive route:

```ebnf
recursive_values_cte    ::= "WITH" "RECURSIVE" identifier column_alias_list?
                            "AS" "(" values_source "UNION" "DISTINCT"? values_source ")"
                            select_statement ;
```

That route produces a `values_recursive_cte` SBLR payload and uses engine-owned fixed-point materialization. A parser must refuse recursive CTE forms whose duplicate behavior, recursive-reference evaluation, or search/cycle semantics are not represented by an admitted SBLR route.

## Used By

| Parent Production |
| --- |
| cte_list |

## Child Productions

| Child Production |
| --- |
| column_alias_list |
| query_statement |
| values_source |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
