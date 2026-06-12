# Denied Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_denied_statement`


## Production

```ebnf
refusal_stmt            ::= denied_token_sequence ;
```

## Meaning

`refusal_stmt` is an SBsql grammar production (registry canonical name `refusal_stmt`). It is part of contextual parsing only; it does not by itself authorize execution.

## Used By

| Parent Production |
| --- |
| native_statement |

## Child Productions

No child production reference was detected in the production body.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
