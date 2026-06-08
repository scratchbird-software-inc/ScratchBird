# FSPE-003 Validation Result

Status: complete
Search key: `FSPE-003-CST-AST-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 21:17:40 EDT
Owning slice: `FSPE-003`

## Implemented Outputs

- Added CST source ranges, token nodes, root node coverage, and lossless token reconstruction in `project/src/parsers/sbsql_worker/cst/`.
- Added AST source text, canonical render artifact, statement hash, root/source-artifact nodes, token ranges, and statement-family metadata in `project/src/parsers/sbsql_worker/ast/`.
- Updated downstream parser paths to keep behavior stable while consuming the expanded token/CST/AST artifacts.
- Added CST/AST conformance probe at `project/tests/sbsql_parser_worker/generated/cst_ast/sbsql_cst_ast_conformance_probe.cpp`.
- CTest label: `sbsql_cst_ast_conformance`.

## Coverage

The conformance probe validates:

- lossless CST reconstruction from preserved token raw text;
- CST root/node coverage for every non-end token;
- preserved comments, whitespace, quoted identifiers, temporal literals, statement terminators, and ranges;
- AST source text and canonical render preservation;
- AST root ranges that skip leading trivia while preserving source artifacts;
- current vertical-slice statement-family classification for query, values, DML, DDL/catalog, SHOW, SET, transaction, EXECUTE, and CALL families;
- recovery artifacts for empty/trivia-only and unknown/refusal-style statements.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation -DSB_BUILD_SBSQL_PARSER_WORKER=ON -DSB_BUILD_SBSQL_PARSER_WORKER_TESTS=ON -DSB_BUILD_SBU_SBSQL_PARSER_SUPPORT=ON
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_cst_ast_conformance_probe
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_cst_ast_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_cst_ast_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `19/19` passed.

## Boundary Notes

`FSPE-003` closes CST/AST source-artifact scaffolding and current statement-family AST metadata. Full expression grammar, builtin/operator semantics, statement grammar coverage, binder descriptor authority, SBLR lowering, UDR, server, and engine behavior remain owned by later slices.
