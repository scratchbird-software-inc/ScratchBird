# FSPE-010 Validation Result

Status: passed
Search key: `FSPE-010-SERVER-SBSQL-ADMISSION-VALIDATION`

## Implemented Scope

- Added server-owned SBLR admission validation before prepare/execute dispatch.
- Covered all 15 current `SBSQL_TO_SBLR_OPERATION_MATRIX.csv` operation-family values.
- Preserved non-cluster fail-closed behavior for `sblr.cluster.private_operation.v3`.
- Rejected raw SQL/source-text-marked payloads before engine dispatch.
- Routed binary canonical SBLR operation envelopes through the public engine ABI and propagated rejection instead of ignoring engine status.
- Materialized `sb_server_sbsql_admission_conformance` as a runnable CTest label.

## Validation Commands

```bash
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sb_server_sbsql_admission_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_sbsql_admission_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

## Results

- `sb_server_sbsql_admission_conformance`: passed `2/2`.
- Focused validation shard: passed `25/25`.

## Boundary Evidence

- Server validation is in `project/src/server/sblr_admission.cpp` under search key `SB_SERVER_SBLR_ADMISSION_VALIDATOR`.
- Server dispatch uses public engine ABI only; it does not include engine internal API or private SBLR dispatch headers.
- `sb_engine_public_source_boundary_fixture` passed in the focused shard.
