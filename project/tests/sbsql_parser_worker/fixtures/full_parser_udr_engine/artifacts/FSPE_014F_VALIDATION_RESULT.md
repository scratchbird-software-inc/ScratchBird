# FSPE-014F Validation Result

Status: passed
Date: 2026-05-08
Owning slice: `FSPE-014F`
Search key: `FSPE-014F-VALIDATION-RESULT`

## Scope

FSPE-014F records non-tuning timing and memory baselines for parser registry startup, lex/parse, CST/AST, binding, SBLR lowering, UDR conversion, server admission, streaming, full-route smoke, and the complete parser-worker CTest label.

## Commands

| Command | Result |
| --- | --- |
| `/usr/bin/time -f 'elapsed=%e maxrss_kb=%M' ctest --test-dir build/sbsql_parser_worker_validation -R 'sbp_sbsql_registry_generation_probe|sbp_sbsql_lexer_conformance_probe|sbp_sbsql_cst_ast_conformance_probe|sbp_sbsql_binder_authority_conformance_probe|sbp_sbsql_sblr_lowering_verifier_conformance_probe|sbu_sbsql_parser_support_probe|sb_server_sbsql_admission_conformance|sb_listener_sbp_sbsql_server_engine_execution_smoke|sb_streaming_result_protocol_conformance' --output-on-failure` | passed 9/9; `elapsed=0.37`; `maxrss_kb=26020` |
| `/usr/bin/time -f 'elapsed=%e maxrss_kb=%M' ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | passed 39/39; `elapsed=1.97`; `maxrss_kb=49344` |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/benchmark_baseline_gate.py --repo-root .` | passed |

## Evidence

- `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/BENCHMARK_BASELINE_REPORT.md`
- `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/benchmark_baseline_gate.py`

## Result

FSPE-014F is complete. Final FSPE-014 audit can open as the next serialized slice.
