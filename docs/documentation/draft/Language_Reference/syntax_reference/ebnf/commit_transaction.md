# Commit Transaction EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_commit_transaction`


## Production

```ebnf
commit_transaction      ::= "COMMIT" ("TRANSACTION" | "WORK")? ;
```

## Meaning

`commit_transaction` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

Full user-facing semantics for commit finality, chain/release options, uncertainty, and recovery-facing behavior are documented in [../transaction_control.md](../transaction_control.md).

## Used By

| Parent Production |
| --- |
| transaction_statement |

## Child Productions

No child production reference was detected in the production body.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
