# Search Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for search commands.

Generation task: `ebnf_search_statement`

## Production

```ebnf
fulltext_search_query ::=
    "SEARCH" search_target search_payload return_clause? statement_option_list? ;

search_target ::=
    qualified_name ;

search_payload ::=
    "FOR" expression search_field_clause? multimodel_where_clause? limit_clause? ;

search_field_clause ::=
    "FIELDS" identifier ("," identifier)* ;
```

## Meaning

`fulltext_search_query` recognizes descriptor-bound search commands. Search text, fields, analyzer profile, scoring, snippets, matched fields, and refresh behavior are bound operation inputs, not execution authority.

## Used By

| Parent Production |
| --- |
| nosql_statement |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| expression |
| identifier |
| return_clause |
| statement_option_list |
| multimodel_where_clause |
| limit_clause |

## Binding Contract

The target must resolve to a search-capable descriptor. Analyzer profile, field list, filters, score descriptor, snippet policy, and result projection must be admitted before execution. Search score is ranking evidence and requires final row recheck.

## Practical Notes

- Search query text is untrusted input.
- Snippet rendering must obey protected-material policy.
- Stable ordering requires the statement result descriptor or an explicit ordering surface.
