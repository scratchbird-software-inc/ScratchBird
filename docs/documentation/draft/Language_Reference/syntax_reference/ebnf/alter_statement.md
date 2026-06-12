# Alter Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_alter_statement`


## Production

```ebnf
alter_object_stmt       ::= alter_schema
                          | alter_database
                          | alter_filespace
                          | alter_table
                          | alter_index
                          | alter_domain
                          | alter_type_descriptor
                          | alter_sequence
                          | alter_view
                          | alter_routine_stmt
                          | alter_trigger
                          | alter_policy
                          | alter_mask
                          | alter_rls ;
```

## Meaning

`alter_object_stmt` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| ddl_statement |
| database_lifecycle_statement |
| filespace_lifecycle_statement |

## Child Productions

No child production reference was detected in the production body.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
