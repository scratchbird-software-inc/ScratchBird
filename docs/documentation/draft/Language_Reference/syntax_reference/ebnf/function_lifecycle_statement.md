# Function Lifecycle Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for function lifecycle statements without treating SQL text as engine authority.

Generation task: `ebnf_function_lifecycle_statement`

## Production

```ebnf
function_lifecycle_statement ::=
      create_function_stmt
    | alter_routine_stmt
    | recreate_function
    | rename_object_stmt
    | comment_on_stmt
    | show_stmt
    | describe_stmt
    | drop_routine_stmt ;
```

## Meaning

`function_lifecycle_statement` groups the complete SBsql function object lifecycle. Function definitions create or modify durable catalog objects identified by UUID, with descriptor-bound signatures, executable SBLR or trusted UDR binding, source reference text, dependency graphs, security metadata, and transactional catalog visibility.

## Used By

| Parent Production |
| --- |
| ddl_catalog (statement family) |
| create_object |
| alter_object_stmt |
| drop_object_stmt |

## Child Productions

| Child Production |
| --- |
| create_function_stmt |
| alter_routine_stmt |
| recreate_function |
| rename_object_stmt |
| comment_on_stmt |
| show_stmt |
| describe_stmt |
| drop_routine_stmt |

## Binding Contract

The grammar can only recognize the statement shape. The binder must resolve names, overload signatures, parameter descriptors, return descriptors, security mode, dependency graph, UDR package metadata where present, and transaction context. The engine verifier must admit the resulting SBLR or trusted binding before the function becomes executable.

## Practical Notes

- Function identity is UUID based; names and signatures are resolver input.
- The original source text is reference material; execution uses encoded behavior.
- Function lifecycle changes are transactional and rollback-safe.
- Dependent plans, expression indexes, generated columns, routines, triggers, and metadata projections must be invalidated or refused when function metadata changes.
