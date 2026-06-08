# Baseline Test Result

Status: complete
Search key: `FSPE-BASELINE-TEST-RESULT`
Generated: 2026-05-07 20:22:35 EDT

## Result Summary

| Scope | Tests | Passed | Failed | Result | Log |
| --- | ---: | ---: | ---: | --- | --- |
| `build/sbsql_parser_worker_validation` CTest | 15 | 15 | 0 | passed | `artifacts/baseline/ctest.log` |

## Tests Listed

- `sbp_sbsql_pipeline_probe`
- `sbp_sbsql_ipc_schema_probe`
- `sbp_sbsql_no_spin_gate`
- `sbp_probe_product_exists`
- `sb_listener_sbp_sbsql_server_engine_execution_smoke`
- `sbu_sbsql_parser_support_probe`
- `sb_engine_public_abi_c_fixture`
- `sb_engine_public_abi_cpp_fixture`
- `sb_engine_public_sblr_admission_fixture`
- `sb_engine_public_diagnostic_shape_fixture`
- `sb_engine_public_metrics_thread_fixture`
- `sb_engine_public_source_boundary_fixture`
- `sb_engine_public_wire_stability_fixture`
- `sb_engine_public_documentation_example_fixture`
- `sb_engine_public_abi_symbol_gate`

## Focused Coverage Represented

- Parser worker pipeline and IPC schema probes.
- Parser no-spin gate and product existence probe.
- Listener -> parser -> server -> engine smoke route.
- Trusted parser-support UDR probe.
- Engine public ABI, SBLR admission, diagnostic shape, metrics-thread, source-boundary, wire-stability, documentation-example, and symbol gates.

## Known Failures

No focused baseline test failures were observed. This does not imply full SBSQL closure readiness; it only establishes the pre-implementation state for currently configured focused tests.

## Logs

- `artifacts/baseline/configure.log`
- `artifacts/baseline/build.log`
- `artifacts/baseline/build-timed.log`
- `artifacts/baseline/ctest-list.log`
- `artifacts/baseline/ctest.log`
- `artifacts/baseline/ctest-timed.log`
- `artifacts/baseline/target-help.log`
- `artifacts/baseline/df.log`
- `artifacts/baseline/build-dir-size.log`
- `artifacts/baseline/git-status-short.log`
