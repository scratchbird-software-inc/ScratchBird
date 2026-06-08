# Benchmark Baseline Report

Status: complete
Search key: `FSPE-BENCHMARK-BASELINE-REPORT`
Owning slice: `FSPE-014F`
Date: 2026-05-08

## Purpose

Record non-tuning baseline measurements for the first complete SBSQL parser/UDR/server/engine route. These numbers are regression anchors, not performance claims.

This report does not claim tuned performance, external benchmark results, or release throughput. It records the current validation build cost so future parser/UDR/server/engine changes can detect catastrophic regressions with comparable commands.

## Machine And Build Profile

| Field | Value |
| --- | --- |
| Host | `daltonkeellaptop` |
| Kernel | `Linux 6.17.0-23-generic #23~24.04.1-Ubuntu SMP PREEMPT_DYNAMIC Tue Apr 14 16:11:48 UTC 2 x86_64` |
| CPU slots visible to build/test | `14` |
| Memory at measurement | `30Gi total`, `22Gi available`, `33Gi swap`, `0B swap used` |
| Repo build id | `84136ff1` |
| Build tree | `build/sbsql_parser_worker_validation` |
| Build type | `Release` |
| C compiler | `/usr/bin/cc` |
| C++ compiler | `/usr/bin/c++` |
| Debug logs | `SCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF` |
| Hot-path trace | `SCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF` |
| Exec-profile trace | `SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF` |
| Prepared trace | `SCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF` |

## Fixture Labels And Route Anchors

| Baseline area | Fixture id, binary, or label anchor | Current measurement |
| --- | --- | --- |
| Parser startup and registry load | `sbp_sbsql_registry_generation_probe`; label `registry_linter_tests`; generated surface fixtures with `SBSQL-SURFACE-*` ids | `0.05 sec` CTest time in the focused baseline run. |
| Lex/parse | `sbp_sbsql_lexer_conformance_probe`; label `sbsql_lexer_conformance` | `0.02 sec` CTest time in the focused baseline run. |
| CST/AST source artifacts | `sbp_sbsql_cst_ast_conformance_probe`; label `sbsql_cst_ast_conformance` | `0.00 sec` CTest rounded time in the focused baseline run. |
| Bind/name resolution | `sbp_sbsql_binder_authority_conformance_probe`; label `sbsql_binder_authority_conformance` | `0.00 sec` CTest rounded time in the focused baseline run. |
| SBLR lowering and verifier admission | `sbp_sbsql_sblr_lowering_verifier_conformance_probe`; label `sbsql_sblr_lowering_verifier_conformance` | `0.00 sec` CTest rounded time in the focused baseline run. |
| UDR conversion | `sbu_sbsql_parser_support_probe`; label `sbu_sbsql_parser_support_conformance` | `0.01 sec` CTest time in the focused baseline run. |
| Server admission | `sb_server_sbsql_admission_conformance`; label `sb_server_sbsql_admission_conformance` | Included with full-route smoke in CTest label aggregation: `0.28 sec` in focused run and `0.26 sec` in full-label run for two tests. |
| Engine execution and full route | `sb_listener_sbp_sbsql_server_engine_execution_smoke`; labels `sbsql_full_route_streaming_conformance`, `sbsql_canary_vertical_slice_gate`, and `sb_server_sbsql_admission_conformance` | `0.28 sec` focused run; `0.25 sec` clean full-label run. |
| Streaming and cursor protocol | `sb_streaming_result_protocol_conformance`, plus FSPE-010B streaming labels | `0.00 sec` rounded direct streaming gate time; chunked/full-route label aggregation `0.35 sec` in full-label run. |
| Full parser-worker closure suite | CTest label `sbsql_parser_worker`; 39 tests | `1.97 sec` real time, `elapsed=1.97`, `maxrss_kb=49344`, passed `39/39`. |

## Command Lines

Focused parser/UDR/server/full-route timing:

