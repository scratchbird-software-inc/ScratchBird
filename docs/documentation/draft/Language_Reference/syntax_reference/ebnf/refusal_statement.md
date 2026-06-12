# Refusal Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_refusal_statement`


## Production

```ebnf
refusal_stmt            ::= unsupported_statement | denied_statement | unlicensed_statement ;
```

<!-- Fragment dispatcher: refusal_stmt has no registry id of its own; it is a grammar-level dispatcher for refusal leaf productions. -->

## Meaning

`refusal_stmt` is an SBsql grammar fragment dispatcher. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| statement |

## Child Productions

| Child Production |
| --- |
| denied_statement |
| unlicensed_statement |
| unsupported_statement |

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
