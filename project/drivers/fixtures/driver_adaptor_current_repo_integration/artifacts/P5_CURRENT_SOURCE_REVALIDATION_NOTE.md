# P5 Current-Source Revalidation Note

Date: 2026-05-20

This note supersedes the older historical validation note for the Phase 5 live
fixture rows. The current build was reconfigured with:

```bash
cmake -S project -B build -DSB_BUILD_DRIVER_GATES=ON
```

## CTest Evidence

- `ctest --test-dir build -R "driver_server_fixture_gate|driver_common_conformance_gate|driver_packaging_install_gate|driver_benchmark_readiness_gate|driver_execution_plan10_runner_gate|driver_static_hygiene_gate|drivers_final_zero_drift_audit|drivers_all|tool_cli_gate|driver_server_release_declaration_gate|driver_row_status_manifest_gate|adapter_row_status_manifest_gate|tool_row_status_manifest_gate|driver_status_packaging_evidence_gate|driver_server_verification_packets_gate|driver_full_route_benchmark_evidence_gate|driver_performance_budget_threshold_gate|driver_documentation_sample_app_evidence_gate|driver_donor_compatibility_route_evidence_gate" --output-on-failure`
  passed `19/19`.
- `ctest --test-dir build -L driver_tool_adaptor_gate --output-on-failure`
  passed `35/35`, `0` failed, `0` skipped.
- `ctest --test-dir build -R "database_lifecycle_wire_driver_p5_conformance|database_lifecycle_server_daemon_conformance|database_lifecycle_ipc_conformance|database_lifecycle_admin_cli_conformance|database_lifecycle_admin_cli_static|database_lifecycle_packaging_service_static|database_lifecycle_packaging_service_conformance|database_lifecycle_listener_conformance|sb_listener_sbp_sbsql_server_engine_execution_smoke|sb_listener_sbp_sbsql_handoff_smoke|sb_server_sbsql_admission_conformance|sb_server_cursor_protocol_conformance" --output-on-failure`
  passed `12/12`.

## P5 Rows Closed

- `DCT-060`: closed by `driver_server_fixture_gate` plus live
  listener/server/IPC lifecycle and SBsql route CTests.
- `DCT-061`: closed by `driver_common_conformance_gate` and the full
  `driver_tool_adaptor_gate` label run.
- `DCT-062`: closed by all `adaptor_*_gate` CTests.
- `DCT-063`: closed by `tool_cli_gate` and admin CLI lifecycle CTests.
- `DCT-064`: closed by benchmark policy, Execution_Plan 10 path, full-route
  benchmark evidence, and performance-budget gates.
- `DCT-065`: closed by packaging policy/evidence CTests and packaging-service
  lifecycle CTests.

The current host passed `driver_mojo_gate`; no deterministic toolchain waiver
was consumed in this revalidation run.
