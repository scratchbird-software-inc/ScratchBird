# DML optimization equivalence tests

These tests compare actual engine execution from identical, closed database
baselines. They check expected results as well as agreement between routes.
They are correctness tests, not performance benchmarks or a release-wide
qualification suite.

## Build and run

From the repository root:

```sh
cmake -S project -B build/dml-equivalence -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSB_BUILD_TESTS=ON \
  -DSB_BUILD_DATABASE_LIFECYCLE_TESTS=ON \
  -DSB_ENABLE_TEST_DML_ROUTE_SELECTION=ON \
  -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF \
  -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF \
  -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF \
  -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF
cmake --build build/dml-equivalence --target \
  sb_server sb_listener sbp_sbsql sb_isql sbsql_example_database_seed \
  dml_inventory_snapshot_probe dml_optimization_history_probe \
  dml_test_insert_route_enabled dml_test_insert_route_disabled \
  dml_unique_key_equivalence_unit transactional_index_lifecycle_matrix_gate \
  ipar_unique_index_probe_gate cdp_native_bulk_ingest_api_gate \
  database_lifecycle_constraint_dml_enforcement_conformance -- -k 0 -j 4
ctest --test-dir build/dml-equivalence --output-on-failure \
  -L dml_optimization_equivalence
ctest --test-dir build/dml-equivalence --output-on-failure \
  -R '^(transactional_index_lifecycle_matrix_gate|ipar_unique_index_probe_gate|database_lifecycle_constraint_dml_enforcement_conformance|cdp_native_bulk_ingest_api_gate)$'
```

Retain complete build/test logs. Do not stop the selected test set at its
first failure. The process-route tests run serially and may take several
minutes each. Do not rebuild their executables while a run is in progress.

The option defaults OFF and requires `SB_BUILD_TESTS`. Production-disabled
builds ignore both selection variables and compile out evidence-file writes.
Controls are private test interventions, not public ABI, wire-protocol fields,
server configuration, or alternative transaction policies. Selection is
immutable after first use; start a fresh engine process for every profile.

## Reference routes and interventions

`SCRATCHBIRD_TEST_INSERT_ROUTE=optimized|staged` selects ordinary INSERT's
direct or staged executor, including the no-match post-conflict shortcut.
The staged route alone does not disable every optimization.

`SCRATCHBIRD_TEST_DML_OPTIMIZATION` selects these independent interventions:

| Profile | Intervention / fallback |
| --- | --- |
| `normal` | Existing optimized behavior; repeated work exercises warm caches. |
| `cold` | Evict the append index cache before admission to its reuse path. |
| `uncached` | Bypass append index/context cache reads and publication; reload the canonical table-scoped index view. |
| `evicted` | Lose the append cache between lookup and uniqueness proof; reload under the same MGA context. |
| `publication_evicted` | Lose the append cache just before post-append cache publication; a delta must not certify a complete cache. |
| `relation_rows` | Use canonical table-scoped rows instead of the index-only loader; do not use append caches. |
| `publish_cache` | Publish the cache even for the native single-window bulk route that normally skips it. |
| `scan_scalar` | Stage ordinary INSERT; scan visible rows for persisted uniqueness; bypass decoded-row cache reads/writes and hot-point lookup; use UPDATE's existing table scan and scalar row/index publication. Materialized native bulk uses the existing staged INSERT fallback. |

Canonical DELETE already uses its bounded MGA candidate stream. It does not
gain a different transaction implementation in the reference profile.
`relation_rows` is a **table-scoped canonical MGA read**, not a full-database
compatibility-state reconstruction.

The scalar reference is bounded to these audited normal-DML/materialized-row
optimizations. It does not disable every storage micro-optimization. Native
packet-only ingestion, strict bulk loading, and specialized engine-owned
publication callbacks retain their admission/publication contracts; the
materialized-row fallback must not be used to bypass those contracts.

Unknown selection values refuse before INSERT/direct-append mutation in a
test-enabled build. Successful branch records are coverage evidence only.
Native durable transaction inventory, not trace output, supplies finality.

## What the gates prove

