# Delete Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `DELETE` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns transaction finality.

Generation task: `ebnf_delete_statement`

Parent reference: [DELETE Statement](../delete.md)

## Production

```ebnf
delete_statement ::=
    DELETE FROM delete_target
    delete_using_clause?
    where_clause?
    returning_clause? ;

delete_target ::=
    qualified_name target_alias? ;

delete_using_clause ::=
    USING table_expression ;

where_clause ::=
    WHERE predicate ;

returning_clause ::=
    RETURNING projection_list ;
```

## Meaning

`delete_statement` recognizes a request to retire row versions from a deletable target. The grammar accepts a target rowset, optional qualifying source rowsets, an optional predicate, and optional result projection. It does not erase storage or reclaim pages by itself.

After parsing, the binder must resolve:

| Element | Binding requirement |
| --- | --- |
| Target | A table, updatable view, or descriptor-bound rowset target that admits deletion. |
| Using source | Source descriptors used to qualify target rows. |
| Predicate | A boolean descriptor evaluated under statement visibility and authorization. |
| `RETURNING` | Authorized result descriptors projected from the retiring row versions. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `dml_statement` | Places `DELETE` in the data manipulation statement family. |
| `merge_matched_action` | Supplies the delete action inside `MERGE`. |

## Child Productions

| Child production | Role |
| --- | --- |
| `qualified_name` | Resolves the target object name. |
| `table_expression` | Supplies qualifying source rows. |
| `predicate` | Filters target rows. |
| `projection_list` | Defines `RETURNING` output. |

## Ambiguity And Admission Rules

- A missing `WHERE` clause targets all visible and writable rows in the target, subject to policy.
- Source rowsets in `USING` can qualify target rows but cannot bypass target write authority.
- A row is retired under MGA transaction rules; physical cleanup is separate.
- Structured rowset deletion must obey descriptor dependency policy.
- `RETURNING` is a statement result and not commit proof.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+`.
- A production that names an object reference must still pass resolver, authorization, sandbox, policy, SBLR admission, conflict, dependency, and MGA transaction checks.
