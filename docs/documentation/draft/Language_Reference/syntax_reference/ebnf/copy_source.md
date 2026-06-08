# Copy Source EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the endpoint portion of a `COPY` statement.

Generation task: `ebnf_copy_source`

Parent reference: [COPY Streaming Import And Export](../copy.md)

## Production

```ebnf
copy_endpoint ::=
      STDIN
    | STDOUT
    | STREAM parameter_ref
    | LOCATION location_ref ;
```

## Meaning

The copy endpoint identifies where stream data comes from or goes to. For large insert streaming, use `STDIN` or an admitted `STREAM` parameter. Server-local locations are policy-controlled and should not be used as portable application syntax.

## Admission Notes

- Endpoint text is resolver input only.
- Stream handles must be typed and authorized.
- The parser must not embed source handles as durable authority.
