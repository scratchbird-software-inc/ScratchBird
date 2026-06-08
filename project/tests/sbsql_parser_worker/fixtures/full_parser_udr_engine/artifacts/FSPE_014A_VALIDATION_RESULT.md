# FSPE-014A Validation Result

Status: passed
Date: 2026-05-08
Owning slice: `FSPE-014A`
Search key: `FSPE-014A-VALIDATION-RESULT`

## Scope

FSPE-014A materialized the source-size and maintainability gate for parser, UDR, server, wire IPC, and engine implementation sources. The gate enforces a 128 KiB cap for non-exception handwritten files, a 3 MiB cap for generated files, and a recorded 2.5 MiB deterministic seed-data exception for the canonical function seed table.

## Commands

| Command | Result |
| --- | --- |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/source_size_maintainability_gate.py --repo-root .` | passed |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/spec_synchronization_audit.py --repo-root .` | passed |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | passed |

## Evidence

- `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/SOURCE_SIZE_MAINTAINABILITY_REPORT.md`
- `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/source_size_maintainability_gate.py`

## Result

FSPE-014A is complete. FSPE-014B can open as the next serialized slice.
