# Alter Management EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for management control commands.

Generation task: `ebnf_alter_management`

## Production

```ebnf
alter_management ::=
    "ALTER" "MANAGEMENT" management_target management_subject? management_action management_option_list? ;

management_subject ::=
      identifier
    | uuid_ref
    | object_ref ;

management_action ::=
      "DRAIN"
    | "UNDRAIN"
    | "START"
    | "STOP"
    | "RESTART"
    | "RELOAD"
    | "CANCEL"
    | "TERMINATE"
    | "FLUSH"
    | "ROTATE"
    | "VALIDATE"
    | "REBUILD" ;

management_option_list ::=
    "WITH" option ("," option)* ;
```

## Meaning

`alter_management` requests an operational control action. The statement can affect runtime admission, cancellation, reloads, package readiness, diagnostics, cache state, or maintenance work where policy admits it.

## Used By

| Parent Production |
| --- |
| management_statement |

## Child Productions

| Child Production |
| --- |
| management_target |
| identifier |
| uuid_ref |
| object_ref |
| option |

## Binding Contract

The binder must resolve a single authorized target and action. Control actions must fail closed when the target is hidden, unavailable, not controllable, protected by recovery fencing, or unsafe to change.

## Practical Notes

- `CANCEL` should preserve transaction finality and return a clear state if the target already completed.
- `TERMINATE` requires explicit forced-action authority.
- `RELOAD` must leave the prior effective state active when validation fails.