- `sbsql_insert_route_equivalence_gate`: deterministic seeded INSERT and mixed
  INSERT/UPDATE/DELETE histories over the client/listener/parser/server route;
  exact results, diagnostics and affected counts; savepoints, rollback, key
  reuse, overflow values, native mutation finality and two server restarts.
- `sbsql_insert_cache_equivalence_gate`: the cache and scoped-loader profiles
  with actual branch coverage, including eviction before proof/publication.
- `sbsql_scan_scalar_equivalence_gate`: optimized/staged/scalar execution,
  mixed mutation histories and the independently computed row model.
- `dml_snapshot_equivalence_gate`: positive unmatched/matched conflict
  INSERT, conflict UPDATE, savepoint rewind and an overlapping old snapshot.
- `dml_bulk_equivalence_gate`: materialized bulk success, duplicate-batch
  atomicity, key reuse, rewind and snapshot overlap.
- `dml_random_equivalence_901_gate` and `_902_gate`: seeded bulk/INSERT/conflict
  histories across all nine profiles, three transaction epochs, whole rollback,
  savepoint rewind, counts/diagnostics, row and index checks after each action,
  overlapping snapshots and two fresh-process native recovery/opens.
- `dml_crash_before_commit_equivalence_gate` and
  `dml_crash_after_commit_equivalence_gate`: kill the engine process after a
  successful actual INSERT, before or after commit. Native open/recovery must
  classify unfinished work as rolled back and retain committed work. These
  two cuts are not a claim of exhaustive internal page-write crash coverage.
- `transactional_index_lifecycle_matrix_gate`: the separate lifecycle
  regression for every admitted B-tree and non-B-tree profile. It is not a
  claim that every public SELECT uses a physical index scan.

Engine probes check actual canonical, row-rechecked index memberships and provider lookups
against independently expected rows, including absent/historical keys. Rows
and indexes are observed again in fresh processes. Correspondence is explicit
for mutation transactions; unrelated asynchronous agent transaction ordinals
are not compared as if they represented identical user work. Raw inventory is
retained.

An unchanged indexed key may retain its earlier index locator after a
payload-only UPDATE. The probe checks that locator against the current
MGA-visible row; it does not require unnecessary index-version rewrites.
Atomic native-bulk failures report no per-row accepted/inserted/rejected
outcome. The input batch size is not a reject-and-continue result.

The public SQL `ON CONFLICT` transport is currently unadmitted. Its tests
require `SBLR.OPERATION.NONCANONICAL`, no mutation by the refused statement,
and preservation of surrounding work. Those cases are reported separately
and are **not positive conflict execution proof**. Positive post-conflict
optimization equivalence is tested through the real engine API. Adding the
public transport requires its own canonical binding/admission work.

## Replay, reduction and artifacts

Each run prints its short temporary artifact root. The CMake work directory
contains `artifact_path.txt`. Logs, SQL, seeds, expected observations, native
inventories, branch traces and summaries are retained. Passing database
companions and owned filespace growth segments are removed automatically.
Failures retain their databases for diagnosis. Stop their processes before
removing fixtures; never delete another run's database or shared build tree.

The public-route runner accepts repeated `--seed`, `--case` and
`--optimization-profile` arguments. For example, use `--case mixed_model
--seed 901 --optimization-profile scan_scalar`. Supply the same six executable
arguments and `--work-dir` shown in `ctest -N -V` for that gate.

Replay a saved `history.json` or `reduced_history.json` with
`--replay-history /absolute/path/to/history.json`, without `--seed` or `--case`.
On a semantic mixed-history failure, the runner attempts bounded reduction
against the real engine and preserves candidates and `reduction.json`.
`--reduction-attempts N` controls the budget; zero disables reduction. It
preserves transaction epochs and the failure class, recomputes the oracle,
and does not claim a globally minimal result. Startup, timeout and missing
branch-evidence failures are not reduced as semantic counterexamples.

The engine-API runner accepts `--case random --seed N`; `replay.json` and
`history.stdout` retain the seed and exact action sequence. Its fixed integer
PRNG mapping is portable. Engine-API histories are seed-replayable; automatic
delta reduction currently applies to public-route mixed histories only.
