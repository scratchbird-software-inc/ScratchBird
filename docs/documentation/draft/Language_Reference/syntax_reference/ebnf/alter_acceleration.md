# Alter Acceleration EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_alter_acceleration`


## Production

```ebnf
alter_acceleration      ::= "ALTER" ("NATIVE" "COMPILE" | "GPU") acceleration_action ;
```

## Meaning

`alter_acceleration` is a grammar fragment grouping. It is NOT a registry canonical surface name; individual ALTER GPU and ALTER NATIVE COMPILE operations dispatch through their concrete operation names (e.g., `gpu_capability_name`, `gpu_capability_options`). This fragment is documented as a documentation aid only.

## Used By

| Parent Production |
| --- |
| acceleration_stmt |

## Child Productions

No child production reference was detected in the production body.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
