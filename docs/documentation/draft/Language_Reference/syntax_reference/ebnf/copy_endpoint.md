# Copy Endpoint EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the endpoint classification for `COPY` import/export routes.

Generation task: `ebnf_copy_endpoint`

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

`copy_endpoint` is the source or destination boundary for a bulk stream. `STDIN` and `STDOUT` attach to the current client statement. `STREAM` attaches to a typed stream parameter. `LOCATION` attaches to a policy-admitted server-side location.

## Admission Notes

- `STDIN` is the normal declaration for large insert streaming.
- `STDOUT` is the normal declaration for streamed export.
- `LOCATION` is administrative and policy-controlled.
- Endpoint descriptors must not leak local file paths or protected material.
