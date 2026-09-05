# Public CI signal topology

The Linux, Windows, and macOS public CI workflows publish independent signals
for repository policy, compilation, tests, and packaging. A failure in the
`static-policy` job cannot prevent the independent `build` job from running.
The final `public-release-*` job uses `if: always()` and fails unless every
required signal succeeded.

Each platform workflow contains these jobs:

- `static-policy` runs repository, release-policy, public/private-boundary, and
  explicitly classified source-token inventory checks without depending on
  compilation. These results are non-behavioral.
- `build` configures and compiles without depending on static policy. It uploads
  a compressed, one-day bounded build handoff for downstream jobs.
- `unit-runtime-tests` restores that build and runs release tests that are not
  classified as process/integration or source-token tests.
- `process-integration-tests` restores the same build and runs release tests
  carrying at least one process/integration label.
- `packaging` restores the build and independently verifies the staged public
  and native-only bundles. macOS also builds and smokes its installer artifacts.
- `public-release-linux`, `public-release-windows`, or `public-release-macos`
  aggregates every required result and fails closed.

The release-test universe is the union of tests labelled
`public_release_correctness` or `engine_listener_enterprise`. The process and
integration partition currently recognizes the exact labels `integration`,
`e2e`, `live_server`, `process_kill`, and
`database_lifecycle_server_route`. The unit/runtime partition excludes that
same set, so the partitions are non-overlapping. `run_ctest_chunks.py` refuses
an empty selection and records the selected and excluded inventories.

Release summaries report runtime-observable results first, then any separately
qualified durable/reopen evidence, process-level results, model/property
results, static contracts, and source-token checks. Build and packaging remain
separate delivery signals. A source-token pass cannot be reported as runtime,
crash, fuzz, sanitizer, or durability evidence; see
`project/docs/testing/EVIDENCE_REPORTING.md`.

The configured build handoff is derived from CTest's selected release
inventory. It contains CTest/install metadata, the production object graph,
selected test commands, native packaging executables, and runtime
libraries/resources. It excludes unrelated test executables and test object
trees; uploading the complete configured build is intentionally forbidden.
The handoff is transferred as a zstd-compressed tar so executable modes and
symlinks survive job boundaries.

Every signal-producing job stages bounded UTF-8 diagnostics with
`stage_textual_failure_proof.py` and uploads them under `if: always()`. Build
handoffs are passed separately as short-lived archives and are never used as
failure-proof uploads. macOS keeps architecture-specific signal artifacts and
requires the universal package signal whenever both architectures are selected.

When changing test classification, update `SB_CI_PROCESS_LABELS` consistently
in all three platform workflows and run:

```sh
python3 project/tools/release/github_actions_static_gate.py
```

This gate verifies job independence, the shared test partition, diagnostic
uploads, build-artifact handoff, and the fail-closed aggregate dependency set.

## Critical-path review signal

`critical-path-review.yml` is a separate merge-policy workflow. It runs trusted
default-branch policy on pull-request and review changes, derives affected
critical domains from `.github/critical-path-reviewers.json`, and publishes the
`critical-path/independent-review` status on the pull request's current head.
It never checks out pull-request code under `pull_request_target`.

The status passes automatically for noncritical paths. Before public beta, the
policy is advisory and also publishes success when a critical change lacks an
independent approval; Dalton remains the sole owner and authority. Once the
explicit public-beta transition enables required mode, a critical change needs
a current approval from a configured human owner who is not the PR author. The
approval and PR body must then identify concrete invariants, runtime evidence,
and remaining uncertainty. See
`project/docs/testing/critical_path_review_policy.md` for the review format,
owner maintenance, required CI contexts, and ruleset activation procedure.
