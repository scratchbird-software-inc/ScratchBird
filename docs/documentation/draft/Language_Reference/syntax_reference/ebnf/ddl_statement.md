# Ddl Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_ddl_statement`


## Production

```ebnf
ddl_catalog             ::= create_object | alter_object_stmt | drop_object_stmt | comment_on_stmt | rename_object_stmt | recreate_object ;
```

## Meaning

`ddl_catalog` is an SBsql grammar production (registry family name). It is part of contextual parsing only; it does not by itself authorize execution. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| native_statement |

## Child Productions

| Child Production |
| --- |
| alter_object_stmt |
| comment_on_stmt |
| create_object |
| drop_object_stmt |
| recreate_object |
| rename_object_stmt |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
