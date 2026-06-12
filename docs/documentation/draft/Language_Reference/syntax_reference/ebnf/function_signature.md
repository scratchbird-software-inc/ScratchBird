# Function Signature EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the shared grammar contract for identifying function overloads.

Generation task: `ebnf_function_signature`

## Production

```ebnf
function_signature ::=
    qualified_name function_parameter_list? ;

function_ref ::=
    qualified_name function_argument_descriptor_list? ;

function_parameter_list ::=
    "(" function_parameter ("," function_parameter)* ")" ;

function_parameter ::=
    parameter_name type_descriptor parameter_default? ;

function_argument_descriptor_list ::=
    "(" type_descriptor ("," type_descriptor)* ")" ;

parameter_default ::=
    "DEFAULT" expression ;
```

## Meaning

`function_signature` defines or references the descriptor shape used for overload identity. A function name alone can identify an overload set. A function name plus argument descriptors identifies a specific overload when resolution is unambiguous.

## Used By

| Parent Production |
| --- |
| create_function_stmt |
| alter_routine_stmt |
| recreate_function |
| drop_routine_stmt |
| describe_stmt |
| show_stmt |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| type_descriptor |
| expression |

## Binding Contract

Overload resolution is descriptor based. The binder must consider argument count, named arguments, defaults, exact descriptor matches, admitted coercions, visibility, sandbox root, and privileges. Ambiguous matches must be refused.

## Practical Notes

- Parameter names are local routine names, not durable identity by themselves.
- Parameter type/domain descriptors create catalog dependencies.
- Defaults are bound expressions and must remain descriptor compatible with their parameters.
