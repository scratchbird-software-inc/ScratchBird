# FSPE-002 Validation Result

Status: complete
Search key: `FSPE-002-LEXER-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 21:12:23 EDT
Owning slice: `FSPE-002`

## Implemented Outputs

- Expanded SBSQL token model in `project/src/parsers/sbsql_worker/lexer/lexer.hpp`.
- Implemented lexical preservation and classification in `project/src/parsers/sbsql_worker/lexer/lexer.cpp`.
- Updated AST and test-wire paths to ignore preserved trivia tokens while retaining source artifacts.
- Added generated-style lexer conformance probe at `project/tests/sbsql_parser_worker/generated/lexer/sbsql_lexer_conformance_probe.cpp`.
- CTest label: `sbsql_lexer_conformance`.

## Coverage

The lexer now preserves byte offsets, line/column spans, raw token text, quoted flags, literal family, keyword class, and render hints for:

- whitespace, line comments, nested block comments, parser directives, and statement terminators;
- bare, delimited, bracketed, and donor-quoted identifiers;
- reserved/contextual/private/donor/refusal keyword classes;
- integer, unsigned, int128, uint128, real128, decimal, float, hex, binary, and octal numeric literals;
- string, national string, escaped string, binary, UUID, temporal, document/JSON, vector, regex, and range literals;
- positional/named/ordinal parameters, variables, operators, symbols, and meta-command introducers.

The lexer conformance probe also tokenizes all `541` current lexer-owned grammar rows from `SURFACE_IMPLEMENTATION_BACKLOG.csv`.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation -DSB_BUILD_SBSQL_PARSER_WORKER=ON -DSB_BUILD_SBSQL_PARSER_WORKER_TESTS=ON -DSB_BUILD_SBU_SBSQL_PARSER_SUPPORT=ON
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_lexer_conformance_probe
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_lexer_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_lexer_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `18/18` passed.

## Boundary Notes

`FSPE-002` closes tokenization and lexical artifact preservation. CST shape, AST node fidelity, grammar production mapping, expression parsing, statement parsing, binding, lowering, UDR, server, and engine behavior remain owned by `FSPE-003` and later slices.
