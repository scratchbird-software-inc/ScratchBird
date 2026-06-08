# Support Bundle Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for support bundle lifecycle commands.

Generation task: `ebnf_support_bundle_statement`

## Production

```ebnf
support_bundle_statement ::=
    "SUPPORT" "BUNDLE" support_bundle_action support_bundle_target? support_bundle_option_list? ;

support_bundle_action ::=
      "CREATE"
    | "DESCRIBE"
    | "VERIFY"
    | "EXPORT"
    | "DROP" ;

support_bundle_target ::=
      uuid_ref
    | identifier
    | "CURRENT" "DATABASE"
    | "CURRENT" "SESSION" ;

support_bundle_option_list ::=
    "WITH" support_bundle_option ("," support_bundle_option)* ;

support_bundle_option ::=
      "SCOPE" identifier
    | "REDACT" identifier
    | "INCLUDE" identifier
    | "EXCLUDE" identifier
    | "TO" "CLIENT" "STREAM"
    | option ;
```

## Meaning

`support_bundle_statement` creates, inspects, verifies, exports, and drops authorized diagnostic bundles. Bundle content is manifest-driven and redacted.

## Used By

| Parent Production |
| --- |
| management_statement |

## Child Productions

| Child Production |
| --- |
| uuid_ref |
| identifier |
| option |

## Binding Contract

Bundle scope, redaction mode, manifest, retention policy, export route, and caller authority must bind before collection or export. Protected fields must be redacted or omitted.

## Practical Notes

- Support bundles should be exportable to an authorized client stream.
- `VERIFY` checks manifest integrity and redaction proof.
- Bundle creation refusal must not leak hidden object names or protected values.
