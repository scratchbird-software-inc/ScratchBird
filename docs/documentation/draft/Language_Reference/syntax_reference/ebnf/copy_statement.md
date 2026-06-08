# Copy Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `COPY` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the import/export route, and the engine owns transaction finality.

Generation task: `ebnf_copy_statement`

Parent reference: [COPY Streaming Import And Export](../copy.md)

## Production

```ebnf
copy_statement ::=
    COPY copy_target copy_direction copy_endpoint copy_format? copy_options? returning_clause? ;

copy_target ::=
      table_ref copy_column_list?
    | QUERY "(" query_statement ")" ;

copy_column_list ::=
    "(" identifier ("," identifier)* ")" ;

copy_direction ::=
      FROM
    | TO ;

copy_endpoint ::=
      STDIN
    | STDOUT
    | STREAM parameter_ref
    | LOCATION location_ref ;

copy_format ::=
      CSV
    | JSONL
    | BINARY
    | FORMAT identifier ;

copy_options ::=
    WITH copy_option ("," copy_option)* ;
```

## Meaning

`copy_statement` recognizes a bulk import or export declaration. Large insert streaming uses `COPY target FROM STDIN`. The statement opens an import/export plan; subsequent row bytes are stream frames, not SQL text.

## Used By

| Parent production | Purpose |
| --- | --- |
| `dml_statement` | Places `COPY` in the data manipulation statement family. |

## Child Productions

| Child production | Role |
| --- | --- |
| `copy_source` | Defines source or destination endpoint shape. |
| `copy_format` | Defines stream format. |
| `copy_options` | Defines header, batching, reject, and error policy options. |

## Admission Notes

- `COPY ... FROM` requires write authority on the target.
- `COPY ... TO` requires read authority on the source rowset.
- The parser plans and lowers the operation; it does not decode bytes or persist rows.
- Local row visibility follows MGA commit or rollback finality.
