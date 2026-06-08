# FSPE-005 Validation Result

Status: complete
Search key: `FSPE-005-STATEMENT-FAMILY-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 23:28:44 EDT
Owning slice: `FSPE-005`

## Implemented Outputs

- Added parser-owned statement registry descriptors in `project/src/parsers/sbsql_worker/statement/`.
- Materialized statement descriptors from the generated registry instead of duplicating CSV data.
- Added AST handoff metadata for statement surface ID, surface name, parser category, parser handler, binding/admission/behavior contracts, diagnostic key, cluster-profile requirement, and exact-refusal requirement.
- Expanded AST recognition beyond first-keyword vertical-slice coverage for observability, security, storage-management, COPY, LOCK, and SET ROLE / SET TRANSACTION forms.
- Added generated-style statement-family conformance probe at `project/tests/sbsql_parser_worker/generated/statement/sbsql_statement_family_conformance_probe.cpp`.
- CTest label: `sbsql_statement_family_conformance`.

## Coverage

The conformance probe validates:

- all `1,083` non-expression statement/grammar registry rows have statement descriptors;
- expected non-expression counts: `1,010` grammar-production rows and `35` canonical-surface rows;
- expected source-status counts: `1,010` `native_now` rows and `35` `cluster_private` rows;
- every descriptor preserves generated surface ID, fixed UUID, canonical name, family, source status, cluster scope, owner lane, SBLR operation family, parser handler, lowering handler, server admission key, engine rule, diagnostic key, and final acceptance rule;
- all `453` FSPE-005 active `statement parser worker` rows are `native_now`, non-cluster scoped, and have parse/AST/bind/lower/engine-rule behavior descriptors;
- the active FSPE-005 row split matches the frozen inventory: `429` grammar productions and `24` canonical surfaces;
- cluster-private statement/grammar rows require exact fail-closed/profile-gated behavior descriptors;
- representative AST bridge coverage for query, values, DML, DDL/catalog, observability, session, security, transaction, execute, call, storage-management, COPY, and LOCK forms;
- unknown WAL-style input remains parser-refused and does not receive statement descriptor authority.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_statement_family_conformance_probe -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_statement_family_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_statement_family_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `21/21` passed.

## Boundary Notes

FSPE-005 closes statement-family parser coverage only. Closure means every statement registry row has deterministic parser descriptor coverage, CST/AST construction support, surface-key candidate metadata, admission-status candidate metadata, source-span preservation, and parse-stage diagnostics or refusal metadata. FSPE-005 does not claim final binding, UUID resolution, descriptor authority, rights/security decisions, transaction/MGA effects, cluster authority, SBLR verifier admission, server admission, UDR behavior, engine execution, persistence, or full-route conformance.

AST emitted by FSPE-005 is syntax evidence. It is not catalog authority, security authority, execution authority, transaction authority, cluster authority, storage authority, or UUID authority. Names remain parser/user-layer syntax until FSPE-006 resolves them through public server/engine authority.

Any `sblr_family` recorded by FSPE-005 is a parser-side candidate or `none` for closed refusal states. Executable SBLR emission and verifier-admitted lowering are FSPE-007 scope, and engine behavior is FSPE-009 scope.

ScratchBird engine behavior must not be inferred from FSPE-005 parser acceptance. `sb_engine` does not parse SQL and executes only admitted SBLR/internal procedures. Dynamic SQL must return SBLR through the trusted parser-support path; no SQL text becomes engine execution authority.
