# Describe Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for authorized object inspection.

Generation task: `ebnf_describe_statement`

## Production

```ebnf
describe_stmt ::=
    "DESCRIBE" describe_target object_ref describe_option_list? ;

describe_target ::=
      "DATABASE"
    | "FILESPACE"
    | "SCHEMA"
    | "TABLE"
    | "VIEW"
    | "MATERIALIZED" "VIEW"
    | "INDEX"
    | "DOMAIN"
    | "TYPE" "DESCRIPTOR"
    | "SEQUENCE"
    | "FUNCTION"
    | "PROCEDURE"
    | "TRIGGER"
    | "POLICY"
    | "MASK"
    | "RLS"
    | "USER"
    | "ROLE"
    | "GROUP"
    | "GRANT"
    | "BRIDGE" ;

describe_option_list ::=
    "WITH" describe_option ("," describe_option)* ;

describe_option ::=
      "DEPENDENCIES"
    | "PRIVILEGES"
    | "STORAGE"
    | "SECURITY"
    | "READINESS"
    | "DIAGNOSTICS"
    | "SOURCE" ;
```

## Meaning

`describe_stmt` returns an authorized metadata projection for one object or one resolved overload. It is an inspection surface only. It does not grant catalog authority, storage authority, security authority, or transaction authority.

## Used By

| Parent Production |
| --- |
| statement |
| ddl_statement |
| function_lifecycle_statement |
| object_lifecycle_statement |

## Child Productions

| Child Production |
| --- |
| object_ref |
| describe_target |
| describe_option_list |

## Binding Contract

The binder must resolve the target object under the caller's schema root, privileges, disclosure policy, and transaction snapshot. Protected metadata must be omitted or redacted when the caller can observe the object but cannot inspect all fields.

## Practical Notes

- `DESCRIBE FUNCTION` may require a signature when the function name identifies multiple overloads.
- `DESCRIBE` output is a projection of catalog state and runtime readiness, not direct catalog storage.
- Recovery-required, unavailable, or unlicensed states must be reported through the ordinary message-vector contract.
