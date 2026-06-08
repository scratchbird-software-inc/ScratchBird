# FSPE-011C Validation Result

Status: complete
Search key: `FSPE_011C_VALIDATION_RESULT`

## Scope

This artifact records validation evidence for FSPE-011C semantic oracle and
expected-result authority.

Scope boundary:

- FSPE-011C validates expected-result provenance and authority for every
  generated surface fixture.
- FSPE-011C does not execute every fixture.
- Differential replay remains owned by FSPE-011F.

## Validation Commands

```bash
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_semantic_oracle_authority_gate -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_semantic_oracle_authority_gate --output-on-failure
build/sbsql_parser_worker_validation/tests/sbsql_parser_worker/sbp_sbsql_semantic_oracle_authority_gate project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts public_input_snapshot .
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all
```

## Observed Results

- CMake configure and `sbp_sbsql_semantic_oracle_authority_gate` build completed.
- `sbsql_semantic_oracle_authority_gate`: 1/1 passed.
- Direct gate summary: `oracles=2617 canonical_spec=1988 promotion_or_refusal=573 cluster_profile=56`.
- `sbsql_parser_worker`: 26/26 passed.
- `p0_precode_validation.py --gate all`: all gates passed.

## Closure

FSPE-011C is complete. The dedicated semantic-oracle authority gate proves every
generated surface fixture has independent expected-result provenance and
cross-checks fixture IDs, source search keys, authority source paths, oracle
types, closure status, batch membership, surface backlog rows, registry rows,
and SBLR operation matrix rows.
