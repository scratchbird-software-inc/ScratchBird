# Upgrade and Migration Compatibility Report

Status: complete
Search key: `FSPE-UPGRADE-MIGRATION-COMPATIBILITY-REPORT`
Owning slice: `FSPE-012H`
Date: 2026-05-08

## Compatibility Areas

The database version matrix classifies every open path as current, upgrade-required, read-only-compatible, or unsupported. An unsupported database opened as writable is a blocker and must fail closed before parser registry, catalog seed, or SBLR admission can proceed.

| Area | Implemented evidence |
| --- | --- |
| Parser registry version | `parser_package_registry` retains `SBPPR1`, parser API major, SBPS min/max bounds, version-range checks, unsupported-registry diagnostics, and fail-closed package-state admission. |
| SBLR envelope version | Server admission refuses unsupported `SBLRExecutionEnvelope.*` versions and explicit incompatible envelope major/version fields before accepting SBLR. Engine/internal API version gates remain exact-major and supported-minor only. |
| Catalog seed UUIDs | FSPE-013A fixed UUID catalog seed gate is now part of the parser-worker regression label and is consumed by the upgrade compatibility gate. |
| Fixture regeneration | Generated fixture compatibility rows distinguish current, additive, read-only-compatible, incompatible, and unsupported migrations with exact refusal diagnostics. |
| Database create/open | Database format validation now emits canonical `FORMAT.VERSION_UNSUPPORTED` and `FORMAT.UNKNOWN_REQUIRED_FLAG`; open state exposes `DatabaseOpenCompatibilityClass` with current, upgrade-required, read-only-compatible, and unsupported classes. |
| Database authority flags | Database open fails closed when cluster authority or decryption is required but unavailable, classifies MGA transaction inventory before write admission, and fences writes for read-only opens. |
| Seed-pack policy | Database open can compare persisted seed-pack name, version, and content hash against expected policy and refuses writable stale seed packs with `FORMAT.UPGRADE_REQUIRED`. |
| Parser cache | Cache keys include registry/catalog/security/schema/grant/role/group/search/language/profile/result-contract dimensions and support registry-version invalidation. |

## SBLR Envelope Compatibility Matrix

The SBLR envelope compatibility matrix requires exact-major compatibility for accepted envelopes, canonical refusal for unsupported `SBLRExecutionEnvelope.*` versions, and no fallback to SQL text or parser-owned authority when server admission rejects an incompatible envelope.

## Generated Gate Assets

| Asset | Purpose |
| --- | --- |
| `project/tests/sbsql_parser_worker/generated/upgrade/UPGRADE_MIGRATION_COMPATIBILITY_FIXTURES.csv` | Deterministic compatibility fixture matrix for parser registry, SBLR envelopes, catalog seeds, fixture regeneration, and database open behavior. |
| `project/tests/sbsql_parser_worker/generated/upgrade/sbsql_upgrade_migration_compatibility_gate.cpp` | Static and fixture-backed gate for FSPE-012H compatibility evidence. |
| `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` | Updated deterministic manifest including both generated upgrade artifacts. |

## Validation Summary

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_upgrade_migration_compatibility_gate -j 4` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_persistence_restart_conformance sbsql_cache_epoch_correctness_conformance sbsql_security_redaction_side_channel_gate -j 4` | passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_upgrade_migration_compatibility_gate --output-on-failure` | passed 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L "sbsql_persistence_restart_conformance|sbsql_cache_epoch_correctness_conformance|sbsql_security_redaction_side_channel_gate|sbsql_upgrade_migration_compatibility_gate" --output-on-failure` | passed 4/4 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | passed 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | passed 39/39 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | passed |

## Closure Rule Result

No silent remap, UUID reuse, stale parser cache acceptance, unsupported database writable-open path, SQL-to-engine authority path, or WAL recovery authority was introduced by this slice. FSPE-012H is complete and FSPE-013 can open as the next serialized slice.
