# Cte List EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_cte_list`


## Production

```ebnf
cte_list                ::= cte ("," cte)* ;
```

## Meaning

`cte_list` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

When the parent `with_statement` includes contextual `RECURSIVE`, each CTE entry must still bind through an admitted execution route. The implemented recursive exact route is values-backed and emits the `values_recursive_cte` SBLR payload; unsupported recursive-reference shapes fail closed.

## Used By

| Parent Production |
| --- |
| with_statement |

## Child Productions

| Child Production |
| --- |
| cte |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
