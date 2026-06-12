# Alter Function EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for modifying function metadata.

Generation task: `ebnf_alter_function`

## Production

```ebnf
alter_routine_stmt ::=
    "ALTER" "FUNCTION" function_ref alter_function_action ;

alter_function_action ::=
      "SET" function_attribute
    | "RESET" function_attribute_name
    | "SET" "EXTERNAL" "NAME" string_literal
    | "SET" "LANGUAGE" identifier
    | "SET" "JIT" "ELIGIBLE"
    | "SET" "AOT" "ELIGIBLE"
    | "SET" "SECURITY" ("DEFINER" | "INVOKER")
    | "SET" "COST" numeric_literal ;

function_ref ::=
    qualified_name function_argument_descriptor_list? ;

function_argument_descriptor_list ::=
    "(" type_descriptor ("," type_descriptor)* ")" ;
```

## Meaning

`alter_routine_stmt` changes admitted function metadata without changing durable identity. Signature-changing edits require a recreate route because overload identity, dependency graph, and executable representation must be rebound as a unit.

## Used By

| Parent Production |
| --- |
| function_lifecycle_statement |
| alter_statement |

## Child Productions

| Child Production |
| --- |
| function_ref |
| function_attribute |
| type_descriptor |
| qualified_name |
| identifier |
| numeric_literal |
| string_literal |

## Binding Contract

The binder must resolve a single visible overload. Changes must pass privilege, dependency, policy, and transaction checks. Dependent plans, indexes, generated columns, routines, triggers, and metadata caches must be invalidated or refused where required.

## Practical Notes

- Ambiguous overload references must be refused.
- Security and volatility changes can affect optimizer and authorization behavior.
- UDR binding changes must pass ABI, version, descriptor, and trust checks before commit.
