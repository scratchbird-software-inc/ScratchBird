# FSPE-008 Validation Result

Status: complete
Search key: `FSPE-008-SBU-SBSQL-PARSER-SUPPORT-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 23:43:12 EDT
Owning slice: `FSPE-008`

## Implemented Outputs

- Expanded `sbu_sbsql_parser_support` trusted parser-support entry points for syntax validation, trusted-context parse-to-SBLR, expression parse description, statement description, normalization, redacted SBLR decompile, and policy-controlled debug capability reporting.
- Reused the same SBSQL parser pipeline for CST/AST/binder/lowering/verifier behavior under explicit engine context.
- Preserved fail-closed behavior for missing engine context, missing descriptor context, missing describe context, and disallowed decompile/debug policies.
- Labeled the existing UDR probe as the runnable conformance gate.
- CTest label: `sbu_sbsql_parser_support_conformance`.

## Coverage

The conformance probe validates:

- `validate_syntax` succeeds for valid SBSQL syntax;
- `normalize` trims and validates input without executing SQL;
- `parse_to_sblr` refuses missing context and succeeds only with explicit trusted engine context;
- trusted-context parse-to-SBLR returns SBLR envelope evidence and does not return executable SQL text;
- `parse_expression` refuses missing descriptor context and succeeds with engine descriptor context;
- `describe_statement` refuses missing engine context and returns statement metadata under trusted context;
- `decompile_sblr` refuses normal policy and returns only redacted debug text under explicit debug policy;
- debug capability reporting is policy-controlled and redacted.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbu_sbsql_parser_support_probe -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbu_sbsql_parser_support_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbu_sbsql_parser_support_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `23/23` passed.

## Boundary Notes

`sbu_sbsql_parser_support` is trusted only as an engine-side parser support package. It may produce SBLR/message vectors under engine-supplied context; it must not execute SQL or mutate engine state directly.

The supported route is:

```text
engine-internal trusted SBSQL text request
  -> sbu_sbsql_parser_support
  -> engine-supplied resolver/descriptor/security context
  -> SBLR/message_vector_set
  -> no SQL execution inside parser/UDR
```

FSPE-008 does not close server admission, engine execution, donor bridge wire protocol roles, UDR registration/signature lifecycle, cache invalidation semantics, or generated full-surface conformance. UUID, descriptor, security, transaction, policy, capability, and MGA authority remain server/engine-owned. Recovery remains MGA-based; no WAL authority is introduced.
