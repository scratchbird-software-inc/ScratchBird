# Group By Clause EBNF Production

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `ebnf_group_by_clause`


## Production

```ebnf
group_by_clause ::=
    "GROUP" "BY" group_by_item ("," group_by_item)* group_by_modifier? ;

group_by_item ::=
      expression
    | column_ordinal
    | grouping_set ;

group_by_modifier ::=
      "WITH" "ROLLUP"
    | "WITH" "CUBE" ;

grouping_set ::=
      "GROUPING" "SETS" "(" grouping_set_list ")"
    | "ROLLUP" "(" expression_list ")"
    | "CUBE" "(" expression_list ")" ;

grouping_set_list ::=
    grouping_set_element ("," grouping_set_element)* ;

grouping_set_element ::=
      "(" expression_list? ")"
    | expression ;
```

## Meaning

`group_by_clause` is an SBsql grammar production. It is part of contextual parsing only; it does not by itself authorize execution. After parsing, the surrounding statement or expression must bind to descriptors, UUID catalog objects, security context, transaction context, and an admitted SBLR operation family.

## Used By

| Parent Production |
| --- |
| select_statement |
| query_statement |

## Child Productions

| Child Production |
| --- |
| expression |
| expression_list |
| column_ordinal |

## Binding Contract

`GROUP BY` partitions the visible input rowset into aggregate groups. The binder must resolve each grouping expression to an equality/grouping descriptor. Non-aggregate projections in the same query block must be grouping keys, deterministic expressions over grouping keys and aggregate results, constants, parameters, or expressions proven functionally dependent on the grouping keys.

## Practical Notes

- Quoted uppercase terms are literal contextual tokens.
- Lowercase names refer to other productions or binder-level symbols.
- Optional parts use `?`; repeated lists use `*` or `+` according to the grammar.
- A production that names an object reference must still pass resolver and authorization checks.
- `GROUP BY` does not define output order. Use `ORDER BY` for deterministic presentation order.
- Advanced grouping forms require an admitted grouping metadata descriptor that can distinguish real null keys from subtotal placeholders.
