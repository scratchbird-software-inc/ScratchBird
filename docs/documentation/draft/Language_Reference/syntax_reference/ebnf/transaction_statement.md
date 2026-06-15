# Transaction Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_transaction_statement`


## Production

```ebnf
transaction_statement   ::= begin_transaction | commit_stmt | rollback_stmt | savepoint_stmt | set_transaction_stmt | show_transaction_runtime ;
```

<!-- Fragment dispatcher: transaction_statement has no registry id of its own; it is a grammar-level dispatcher for the transaction control production family. -->

## Meaning

`transaction_statement` is an SBsql grammar fragment dispatcher. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

Full user-facing semantics for transaction boundaries, autocommit, isolation, savepoints, locking, runtime inspection, and recovery-facing states are documented in [../transaction_control.md](../transaction_control.md).

## Used By

| Parent Production |
| --- |
| native_statement |

## Child Productions

| Child Production |
| --- |
| begin_transaction |
| commit_stmt |
| rollback_stmt |
| savepoint_stmt |
| set_transaction_stmt |
| show_transaction_runtime |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
