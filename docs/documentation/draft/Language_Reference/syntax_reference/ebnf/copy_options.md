# Copy Options EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the options portion of a `COPY` statement.

Generation task: `ebnf_copy_options`

Parent reference: [COPY Streaming Import And Export](../copy.md)

## Production

```ebnf
copy_options ::=
    WITH copy_option ("," copy_option)* ;

copy_option ::=
      HEADER
    | NO HEADER
    | DELIMITER string_literal
    | NULL string_literal
    | QUOTE string_literal
    | ESCAPE string_literal
    | ENCODING identifier
    | BATCH SIZE integer_literal
    | REJECTS STREAM parameter_ref
    | MAX ERRORS integer_literal
    | ON ERROR STOP
    | ON ERROR QUARANTINE ;
```

## Meaning

Copy options describe stream decoding, batching, reject handling, and error policy. They must be admitted before row frames are accepted.

## Admission Notes

- `WITH HEADER` marks the first CSV row as column metadata rather than data.
- `BATCH SIZE` is a resource and transaction-policy hint, not commit authority.
- `REJECTS STREAM` must be a typed authorized stream handle.
- Error policy must fail closed when the outcome is uncertain.
