# Key-Value Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for key-value commands.

Generation task: `ebnf_kv_statement`

## Production

```ebnf
keyvalue_op_stmt ::=
    "KEYVALUE" kv_action kv_target kv_payload? return_clause? statement_option_list? ;

kv_action ::=
      "GET"
    | "PUT"
    | "DELETE"
    | "INCREMENT"
    | "EXPIRE"
    | "SCAN"
    | "LIST"
    | "SET"
    | "MAP" ;

kv_target ::=
    qualified_name ;

kv_payload ::=
      kv_key_payload
    | kv_put_payload
    | kv_increment_payload
    | kv_expire_payload
    | kv_scan_payload ;

kv_key_payload ::=
    "KEY" expression ;

kv_put_payload ::=
    "KEY" expression "VALUE" expression ttl_clause? ;

kv_increment_payload ::=
    "KEY" expression "BY" expression ;

kv_expire_payload ::=
    "KEY" expression ttl_clause ;

kv_scan_payload ::=
    ("FROM" "KEY" expression)? ("TO" "KEY" expression)? limit_clause? ;

ttl_clause ::=
    "TTL" expression ;
```

## Meaning

`keyvalue_op_stmt` recognizes descriptor-bound key-value commands. Keys, values, versions, expiration policy, and collection behavior are typed surfaces, not raw storage layout.

## Used By

| Parent Production |
| --- |
| nosql_statement (multi_model family) |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| expression |
| return_clause |
| statement_option_list |
| limit_clause |

## Binding Contract

The target must resolve to a key-value capable descriptor. Key descriptors, value descriptors, TTL behavior, version checks, range bounds, and result projection must bind before execution.

## Practical Notes

- Range scans require descriptor-compatible start and end keys.
- TTL cleanup timing must not change transaction visibility.
- Conditional mutations must fail closed when versions or existence predicates do not match.
