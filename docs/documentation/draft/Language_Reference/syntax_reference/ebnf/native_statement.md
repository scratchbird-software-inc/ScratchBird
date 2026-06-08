# Native Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_native_statement`


## Production

```ebnf
native_statement        ::= query_statement
                          | dml_statement
                          | ddl_statement
                          | transaction_statement
                          | security_statement
                          | policy_statement
                          | observability_statement
                          | management_statement
                          | acceleration_statement
                          | archive_replication_migration_statement
                          | nosql_statement
                          | private_cluster_statement ;
```

## Meaning

`native_statement` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| statement |

## Child Productions

| Child Production |
| --- |
| acceleration_statement |
| archive_replication_migration_statement |
| ddl_statement |
| dml_statement |
| management_statement |
| nosql_statement |
| observability_statement |
| policy_statement |
| private_cluster_statement |
| query_statement |
| security_statement |
| transaction_statement |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
