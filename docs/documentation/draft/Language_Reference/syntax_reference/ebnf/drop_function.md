# Drop Function EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for retiring function catalog objects.

Generation task: `ebnf_drop_function`

## Production

```ebnf
drop_function ::=
    "DROP" "FUNCTION" function_ref drop_behavior? ;

drop_behavior ::=
      "RESTRICT"
    | "CASCADE" ;
```

## Meaning

`drop_function` removes the visible function binding only after overload resolution, privilege checks, dependency checks, transaction checks, and recovery checks pass.

## Used By

| Parent Production |
| --- |
| function_lifecycle_statement |
| drop_statement |

## Child Productions

| Child Production |
| --- |
| function_ref |
| qualified_name |
| type_descriptor |

## Binding Contract

The binder must resolve a single visible overload unless the statement form explicitly names an overload set. `RESTRICT` refuses the drop when dependents exist. `CASCADE` must enumerate and authorize every dependent action before commit.

## Practical Notes

- Dropping a function is transactional and rollback-safe.
- Existing transactions continue to see the version admitted by their snapshot.
- Dropping a function must invalidate dependent plans, expression indexes, generated columns, routines, triggers, and metadata projections.
