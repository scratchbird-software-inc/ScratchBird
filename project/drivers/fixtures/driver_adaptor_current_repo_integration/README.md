# Driver, Adaptor, and Tool Current-Repo Integration Closure Execution_Plan

Status: implementation pass complete; live fixture and packaging release gates remain open
Created: 2026-05-08
Owner: ScratchBird driver/adaptor/tool integration coordinator
Search key: `DRIVER-ADAPTOR-TOOL-CURRENT-REPO-INTEGRATION-CLOSURE`

## Purpose

Make the imported driver, adaptor, and tool source trees first-class
ScratchBird components:

```text
project/drivers/{driver,adaptor,tool}
  -> current-repo path realignment
  -> current server/wire/API refactor
  -> isolated build artifacts under build/
  -> CTest-managed build, test, packaging, conformance, and benchmark gates
  -> release-claim and drift-prevention evidence
```

The execution_plan exists to prevent the imported driver source from drifting back to
the old external repository model. After closure, a full CTest run must be able
to rebuild and retest all supported drivers, adaptors, and tools from the
current repository only.

## Scope

The closure covers:

- Actual drivers under `project/drivers/driver/`.
- Adaptors under `project/drivers/adaptor/`.
- CLI and operational tools under `project/drivers/tool/`.
- Build output isolation for every driver/adaptor/tool language and package
  ecosystem.
- Path realignment away from old `ScratchBird-driver`, `tracks/p3`, legacy
  ScratchBird, and absolute local paths.
- Refactoring to the current `sb_server`, wire, parser, authentication,
  metadata, transaction, error, type, and benchmark surfaces.
- CTest integration for build, unit, integration, conformance, packaging, and
  benchmark-readiness gates.
- Drift-prevention checks that fail on generated artifacts, source-tree writes,
  old-path references, missing toolchains without explicit waiver, or
  unsupported release claims.

## Imported Source Boundary

The active source import is recorded in `project/drivers/IMPORT_MANIFEST.md`.
This execution_plan treats that import as the source baseline.

Current imported categories:

- Drivers: `cpp`, `dart`, `dotnet`, `elixir`, `go`, `jdbc`, `mojo`, `node`,
  `odbc`, `pascal`, `php`, `python`, `r`, `ruby`, `rust`, `swift`.
- Adaptors: `scratchbird-dbeaver-driver`, `scratchbird-hibernate-dialect`,
  `scratchbird-metabase-driver`, `scratchbird-prisma-adapter`,
  `scratchbird-sqlalchemy-dialect`, `scratchbird-superset-driver`,
  `scratchbird-typeorm-adapter`.
- Tools: `cli`.

Planned placeholder lanes from the old driver project are not part of this
initial closure unless source is later imported into `project/drivers/`.

## Non-Negotiable Rules

- All build, test, generated, package, dependency, cache, and benchmark outputs
  must be under the current repo `build/` tree.
- No driver/adaptor/tool may require the old `ScratchBird-driver` repository at
  build time, test time, benchmark time, or runtime.
- No driver/adaptor/tool may hardcode user-local absolute paths.
- No driver/adaptor/tool may link directly to private engine internals.
- Drivers and tools must communicate through supported public server, wire,
  parser, or driver APIs.
- CTest is the authority for regression inclusion. Out-of-band language test
  commands may exist only when wrapped by CTest targets.
- Missing required toolchains must fail the required gate unless an explicit
  CMake waiver option marks that driver/adaptor/tool as intentionally skipped.
- Lockfiles are allowed when they are source-controlled dependency authority.
  Downloaded dependency trees are not source artifacts.
- Full driver/adaptor/tool closure must include static hygiene gates that fail
  on source-tree writes, generated artifacts, old-path references, or release
  claims without passing evidence.

## Required P0 Artifacts

P0 is complete only when these artifacts exist and are internally consistent:

- `artifacts/DRIVER_SOURCE_INVENTORY.csv`
- `artifacts/BUILD_ARTIFACT_ISOLATION_POLICY.md`
- `artifacts/PATH_REALIGNMENT_BACKLOG.csv`
- `artifacts/TOOLCHAIN_AND_DEPENDENCY_POLICY.md`
- `artifacts/DRIVER_COMMON_CONFORMANCE_MATRIX.csv`
- `artifacts/CTEST_DRIVER_GATE_MATRIX.csv`
- `artifacts/SERVER_FIXTURE_LIFECYCLE_POLICY.md`
- `artifacts/BENCHMARK_DRIVER_READINESS_POLICY.md`
- `artifacts/PACKAGING_INSTALL_GATE_POLICY.md`
- `artifacts/STATIC_HYGIENE_GATE_POLICY.md`
- `artifacts/RELEASE_CLAIM_POLICY.md`

## High-Level Implementation Order

```text
P0 inventory, policy, and gate definition
  -> P1 build-system and artifact-isolation substrate
  -> P2 actual driver realignment/refactor/test integration
  -> P3 adaptor realignment/refactor/test integration
  -> P4 tool realignment/refactor/test integration
  -> P5 server fixture, conformance, benchmark, packaging, and hygiene gates
  -> P6 aggregate CTest, release-claim, and final zero-drift closure
```

## Required CTest Labels

Exact target names are materialized during implementation, but these labels are
mandatory:

```text
driver_source_inventory_gate
driver_old_path_gate
driver_build_artifact_isolation_gate
driver_source_tree_write_guard
driver_toolchain_detection_gate
driver_dependency_policy_gate
driver_common_conformance_gate
driver_server_fixture_gate
driver_python_gate
driver_go_gate
driver_rust_gate
driver_node_gate
driver_jdbc_gate
driver_odbc_gate
driver_cpp_gate
driver_dotnet_gate
driver_dart_gate
driver_elixir_gate
driver_mojo_gate
driver_pascal_gate
driver_php_gate
driver_r_gate
driver_ruby_gate
driver_swift_gate
adaptor_dbeaver_gate
adaptor_hibernate_gate
adaptor_metabase_gate
adaptor_prisma_gate
adaptor_sqlalchemy_gate
adaptor_superset_gate
adaptor_typeorm_gate
tool_cli_gate
driver_packaging_install_gate
driver_benchmark_readiness_gate
driver_execution_plan10_runner_gate
driver_static_hygiene_gate
driver_release_claim_gate
drivers_all
drivers_final_zero_drift_audit
```

## Definition Of Done

This execution_plan is complete only when:

- Every imported driver, adaptor, and tool has a current-repo build/test path.
- Every build artifact lands under `build/`.
- Every old external driver path and legacy track path is removed or limited to
  preserved historical reference documentation.
- Every driver/adaptor/tool required by release policy is included in CTest.
- Full CTest can rebuild and retest supported drivers without source-tree
  generated artifacts.
- Execution_Plan 10 benchmarks can use current-repo driver/tool source and emit
  current result JSON under `build/benchmarks/`.
- Release documentation distinguishes `supported`, `experimental`,
  `toolchain-waived`, and `not-imported` status with evidence.
- `drivers_final_zero_drift_audit` passes with zero source-tree output,
  zero old-path hits, zero missing required CTest labels, and zero unsupported
  release claims.
