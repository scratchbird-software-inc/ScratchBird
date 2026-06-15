# Security Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_security_statement`


## Production

```ebnf
dcl_security_stmt       ::= create_principal_stmt | alter_principal_stmt | drop_principal_stmt | grant_stmt | revoke_stmt | show_security_policy ;
```

<!-- Fragment dispatcher: dcl_security_stmt is the registered security family dispatcher (SBSQL registry). -->

## Meaning

`dcl_security_stmt` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| native_statement |

## Child Productions

| Child Production |
| --- |
| alter_principal_stmt |
| create_principal_stmt |
| drop_principal_stmt |
| grant_stmt |
| revoke_stmt |
| show_security_policy |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