```bash
/usr/bin/time -f 'elapsed=%e maxrss_kb=%M' ctest --test-dir build/sbsql_parser_worker_validation -R 'sbp_sbsql_registry_generation_probe|sbp_sbsql_lexer_conformance_probe|sbp_sbsql_cst_ast_conformance_probe|sbp_sbsql_binder_authority_conformance_probe|sbp_sbsql_sblr_lowering_verifier_conformance_probe|sbu_sbsql_parser_support_probe|sb_server_sbsql_admission_conformance|sb_listener_sbp_sbsql_server_engine_execution_smoke|sb_streaming_result_protocol_conformance' --output-on-failure
```

Full parser-worker regression timing:

```bash
/usr/bin/time -f 'elapsed=%e maxrss_kb=%M' ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
```

## Retained Raw Summaries

Focused baseline raw summary:

```text
100% tests passed, 0 tests failed out of 9
Total Test time (real) =   0.37 sec
elapsed=0.37 maxrss_kb=26020
registry_linter_tests = 0.05 sec*proc
sbsql_lexer_conformance = 0.02 sec*proc
sbsql_cst_ast_conformance = 0.00 sec*proc
sbsql_binder_authority_conformance = 0.00 sec*proc
sbsql_sblr_lowering_verifier_conformance = 0.00 sec*proc
sbu_sbsql_parser_support_conformance = 0.01 sec*proc
sb_server_sbsql_admission_conformance = 0.28 sec*proc
sbsql_full_route_streaming_conformance = 0.28 sec*proc
sb_streaming_result_protocol_conformance = 0.00 sec*proc
```

Full parser-worker raw summary:

```text
100% tests passed, 0 tests failed out of 39
Total Test time (real) =   1.97 sec
elapsed=1.97 maxrss_kb=49344
sbsql_parser_worker = 1.96 sec*proc
sbsql_generated_full_surface_conformance = 0.11 sec*proc
sbsql_deterministic_no_network_gate = 0.74 sec*proc
sb_server_sbsql_admission_conformance = 0.26 sec*proc
sbsql_full_route_streaming_conformance = 0.25 sec*proc
sbps_chunked_payload_conformance = 0.35 sec*proc
sbsql_persistence_restart_conformance = 0.10 sec*proc
sbsql_concurrent_session_transaction_conformance = 0.07 sec*proc
sbsql_upgrade_migration_compatibility_gate = 0.03 sec*proc
```

Supplemental direct-probe observations from the benchmark worker:

```text
sbp_sbsql --help: exit 0, maxrss_kb=7836
lexer probe: passed, wall=0:00.02, maxrss_kb=14180
CST/AST probe: passed, wall=0:00.00, maxrss_kb=6696
binder probe: exit 0, wall=0:00.00, maxrss_kb=6708
lowering/verifier probe: exit 0, wall=0:00.00, maxrss_kb=6732
UDR probe: exit 0, wall=0:00.00, maxrss_kb=7216
server admission: passed, wall=0:00.00, maxrss_kb=4988
streaming protocol: passed, wall=0:00.00, maxrss_kb=4284
full-route smoke direct binary: passed, wall=0:00.26, maxrss_kb=18772
differential replay harness: passed, fixtures=2617 routes=8 payloads=2617 unexpected_failures=0, wall=0:00.13, maxrss_kb=48860
```

## Baseline Interpretation

The rounded `0.00 sec` fixture entries are CTest-resolution timing anchors, not proof of zero cost. Future measurements should compare the same labels, build profile, and instrumentation state before treating deltas as parser, server, or engine regressions.

The benchmark-clean profile has ScratchBird tracing compiled off. If a future diagnostic run enables hot-path, exec-profile, prepared, or debug logging instrumentation, its timings must not be compared directly to this baseline.

## Closure Rule

Command lines, fixture ids and labels, machine/profile metadata, build id, non-tuning scope, and retained raw summaries are recorded. FSPE-014F is complete when `sbsql_benchmark_baseline_gate` passes and the tracker opens final FSPE-014 audit.
