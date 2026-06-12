# Revoke Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_revoke_statement`


## Production

```ebnf
revoke_stmt             ::= "REVOKE" grant_payload "FROM" principal_ref revoke_option_list? ;
```

## Meaning

`revoke_stmt` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

Full user-facing semantics for revoke behavior, grant-option removal, admin-option removal, cascade/restrict, security epoch changes, sandbox behavior, and effective privilege resolution are documented in [../security_and_privilege_statements.md](../security_and_privilege_statements.md).

## Used By

| Parent Production |
| --- |
| security_statement |

## Child Productions

| Child Production |
| --- |
| principal_ref |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
