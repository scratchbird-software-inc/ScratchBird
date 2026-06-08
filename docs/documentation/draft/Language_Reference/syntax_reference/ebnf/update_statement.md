# Update Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `UPDATE` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns transaction finality.

Generation task: `ebnf_update_statement`

Parent reference: [UPDATE Statement](../update.md)

## Production

```ebnf
update_statement ::=
    UPDATE update_target
       SET assignment_list
       update_from_clause?
       where_clause?
       returning_clause? ;

update_target ::=
    qualified_name target_alias? ;

assignment_list ::=
    assignment ("," assignment)* ;

assignment ::=
      assignment_target "=" assignment_value
    | "(" assignment_target ("," assignment_target)* ")" "=" row_value_expression ;

assignment_target ::=
      identifier
    | qualified_identifier
    | path_target ;

assignment_value ::=
      expression
    | DEFAULT ;

update_from_clause ::=
    FROM table_expression ;

where_clause ::=
    WHERE predicate ;

returning_clause ::=
    RETURNING projection_list ;
```

## Meaning

`update_statement` recognizes a request to create replacement row versions in an updatable target. The grammar accepts a target rowset, one or more assignments, optional source rowsets, an optional filter, and optional result projection. It does not mutate rows by itself.

After parsing, the binder must resolve:

| Element | Binding requirement |
| --- | --- |
| Target | A table, updatable view, or descriptor-bound rowset target that admits updates. |
| Assignment targets | Writable target columns, structured path targets where admitted, and generation/identity restrictions. |
| Assignment values | Expression descriptors, `DEFAULT` rules, coercions, and protected-material policy. |
| Source rowsets | Descriptors used by assignment expressions or target qualification. |
| Predicate | A boolean descriptor evaluated under statement visibility and authorization. |
| `RETURNING` | Authorized result descriptors evaluated from replacement rows. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `dml_statement` | Places `UPDATE` in the data manipulation statement family. |
| `merge_matched_action` | Supplies the update action inside `MERGE`. |
| `conflict_action` | Supplies the conflict update action inside `UPSERT`. |

## Child Productions

| Child production | Role |
| --- | --- |
| `qualified_name` | Resolves the target object name. |
| `assignment_list` | Defines replacement values. |
| `table_expression` | Supplies source rows for expressions and qualification. |
| `predicate` | Filters target rows. |
| `projection_list` | Defines `RETURNING` output. |

## Ambiguity And Admission Rules

- A target row must have one deterministic replacement row outcome.
- Source rows that drive assignments must not ambiguously produce multiple values for one target row unless an admitted deterministic rule resolves the conflict.
- `DEFAULT` on the right side requests descriptor default or generation behavior; it is not a literal.
- Structured path assignment is valid only through a descriptor that defines path update semantics.
- `RETURNING` is a statement result and not commit proof.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+`.
- A production that names an object reference must still pass resolver, authorization, sandbox, policy, SBLR admission, conflict, and MGA transaction checks.
