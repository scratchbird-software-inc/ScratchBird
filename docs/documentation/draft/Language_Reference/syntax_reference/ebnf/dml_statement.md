# Dml Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_dml_statement`


## Production

```ebnf
dml                     ::= insert_statement | update_statement | delete_statement | merge_statement | upsert_statement | copy_statement ;
```

## Meaning

`dml` is an SBsql grammar production (registry canonical name `dml`). It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| native_statement |

## Child Productions

| Child Production |
| --- |
| delete_statement |
| insert_statement |
| merge_statement |
| copy_statement |
| update_statement |
| upsert_statement |

Note: `delete_statement`, `insert_statement`, `update_statement`, `merge_statement`, `upsert_statement`, and `copy_statement` are the registry canonical names for these productions.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
