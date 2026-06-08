# Firebird Definition of Done

Status: P0 seed
Search key: `FIREBIRD_DEFINITION_OF_DONE`

## Closure Rule

The Firebird parser, parser-support UDR, service/wire bridge, emulation layer, diagnostics, and regression suite are done only when every Firebird-visible SQL, API, BLR, service, metadata, utility, diagnostic, and donor-native test surface has an assigned owner, implementation or emulation behavior, diagnostic behavior where applicable, and passing tests. No Firebird-visible surface may be removed from scope because it is hard, storage-facing, utility-facing, legacy, file-oriented, or currently missing exact extraction.

## Required Completion Criteria

- `FIREBIRD_SOURCE_AUTHORITY_MAP.csv` and `FIREBIRD_SOURCE_HASH_AUDIT.md` identify every source/spec/reference input used by generated registries, with hash evidence for local donor inputs.
- `FIREBIRD_PACKAGE_BOUNDARY_MAP.csv` passes dependency validation: Firebird packages may share Firebird-owned code, but may not depend on SBSQL, PostgreSQL, MySQL, other donor parser/UDR packages, donor client libraries, or Firebird runtime tools.
- Every row in the Firebird surface registry is assigned to one of: implemented SBLR lowering, implemented bridge/service response, emulated catalog/report operation, invalid-input diagnostic, or authority-violation diagnostic.
- Every SQL, API, BLR, message BLR, SQLDA, DPB, TPB, SPB, BPB, service, utility, catalog, datatype, builtin, PSQL, trigger, routine, package, wire, and diagnostic row has generated or donor-native test coverage.
- The parser worker `sbp_firebird` is untrusted and performs no SQL execution, engine mutation, file opening, or security authority enforcement.
- The parser-support UDR `sbup_firebird` is trusted only after registration, signature, version, ABI, and policy checks, and uses only Firebird-owned sharing with `sbp_firebird`.
- ScratchBird core starts and runs with no Firebird package installed; package absence, disabled package, incompatible package, untrusted package, and policy-denied package cases have explicit diagnostics and tests.
- The ScratchBird engine receives only SBLR/internal procedure operations and UUID-resolved descriptors; donor SQL/API/BLR/wire frames never enter the engine as executable authority.
- Firebird file/storage/admin surfaces have zero real Firebird file effects in runtime products and are implemented as ScratchBird logical operations, emulated reports, or exact authority-violation diagnostics.
- Firebird catalog, monitoring, security, and optional INFORMATION_SCHEMA-compatible surfaces are projections over ScratchBird authority, not imported Firebird catalog/security authority.
- Donor-native Firebird tools are built and run only by CTest in a sandboxed test area with loopback-only endpoints, strict timeouts, no system install, no external network, preserved logs, and deterministic cleanup.
- Donor-original regression roots are acquired or explicitly recorded with an unresolved evidence blocker that prevents final closure; once acquired, replay classification must be complete.
- Final validation passes all mandatory Firebird CTest labels from the execution_plan, including package boundary, SBLR lowering, UDR dynamic SQL, bridge/service, non-file emulation, status-vector diagnostics, donor tool sandbox, original regression replay, differential oracle, fuzz, runtime absence, and zero-open audit gates.

## Evidence Required For Final Sign-Off

- Generated registry lint output proving zero unassigned Firebird-visible rows.
- Package dependency graph output proving no forbidden cross-dialect or donor-runtime dependencies.
- Hash audit output proving source/reference inputs match the audited baseline or an approved updated baseline.
- CTest logs for narrow, standard, exhaustive, donor-native, differential-oracle, fuzz, and runtime-absence lanes.
- Donor-native result normalization report with every non-exact pass classified as expected normalized behavior, expected emulation, expected authority violation, or expected invalid input.
- Operational failure inventory showing all failed tests, rerun commands, preserved logs, current disposition, and owner.
- Final audit showing no Firebird storage, recovery, filesystem, security database, optimizer, catalog, or transaction authority was imported into ScratchBird core.

## Current P0 Evidence Gaps

- Donor-original regression roots are unresolved in `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression/SOURCE_POINTERS.md`.
- Catalog exact rowset extraction status records missing exact rowsets and rowset hashes in `project/tests/donor_regression/donor_catalog_seeds/firebird/firebird_5_exact_rowset_extraction_status.md`.
- The initial P0 artifacts are seed/family-level rows; generated exact row expansion remains required before implementation can claim zero-open Firebird surface closure.
