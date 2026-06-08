# FSPE-004 Validation Result

Status: complete
Search key: `FSPE-004-EXPRESSION-BUILTIN-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 23:17:12 EDT
Owning slice: `FSPE-004`

## Implemented Outputs

- Added parser-owned expression runtime descriptors in `project/src/parsers/sbsql_worker/expression/`.
- Materialized descriptors from the generated SBSQL registry instead of duplicating CSV data.
- Added parser, binder, lowering, engine-rule, diagnostic, behavior, arity, expression-class, precedence, associativity, and exact-refusal metadata for every `expression_runtime` row.
- Added generated-style expression/builtin conformance probe at `project/tests/sbsql_parser_worker/generated/expression/sbsql_expression_builtin_conformance_probe.cpp`.
- CTest label: `sbsql_expression_builtin_conformance`.

## Coverage

The conformance probe validates:

- all `1,534` `expression_runtime` registry rows have descriptors;
- expected row counts: `1,515` functions, `18` operators/special variable forms, and `1` variable marker;
- expected source-status counts: `663` `native_now` rows and `911` `native_future` rows;
- every descriptor preserves generated surface ID, fixed UUID, canonical name, source status, cluster scope, parser handler, lowering handler, engine rule, diagnostic key, and final acceptance rule;
- every row has non-empty binder, behavior, expression-class, and arity descriptors;
- `native_future` rows require exact refusal unless promoted by a later behavior slice;
- cluster-private rows use fail-closed/profile-gated behavior descriptors even when also marked `native_future`;
- representative lookup and lexer bridge coverage for `@`, `@@ROWCOUNT`, `Accept`, `COALESCE(a,b,...)`, and `f(name=>value)`.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_expression_builtin_conformance_probe -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_expression_builtin_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_expression_builtin_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `20/20` passed.

## Boundary Notes

`FSPE-004` closes the parser-owned expression/builtin catalog and exact behavior/refusal descriptor gate for the full expression registry. Full statement grammar use of these descriptors remains owned by `FSPE-005`, binder authority and descriptor attachment remain owned by `FSPE-006`, verifier-admitted SBLR lowering remains owned by `FSPE-007`, and concrete engine builtin execution remains owned by `FSPE-009`.
