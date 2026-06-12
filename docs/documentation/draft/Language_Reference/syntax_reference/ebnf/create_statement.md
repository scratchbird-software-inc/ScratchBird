# Create Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_create_statement`


## Production

```ebnf
create_object           ::= create_schema
                          | create_database_stmt
                          | create_filespace_stmt
                          | create_table_stmt
                          | create_index_stmt
                          | create_domain_stmt
                          | create_type_stmt
                          | create_sequence_stmt
                          | create_view_stmt
                          | create_function_stmt
                          | create_procedure_stmt
                          | create_trigger_stmt
                          | create_event_trigger_stmt
                          | create_policy_stmt ;
```

## Meaning

`create_object` is an SBsql grammar production (registry canonical name `create_object`). Note: `create_mask` and `create_rls` as standalone productions do not exist in the registry; policy-class objects are covered by `create_policy_stmt`. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| ddl_statement |
| recreate_statement |
| database_lifecycle_statement |
| filespace_lifecycle_statement |
| trigger lifecycle statements |

## Child Productions

No child production reference was detected in the production body.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
