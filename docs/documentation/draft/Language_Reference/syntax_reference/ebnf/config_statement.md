# Config Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for configuration inspection and reload commands.

Generation task: `ebnf_config_statement`

## Production

```ebnf
config_statement ::=
      "CONFIG" "SHOW" config_target? config_option_list?
    | "CONFIG" "VALIDATE" config_target? config_option_list?
    | "CONFIG" "RELOAD" config_target? config_option_list?
    | "CONFIG" "HISTORY" config_target? config_option_list?
    | "CONFIG" "EFFECTIVE" config_target? config_option_list? ;

config_target ::=
      identifier
    | qualified_name ;

config_option_list ::=
    "WITH" option ("," option)* ;
```

## Meaning

`config_statement` inspects, validates, reloads, and reports configuration history or effective values. It does not grant direct file access or permission to read protected values.

## Used By

| Parent Production |
| --- |
| management_statement |

## Child Productions

| Child Production |
| --- |
| identifier |
| qualified_name |
| option |

## Binding Contract

Configuration keys, targets, defaults, reloadability, protected references, and result descriptors must bind before execution. A failed reload must leave the prior effective configuration active.

## Practical Notes

- `CONFIG VALIDATE` should be usable without applying changes.
- `CONFIG HISTORY` reports authorized reload attempts and message vectors.
- Secrets and protected values must render as references or redacted values.
