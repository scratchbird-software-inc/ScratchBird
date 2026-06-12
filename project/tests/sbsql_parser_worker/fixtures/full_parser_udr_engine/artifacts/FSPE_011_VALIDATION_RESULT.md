# FSPE-011 Validation Result

Status: complete
Search key: `FSPE_011_VALIDATION_RESULT`

## Scope Validated

This artifact records closure evidence for the FSPE-011 generated full-surface
coverage gate.

Scope boundary:

- FSPE-011 validates mechanical coverage across the canonical surface, operation,
  engine-gap, reference-alias, message-vector, batch, and oracle matrices.
- FSPE-011 does not claim durable per-row semantic fixture execution for every
  parser, CST, AST, BoundAST, SBLR, UDR, server, and engine route.
- Durable fixture execution remains owned by FSPE-011A through FSPE-011F.

Validated coverage:

- `sbsql_generated_full_surface_conformance` is a runnable CTest gate.
- The generated registry contains 2,617 stable surface rows and matches the generated count constant.
- Canonical surface, surface-status, SBLR operation, surface backlog, batch membership, and semantic-oracle rows match the generated registry by stable surface ID.
- Engine gap coverage reconciles 932 canonical/backlog rows and validates non-cluster versus cluster-private closure status.
- Reference alias coverage reconciles 312 canonical/backlog rows and validates exact mapped-or-refused behavior metadata.
- Message-vector fixture coverage reconciles 41 concrete diagnostic rows across parser, UDR, server, engine, listener, manager, and agent origins.
- Registry batching reconciles 77 batch rows against observed generated surface membership.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_generated_full_surface_conformance_probe -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_generated_full_surface_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
build/sbsql_parser_worker_validation/tests/sbsql_parser_worker/sbp_sbsql_generated_full_surface_conformance_probe project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts public_input_snapshot
```

Observed results:

- `sbsql_generated_full_surface_conformance`: 1/1 passed.
- `sbsql_parser_worker`: 23/23 passed with the generated full-surface conformance probe included.
- Direct probe summary: `surfaces=2617 operation_rows=2617 operation_families=19 engine_gaps=932 reference_aliases=312 message_vectors=41 batches=77`.

## Remaining FSPE-011 Closure Gaps

None. `FSPE-011A` is the next ready execution_plan slice.
