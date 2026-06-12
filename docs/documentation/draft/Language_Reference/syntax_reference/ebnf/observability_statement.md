# Observability Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for observability commands.

Generation task: `ebnf_observability_statement`

## Production

```ebnf
observability_statement ::=
      show_stmt
    | explain_stmt ;
```

## Meaning

`observability_statement` dispatches non-mutating inspection and plan-report commands. It can expose runtime, catalog, diagnostic, or plan metadata only through authorized result descriptors.

## Used By

| Parent Production |
| --- |
| native_statement |
| statement |

## Child Productions

| Child Production |
| --- |
| show_stmt |
| explain_stmt |

## Binding Contract

The binder must apply target visibility, descriptor resolution, sandbox root, disclosure policy, and protected-material redaction before rendering a report.

## Practical Notes

- Observability output is evidence for operators and tools, not direct catalog or storage authority.
- `EXPLAIN ANALYZE` executes the statement and therefore requires execution privileges as well as plan visibility.
