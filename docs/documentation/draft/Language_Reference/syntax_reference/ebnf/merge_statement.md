# Merge Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `MERGE` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns transaction finality.

Generation task: `ebnf_merge_statement`

Parent reference: [MERGE And UPSERT](../merge_and_upsert.md)

## Production

```ebnf
merge_statement ::=
    MERGE INTO merge_target
    USING merge_source
       ON merge_search_condition
    merge_when_clause+
    returning_clause? ;

merge_target ::=
    qualified_name target_alias? ;

merge_source ::=
      table_expression
    | "(" query_statement ")" source_alias? ;

merge_search_condition ::=
    predicate ;

merge_when_clause ::=
      WHEN MATCHED merge_when_condition? THEN merge_matched_action
    | WHEN NOT MATCHED merge_when_condition? THEN merge_not_matched_action ;

merge_when_condition ::=
    AND predicate ;

merge_matched_action ::=
      UPDATE SET assignment_list
    | DELETE
    | DO NOTHING ;

merge_not_matched_action ::=
      INSERT insert_column_list? VALUES row_constructor
    | DO NOTHING ;

returning_clause ::=
    RETURNING projection_list ;
```

## Meaning

`merge_statement` recognizes a conditional write that classifies source rows against target rows. Matched rows may update, delete, or do nothing. Not-matched rows may insert or do nothing. The grammar accepts the action list, but the engine must prove that each target row has a deterministic action outcome.

After parsing, the binder must resolve:

| Element | Binding requirement |
| --- | --- |
| Target | A writable rowset target with admitted insert, update, delete, or no-op action routes as required by the clauses. |
| Source | A rowset-producing source with stable descriptors. |
| Match predicate | A boolean descriptor used to classify source and target rows. |
| Clause predicates | Boolean descriptors evaluated after matched/not-matched classification. |
| Actions | Assignment, delete, insert, and default descriptors required by each branch. |
| `RETURNING` | Authorized result descriptors for affected rows. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `dml` (statement family) | Places `MERGE` in the data manipulation statement family. |

## Child Productions

| Child production | Role |
| --- | --- |
| `qualified_name` | Resolves the target object name. |
| `table_expression` | Supplies source rows. |
| `query_statement` | Supplies derived source rows. |
| `predicate` | Defines match and clause conditions. |
| `assignment_list` | Defines update actions. |
| `row_constructor` | Defines insert actions. |
| `projection_list` | Defines `RETURNING` output. |

## Ambiguity And Admission Rules

- Clause order matters when more than one clause could apply.
- A target row must not be changed more than once unless an admitted deterministic rule defines that behavior.
- A source row that could match multiple target rows must be handled by a deterministic match contract or refused.
- Insert, update, and delete branches use the same engine-owned enforcement as the standalone statements.
- `RETURNING` is a statement result and not commit proof.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+`.
- A production that names an object reference must still pass resolver, authorization, sandbox, policy, SBLR admission, conflict, dependency, and MGA transaction checks.
