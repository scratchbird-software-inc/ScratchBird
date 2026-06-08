# Document Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for document commands.

Generation task: `ebnf_document_statement`

## Production

```ebnf
document_statement ::=
    "DOCUMENT" document_action document_target document_payload? return_clause? statement_option_list? ;

document_action ::=
      "GET"
    | "PUT"
    | "PATCH"
    | "DELETE"
    | "QUERY"
    | "VALIDATE" ;

document_target ::=
    qualified_name ;

document_payload ::=
      document_key_payload
    | document_put_payload
    | document_patch_payload
    | document_query_payload
    | document_validate_payload ;

document_key_payload ::=
    "KEY" expression ;

document_put_payload ::=
    "KEY" expression "VALUE" expression ;

document_patch_payload ::=
    "KEY" expression document_patch_operation+ ;

document_patch_operation ::=
      "SET" "PATH" path_expression "=" expression
    | "REMOVE" "PATH" path_expression ;

document_query_payload ::=
    multimodel_where_clause? ;

document_validate_payload ::=
    "VALUE" expression ;
```

## Meaning

`document_statement` recognizes document get, put, patch, delete, query, and validate commands. Document paths, values, keys, and validation profiles are untrusted inputs until the binder maps them to descriptors and an admitted operation.

## Used By

| Parent Production |
| --- |
| nosql_statement |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| expression |
| path_expression |
| return_clause |
| statement_option_list |
| multimodel_where_clause |

## Binding Contract

The target must resolve to a document-capable descriptor. Missing path and JSON null behavior must be preserved according to the bound operation. Mutations must update document payloads and document index evidence transactionally.

## Practical Notes

- A document path is bound operation input, not storage authority.
- `RETURN` controls the typed result projection.
- Document indexes are candidate evidence and require final recheck.
