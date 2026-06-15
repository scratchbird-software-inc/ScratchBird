# Show Management EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for management inspection commands.

Generation task: `ebnf_show_management`

## Production

```ebnf
show_management ::=
    "SHOW" "MANAGEMENT" management_target management_filter? show_option_list? ;

management_target ::=
      "HEALTH"
    | "READINESS"
    | "LIVENESS"
    | "LISTENERS"
    | "LOCAL" "MANAGERS"
    | "PARSER" "POOLS"
    | "PARSERS"
    | "UDR" "PACKAGES"
    | "SESSIONS"
    | "CONNECTIONS"
    | "STATEMENTS"
    | "TRANSACTIONS"
    | "WAITS"
    | "JOBS"
    | "CONFIG"
    | "STORAGE"
    | "FILESPACES"
    | "INDEXES"
    | "MEMORY"
    | "CACHE"
    | "TEMP"
    | "RECOVERY"
    | "CLEANUP"
    | "DIAGNOSTICS"
    | "SUPPORT" ;

management_filter ::=
      identifier
    | uuid_reference
    | "WITH" option_list ;
```

## Meaning

`show_management` returns authorized management projections for runtime targets. It is inspection only and must not control service state.

## Used By

| Parent Production |
| --- |
| management_stmt |

## Child Productions

| Child Production |
| --- |
| identifier |
| uuid_reference |
| option_list |
| show_option_list |

## Binding Contract

The target and optional filter must bind to visible runtime state. The result descriptor must redact protected values and hidden targets according to the caller's privileges and sandbox root.

## Practical Notes

- `SHOW MANAGEMENT` reports should be stable enough for tools to parse.
- Hidden targets should be omitted or refused without leaking existence.
- Runtime paths, secrets, and protected payloads are redacted by default.
