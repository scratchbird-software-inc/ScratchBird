# Create Function EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for creating function catalog objects.

Generation task: `ebnf_create_function`

## Production

```ebnf
create_function_stmt ::=
    "CREATE" "FUNCTION" function_signature
    "RETURNS" function_return_descriptor
    function_attribute_list?
    "AS" function_body ;

function_signature ::=
    qualified_name function_parameter_list? ;

function_parameter_list ::=
    "(" function_parameter ("," function_parameter)* ")" ;

function_parameter ::=
    parameter_name type_descriptor parameter_default? ;

parameter_default ::=
    "DEFAULT" expression ;

function_return_descriptor ::=
      type_descriptor
    | "ROW" "(" result_column ("," result_column)* ")"
    | "TABLE" "(" result_column ("," result_column)* ")" ;

result_column ::=
    identifier type_descriptor ;

function_attribute_list ::=
    function_attribute+ ;

function_attribute ::=
      "DETERMINISTIC"
    | "NOT" "DETERMINISTIC"
    | "IMMUTABLE"
    | "STABLE"
    | "VOLATILE"
    | "RETURNS" "NULL" "ON" "NULL" "INPUT"
    | "CALLED" "ON" "NULL" "INPUT"
    | "SECURITY" "DEFINER"
    | "SECURITY" "INVOKER"
    | "COST" numeric_literal
    | "LANGUAGE" identifier
    | "JIT" "ELIGIBLE"
    | "AOT" "ELIGIBLE"
    | "EXTERNAL" "NAME" string_literal ;

function_body ::=
      procedural_block
    | "RETURN" expression
    | "EXTERNAL" "UDR" function_udr_binding ;
```

## Meaning

`create_function_stmt` creates a durable function catalog row. The create route must store the function UUID, resolver name, overload signature, descriptors, source reference, executable representation, dependency graph, grants, and readiness metadata.

## Used By

| Parent Production |
| --- |
| function_lifecycle_statement |
| create_statement |

## Child Productions

| Child Production |
| --- |
| function_signature |
| type_descriptor |
| expression |
| procedural_block |
| function_udr_binding |
| qualified_name |
| identifier |
| numeric_literal |
| string_literal |

## Binding Contract

The function body must bind to parameters, local declarations, descriptor-compatible expressions, return paths, dependencies, and security context. A table-valued function must declare a rowset return descriptor before it can be used as a table expression.

## Practical Notes

- Default parameter expressions are bound at create time and evaluated under call-time rules.
- `SECURITY DEFINER` requires explicit policy admission and audit metadata.
- JIT/AOT eligibility is stored as readiness metadata, not as a promise that native compilation will occur.
