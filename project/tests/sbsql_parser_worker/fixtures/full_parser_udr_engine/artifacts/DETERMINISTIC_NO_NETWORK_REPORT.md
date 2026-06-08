# Deterministic No-Network Report

Status: complete
Search key: `FSPE-DETERMINISTIC_NO_NETWORK_REPORT`
Owning slice: `FSPE-012C`

## Summary

FSPE-012C materialized `sbsql_deterministic_no_network_gate` as a reproducibility and offline-input CTest gate.

The gate verifies:

- `project/tools/sb_parser_gen/generate_sbsql_registry.py` runs twice into build-temp output directories.
- The two generated registry runs are byte-identical for `sbsql_generated_registry.hpp`, `sbsql_generated_registry.cpp`, and `sbsql_generated_registry.manifest`.
- The generated registry output matches the tracked registry files under `project/src/parsers/sbsql_worker/registry/generated/`.
- Tracked generated parser-worker artifacts match `DETERMINISTIC_ARTIFACT_MANIFEST.csv` by path, SHA-256, size, category, and source-input class.
- Generator and generated-test inputs are scanned for explicit external network/dependency fetch tokens.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_deterministic_no_network_gate` |
| Repro gate | `project/tests/sbsql_parser_worker/generated/repro/sbsql_deterministic_no_network_gate.py` |
| Manifest | `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` |
| Generated artifact count | 34 |
| Unexpected failures | 0 |

## Boundary

CTest does not disable host networking at the kernel level. This gate proves the checked-in generator and generated-test inputs are repo-local, deterministic, and free of explicit network/dependency-fetch calls. CI can add an external network-deny sandbox around the same label without changing the gate contract.

## Result

Generated registry files and generated parser-worker fixtures, including the FSPE-014G exhaustive E2E regression gate, are reproducible from repo-local inputs without internet-dependent generation steps.
