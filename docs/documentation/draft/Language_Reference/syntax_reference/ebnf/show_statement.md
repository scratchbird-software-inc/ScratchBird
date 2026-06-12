# Show Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for generic `SHOW` inspection.

Generation task: `ebnf_show_statement`

## Production

```ebnf
show_stmt ::=
    "SHOW" show_target show_option_list? ;

show_target ::=
      "HEALTH"
    | "READINESS"
    | "LIVENESS"
    | "VERSION"
    | "BUILD"
    | "RUNTIME"
    | "DIAGNOSTICS"
    | "DATABASES"
    | "SCHEMAS"
    | "TABLES"
    | "VIEWS"
    | "INDEXES"
    | "FUNCTIONS"
    | "PROCEDURES"
    | "TRIGGERS"
    | "DOMAINS"
    | "SEQUENCES"
    | "PRIVILEGES"
    | "SEARCH" "PATH"
    | show_management_target ;

show_option_list ::=
    "WITH" option ("," option)* ;
```

## Meaning

`show_stmt` returns compact authorized projections. It does not grant access to hidden catalog rows, runtime internals, secrets, or protected material.

## Used By

| Parent Production |
| --- |
| observability_statement |

## Child Productions

| Child Production |
| --- |
| option |
| show_management_target |

## Binding Contract

The selected target must bind to a visible report descriptor. Rows and fields must be filtered or redacted according to privileges, sandbox root, and disclosure policy.

## Practical Notes

- `SHOW` is for compact inspection; use `DESCRIBE` for one-object detail.
- Tool-facing output should be stable, typed, and redacted.
- Hidden targets should not leak through counts or diagnostics.
