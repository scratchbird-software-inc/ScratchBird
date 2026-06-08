# Copy Format EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the format portion of a `COPY` statement.

Generation task: `ebnf_copy_format`

Parent reference: [COPY Streaming Import And Export](../copy.md)

## Production

```ebnf
copy_format ::=
      CSV
    | JSONL
    | BINARY
    | FORMAT identifier ;
```

## Meaning

The format controls how stream frames are decoded or encoded by the admitted stream execution route. `CSV` is the default for `COPY ... FROM STDIN` when no format is supplied. `JSONL` declares one JSON value per line. `BINARY` and named formats require compatible descriptors and policy admission.

## Admission Notes

- Format names are descriptors or contextual tokens, not parser byte-decoding authority.
- Unsupported formats must fail before row data is accepted.
- Format conversion errors must be reported as stream or descriptor diagnostics.
