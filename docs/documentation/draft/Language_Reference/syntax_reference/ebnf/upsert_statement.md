# Upsert Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `UPSERT` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns transaction finality.

Generation task: `ebnf_upsert_statement`

Parent reference: [MERGE And UPSERT](../merge_and_upsert.md)

## Production

```ebnf
upsert_statement ::=
    UPSERT INTO upsert_target insert_column_list?
    upsert_source
    conflict_clause?
    returning_clause? ;

upsert_target ::=
    qualified_name target_alias? ;

upsert_source ::=
      values_insert_source
    | query_insert_source
    | default_values_source ;

conflict_clause ::=
    ON CONFLICT conflict_target? conflict_action ;

conflict_target ::=
      "(" conflict_key ("," conflict_key)* ")"
    | ON CONSTRAINT identifier ;

conflict_key ::=
      identifier
    | qualified_identifier
    | expression ;

conflict_action ::=
      DO NOTHING
    | DO UPDATE SET assignment_list where_clause? ;

returning_clause ::=
    RETURNING projection_list ;
```

## Meaning

`upsert_statement` recognizes an insert request with an admitted conflict action. It may insert a new row, update an existing conflict row, or do nothing. The grammar accepts conflict syntax, but conflict authority comes from catalog-bound uniqueness or conflict descriptors.

After parsing, the binder must resolve:

| Element | Binding requirement |
| --- | --- |
| Target | A writable rowset target admitting insert and any required conflict action. |
| Insert source | Values, query rows, or default-only row construction compatible with the target. |
| Conflict target | A unique or otherwise admitted conflict descriptor. |
| Conflict action | No-op or update assignments with optional predicate. |
| `RETURNING` | Authorized result descriptors for inserted or updated rows. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `dml_statement` | Places `UPSERT` in the data manipulation statement family. |

## Child Productions

| Child production | Role |
| --- | --- |
| `qualified_name` | Resolves the target object name. |
| `values_insert_source` | Supplies explicit insert rows. |
| `query_insert_source` | Supplies query-produced insert rows. |
| `default_values_source` | Supplies default-only rows. |
| `assignment_list` | Defines conflict update assignments. |
| `where_clause` | Qualifies the conflict update action. |
| `projection_list` | Defines `RETURNING` output. |

## Ambiguity And Admission Rules

- A conflict target must bind to catalog metadata; text names are resolver input only.
- Concurrent conflicts must be rechecked under the active transaction profile.
- `DO NOTHING` leaves the existing target row unchanged.
- `DO UPDATE` uses the same engine-owned enforcement as standalone `UPDATE`.
- `RETURNING` is a statement result and not commit proof.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+`.
- A production that names an object reference must still pass resolver, authorization, sandbox, policy, SBLR admission, conflict, dependency, and MGA transaction checks.
