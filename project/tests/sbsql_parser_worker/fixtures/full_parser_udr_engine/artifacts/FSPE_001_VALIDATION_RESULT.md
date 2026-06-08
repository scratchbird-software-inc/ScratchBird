# FSPE-001 Validation Result

Status: complete
Search key: `FSPE-001-REGISTRY-GENERATION-VALIDATION`
Generated: 2026-05-07 20:50:56 EDT
Owning slice: `FSPE-001`

## Implemented Outputs

- `project/tools/sb_parser_gen/generate_sbsql_registry.py`
- `project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.hpp`
- `project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp`
- `project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest`
- `project/tests/sbsql_parser_worker/sbsql_registry_generation_probe.cpp`
- CTest label: `registry_linter_tests`

## Coverage

The generated registry contains all `2617` rows from `SURFACE_IMPLEMENTATION_BACKLOG.csv`.

| Class | Count |
| --- | ---: |
| `native_now` | 2580 |
| `cluster_private` status | 37 |
| `cluster_private` scope | 57 |

Every generated row has parser, UDR, SBLR lowering, server admission, engine rule/refusal, diagnostic, semantic-oracle, batch, CTest-label, fixture, and final-acceptance assignments. The registry linter also compares generated surface IDs against `SURFACE_IMPLEMENTATION_BACKLOG.csv`, `BATCH_ROW_MEMBERSHIP.csv`, and `SEMANTIC_ORACLE_AUTHORITY_MAP.csv`.

## Determinism Check

Running the generator twice produced stable generated file hashes:

| File | SHA-256 |
| --- | --- |
| `sbsql_generated_registry.hpp` | `a7935224e25dbe67be0a2237f90d4bee55ee57f9a9189887c5f3b9603785310f` |
| `sbsql_generated_registry.cpp` | `229bccc442424debeef93f519b69dcd6fde60c25969cd0241e159aba46fdafad` |
| `sbsql_generated_registry.manifest` | `cfeca5380ad4cbdfa513b4aed05ed2e2e336009645d49c67cadd7f86f88dce2c` |

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation -DSB_BUILD_SBSQL_PARSER_WORKER=ON -DSB_BUILD_SBSQL_PARSER_WORKER_TESTS=ON -DSB_BUILD_SBU_SBSQL_PARSER_SUPPORT=ON
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_registry_generation_probe
ctest --test-dir build/sbsql_parser_worker_validation -L registry_linter_tests --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `registry_linter_tests`: `1/1` passed.
- Focused SBSQL parser-worker validation shard after canary test addition: `17/17` passed.

## Boundary Notes

`FSPE-001` generates implementation assignment and coverage metadata only. It does not claim full parser, UDR, server, or engine behavior completion for the 2617 surfaces. `FSPE-001A` has since closed; later behavior remains owned by downstream slices.
