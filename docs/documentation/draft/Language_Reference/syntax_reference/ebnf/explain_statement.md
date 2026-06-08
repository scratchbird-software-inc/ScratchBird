# Explain Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for plan inspection.

Generation task: `ebnf_explain_statement`

## Production

```ebnf
explain_statement ::=
    "EXPLAIN" explain_option_list? query_statement ;

explain_option_list ::=
    ("ANALYZE" | "PLAN" | "VERBOSE" | "COSTS" | "BUFFERS" | "TIMING" | "FORMAT" identifier)* ;
```

## Meaning

`explain_statement` returns an authorized plan report. `ANALYZE` executes the statement and adds measured execution evidence. Plan reports are diagnostic evidence and do not change query authority.

## Used By

| Parent Production |
| --- |
| observability_statement |

## Child Productions

| Child Production |
| --- |
| query_statement |
| identifier |

## Binding Contract

The explained query must bind normally. The caller must have permission to see the target objects and the requested plan details. `EXPLAIN ANALYZE` also requires permission to execute the query.

## Practical Notes

- Plan output must redact hidden object names, protected predicates, protected values, and unauthorized row counts.
- Candidate evidence from indexes, document paths, vectors, search, graph, or bridge sources remains evidence only.
- `EXPLAIN ANALYZE` must preserve transaction and recovery invariants.
