# FSPE-011D Validation Result

Status: complete
Search key: `FSPE_011D_VALIDATION_RESULT`

## Scope

This artifact records validation evidence for FSPE-011D persistence and restart
conformance.

Scope boundary:

- FSPE-011D proves server-admitted SBLR persistence/restart behavior for the
  current engine-owned persistence surfaces.
- FSPE-011D does not claim parser cache concurrency behavior; that is owned by
  FSPE-011E and FSPE-012A.
- FSPE-011D preserves MGA recovery authority and does not introduce WAL
  assumptions.

## Validation Commands

```bash
cmake --build build/sbsql_parser_worker_validation --target sbsql_persistence_restart_conformance -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_persistence_restart_conformance --output-on-failure
build/sbsql_parser_worker_validation/tests/sbsql_parser_worker/sbsql_persistence_restart_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all
```

## Observed Results

- `sbsql_persistence_restart_conformance`: 1/1 passed.
- Direct executable summary: `rows=2 table_uuid=019e078d-f11d-7000-8000-000000000102`.
- `sbsql_parser_worker`: 27/27 passed.
- `p0_precode_validation.py --gate all`: all gates passed.

## Closure

FSPE-011D is complete. FSPE-011E later closed the parser/session concurrency follow-up scope.
