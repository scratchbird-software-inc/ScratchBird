# Live Fixture Gap Register

Search key: `DRIVER_LIVE_FIXTURE_GAP_REGISTER`

The offline component gates pass. These gaps remain before a full release claim:

| Gap | Status | Required closure |
| --- | --- | --- |
| Mojo toolchain | Waived on this host | Install/discover Mojo or pixi workspace and run `driver_mojo_gate` without waiver. |
| JDBC live endpoint classes | Filtered from offline gate | Start a CTest-owned `sb_server` fixture and run `JDBC203PoolingAndRecoveryContractTest`, `SBIntegrationTest`, `SBJdbcClosureParityTest`, and `SBNativeSQLParityTest` against it. |
| .NET live integration collection | Filtered from offline gate | Start a CTest-owned `sb_server` fixture and run `IntegrationTests`, `JDBC203PoolingAndRecoveryContractTests`, and enabled soak/fault classes against it. |
| Common conformance | Policy-ready only | Execute shared conformance contracts against every driver with live fixture credentials. |
| Packaging install smoke | Policy-ready only | Build distributable artifacts and install/import them from build-scoped installation roots. |
| Execution_Plan 10 benchmark execution | Path-ready only | Run the benchmark matrix using current repo driver/tool outputs and emit JSON under `build/benchmarks`. |
