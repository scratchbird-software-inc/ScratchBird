# METADATA RESULT SHAPE REPORT

Status: complete
Search key: `FSPE-METADATA_RESULT_SHAPE_REPORT`
Owning slice: `FSPE-012F`

## Summary

FSPE-012F materialized `sbsql_metadata_result_shape_gate` as a parser/server metadata and result-shape CTest gate.

The gate verifies:

- Parser/binder result-shape keys for rowsets, management reports, routine results, and command-status statements.
- Required-right and diagnostic-shape metadata remain attached to bound statements without making the parser an execution authority.
- Server synthetic rowset metadata includes canonical column descriptors for visible value columns and hidden/system columns.
- Column names, aliases, object UUIDs, canonical types, domains, precision, scale, length, nullability, generated/computed flags, hidden/system flags, charset, and collation fields are emitted.
- Command completion tags, affected-row counts, returned-row counts, warning/notice arrays, and cursor capability metadata are emitted on rowset packets.
- Multi-result sequencing carries result-set metadata, command tags, affected-row counts, and deterministic finality.
- Warning and partial-result streams carry non-aborting warning diagnostics and completed-with-warnings finality.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_metadata_result_shape_gate` |
| Test source | `project/tests/sbsql_parser_worker/generated/result_shape/sbsql_metadata_result_shape_gate.cpp` |
| CMake target | `sbsql_metadata_result_shape_gate` |
| Supporting labels | `sbsql_result_shape_gate`; `sbsql_parser_worker` |
| Production search key | `SB_SERVER_SBLR_DISPATCH_RESULTS` |
| Unexpected failures | 0 |

## Boundary

The gate validates metadata contracts at parser and server boundaries. It does not make SQL text, parser ASTs, or parser-generated metadata authoritative inside the engine. Engine execution remains SBLR/internal-procedure based, and authority-bearing object identity continues to use UUID and server/engine-owned catalog/security/transaction context.

## Result

Metadata, result-shape, command-tag, warning, multi-result, and cursor metadata behavior now have a runnable validation gate and are included in the full parser-worker CTest label.
