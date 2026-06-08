# Management Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for management command routing without treating SQL text as engine authority.

Generation task: `ebnf_management_statement`

## Production

```ebnf
management_statement ::=
      show_management
    | agent_statement
    | alter_management
    | config_statement
    | support_bundle_statement ;
```

## Meaning

`management_statement` dispatches management inspection and control commands. The grammar recognizes the statement family only. Binding must still resolve runtime targets, configuration keys, job identifiers, service identifiers, result descriptors, security context, and an admitted SBLR operation route.

## Used By

| Parent Production |
| --- |
| native_statement |
| statement |

## Child Productions

| Child Production |
| --- |
| agent_statement |
| show_management |
| alter_management |
| config_statement |
| support_bundle_statement |

## Binding Contract

Management commands are operationally sensitive. The binder must distinguish inspection from control, apply sandbox and disclosure policy, refuse hidden targets, and admit only routes that preserve transaction finality, recovery fencing, and protected-material redaction.

## Practical Notes

- Management syntax is contextual.
- Runtime control actions require stronger authority than inspection.
- A management report is a typed projection, not direct access to internal state.
- Failed management actions must return canonical message vectors.
