# UDR COVERAGE REPORT

Status: complete
Search key: `FSPE-UDR_COVERAGE_REPORT`
Owning slice: `FSPE-008`

## Scope

This report records trusted parser-support UDR coverage for `sbu_sbsql_parser_support`. The UDR is trusted only as an engine-side parser-support package: it validates, parses, describes, normalizes, and decompiles under engine-supplied context and never executes SQL or mutates engine state directly.

## Covered Entry Points

| Entry point | Coverage evidence |
| --- | --- |
| `validate_syntax` | Valid SBSQL syntax is accepted through the shared parser pipeline. |
| `normalize` | Input is normalized after parser validation without executing SQL. |
| `parse_to_sblr` | Missing engine context fails closed; trusted context returns SBLR envelope evidence. |
| `parse_expression` | Missing descriptor context fails closed; engine descriptor context succeeds. |
| `describe_statement` | Missing engine context fails closed; trusted context returns statement metadata. |
| `decompile_sblr` | Normal policy refuses; explicit debug policy returns redacted text only. |
| Debug capability reporting | Policy controlled and redacted. |

## Validation

| Command | Result |
| --- | --- |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbu_sbsql_parser_support_conformance --output-on-failure` | passed 1/1 |

## Boundary

The supported route is engine-internal SBSQL text request to `sbu_sbsql_parser_support`, then engine-supplied resolver/descriptor/security context, then SBLR/message-vector output. SQL text is not executed inside the UDR, and authority remains server/engine-owned by UUID, descriptors, security, transaction, policy, and MGA rules.
