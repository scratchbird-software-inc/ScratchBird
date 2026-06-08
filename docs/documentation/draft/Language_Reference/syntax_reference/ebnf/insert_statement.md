# Insert Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `INSERT` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns transaction finality.

Generation task: `ebnf_insert_statement`

Parent reference: [INSERT Statement](../insert.md)

## Production

```ebnf
insert_statement ::=
    INSERT INTO insert_target insert_column_list? insert_source returning_clause? ;

insert_target ::=
    qualified_name target_alias? ;

insert_column_list ::=
    "(" insert_column ("," insert_column)* ")" ;

insert_column ::=
      identifier
    | qualified_identifier
    | path_target ;

insert_source ::=
      values_insert_source
    | query_insert_source
    | default_values_source
    | multimodel_insert_source ;

values_insert_source ::=
    VALUES row_constructor ("," row_constructor)* ;

row_constructor ::=
      "(" insert_value ("," insert_value)* ")"
    | ROW "(" insert_value ("," insert_value)* ")" ;

insert_value ::=
      expression
    | DEFAULT ;

query_insert_source ::=
    query_statement ;

default_values_source ::=
    DEFAULT VALUES ;

returning_clause ::=
    RETURNING projection_list ;
```

## Meaning

`insert_statement` recognizes a request to create row versions in an insertable target. The grammar accepts target names, optional target columns, row sources, and optional result projection. It does not by itself authorize writes or decide row visibility.

After parsing, the binder must resolve:

| Element | Binding requirement |
| --- | --- |
| Target | A table, updatable view, or descriptor-bound rowset target that admits insertion. |
| Column list | Target descriptors, ordinals, structured path targets where admitted, and generated/default rules. |
| Values source | Expression descriptors, parameter descriptors, row counts, and row constructor shapes. `VALUES (a, b), (c, d)` is one insert source containing multiple row constructors. |
| Query source | Query result descriptors assignable to the target columns. |
| Default source | Default, identity, generated, and nullable-column rules for a complete row. |
| `RETURNING` | Authorized result descriptors evaluated after row construction. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `dml_statement` | Places `INSERT` in the data manipulation statement family. |
| `script_statement` | Allows `INSERT` in scripts and statement blocks where DML is admitted. |

## Child Productions

| Child production | Role |
| --- | --- |
| `qualified_name` | Resolves the target object name. |
| `target_alias` | Provides a contextual target alias where admitted. |
| `expression` | Supplies explicit inserted values. |
| `query_statement` | Supplies rows from a rowset-producing query. |
| `projection_list` | Defines `RETURNING` output. |

## Ambiguity And Admission Rules

- `DEFAULT` is contextual inside inserted values and is not a string literal.
- A missing column list means the source must bind to the target's admitted insert column order.
- Every row constructor in a multi-row `VALUES` source must match the target column count and bind by ordinal.
- Structured path targets are valid only when the target descriptor defines insertable structured fields.
- Multimodel inserts are rowset inserts only when the target exposes a row descriptor; command-style multimodel operations use their own statement family.
- `RETURNING` is a statement result and not commit proof.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+`.
- A production that names an object reference must still pass resolver, authorization, sandbox, policy, SBLR admission, and MGA transaction checks.
