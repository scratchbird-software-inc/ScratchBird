# Fault Injection Tests

Fault-injection test source belongs here and must not be compiled into
production behavior except through guarded diagnostic builds.

Only tests that inject a runtime fault may carry the `fault_injection` CTest
label. `public_crash_fault_source_contract_matrix.py` is a source-contract
coverage inventory and deliberately carries only `source_contract`,
`coverage_inventory`, and `evidence_gate` taxonomy labels.

## Whole-store recovery through the real DML route

`sbsql_whole_store_recovery_full_route_gate.py` proves recovery through the
same public route used by a client:

`SBsql -> listener -> SBSql parser -> SBLR -> server DML -> canonical store`

It creates an indexed database fixture, performs transaction-controlled DML,
kills the server process at each registered storage boundary, restarts the
ordinary stack, and verifies scans, point reads, uniqueness, row/index
agreement, and transaction classification through `SBsql`.  It does not use a
physical-store test API to verify recovery.

Configure, build, and run the gate with:

```sh
cmake -S project -B build/whole-store-recovery \
  -DSB_BUILD_SBSQL_PARSER_WORKER=ON \
  -DSB_BUILD_SBSQL_PARSER_WORKER_TESTS=ON \
  -DSB_BUILD_SBU_SBSQL_PARSER_SUPPORT=ON \
  -DSB_BUILD_SERVER=ON \
  -DSB_BUILD_SB_LISTENER=ON \
  -DSB_ENABLE_TEST_CRASH_INJECTION=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/whole-store-recovery --target \
  sb_server sb_listener sbp_sbsql sb_isql sbsql_example_database_seed
ctest --test-dir build/whole-store-recovery --output-on-failure \
  -R '^sbsql_whole_store_recovery_full_route_gate$'
```

The build option is off by default.  A crash-enabled binary still ignores the
fault points unless the harness supplies all four test-only environment
values: `SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_ARM=issue6-real-dml-route`, a
registered `SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_POINT`, an explicit
`SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_MARKER` path, and the exact
`SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_TRIGGER_VALUE` carried by the intended DML
row.  Observing that value arms only its engine transaction, preventing startup
or background work from consuming the fault.  Commit-publication points are
additionally restricted to that transaction's engine commit scope.

The registered matrix covers allocation, partial page write, page sync,
directory mutation, index write, index sync, catalog/trigger effect, mutation
manifest publication, transaction-inventory publication, final sync, and
recovery cleanup.  Pre-publication cases must recover the attempted transaction
as rolled back; transaction-inventory publication and final sync must recover
it as committed.  The cleanup case kills recovery itself once, then proves a
second restart is idempotent.

The test retains its complete work directory on failure and prints its path.
On success it writes the last work path to
`build/whole-store-recovery/sbsql_whole_store_recovery_full_route/latest_work_path.txt`.
Each case contains server, listener, parser, and `SBsql` logs plus the crash
marker and the aggregate recovery matrix, so a developer can inspect every
boundary instead of only the first failure.
