# Full SBSQL Parser/UDR/Engine Closure Artifacts

Status: complete
Search key: `FULL-SBSQL-PARSER-UDR-ENGINE-ARTIFACTS`

This directory holds generated implementation backlogs, matrix coverage reports, validation evidence, and no-defer audits for the full SBSQL parser/UDR/server/engine closure execution_plan.

## Required Artifact Families

| Artifact | Owning slice | Purpose |
| --- | --- | --- |
| `AGENT_ORCHESTRATION_PLAN.md` | `FSPE-000A` | Coordinator, worker assignment, write-scope, and five-minute validation cadence. |
| `BASELINE_BUILD_CAPABILITY_INVENTORY.md` | `FSPE-000B` | Build targets, feature gates, LLVM/real128/library capability inventory. |
| `REGISTRY_FAMILY_BATCHING_PLAN.csv` | `FSPE-000C` | Deterministic row batching by surface/function/statement family. |
| `FEATURE_PROFILE_CLUSTER_GATE_POLICY.md` | `FSPE-000D` | Standalone, dev-only, donor-profile, and cluster-private gating rules. |
| `DEFINITION_OF_DONE_CONTRACT.md` | `FSPE-000E` | Exact implemented-in-full standard for all slices. |
| `VALIDATION_COMMAND_MATERIALIZATION.csv` | `FSPE-000F` | Runnable pre-code validation commands and assigned future gate contracts. |
| `p0_precode_validation.py` | `FSPE-000F` | Runnable validation script for P0 through P0L gates. |
| `P0_P0L_VALIDATION_RESULT.md` | `FSPE-000F` - `FSPE-000L` | Consolidated passed validation result for all pre-code gates. |
| `BATCH_ROW_MEMBERSHIP.csv` | `FSPE-000G` | Explicit surface-to-batch fixture membership. |
| `REGRESSION_FIXTURE_ORACLE_PREPLAN.md` | `FSPE-000H` | Fixture roots, sharding rules, and expected-result authority plan. |
| `SEMANTIC_ORACLE_AUTHORITY_MAP.csv` | `FSPE-000H` | Per-surface oracle source assignments. |
| `AUTHORITY_IMPORT_AUDIT.md` | `FSPE-000J` | Authority chain and implementation-matrix use audit. |
| `RESOURCE_BUDGET_POLICY.md` | `FSPE-000K` | Parser/server/SBLR/diagnostic/cache/fixture resource limits. |
| `CANARY_VERTICAL_SLICE_PLAN.md` | `FSPE-001A` | Representative pre-scale canary execution_plan. |
| `CANARY_VERTICAL_SLICE_RESULT.md` | `FSPE-001A` | Canary execution results and closure evidence. |
| `FSPE_001_VALIDATION_RESULT.md` | `FSPE-001` | Generated registry and registry-linter validation evidence. |
| `FSPE_002_VALIDATION_RESULT.md` | `FSPE-002` | Lexer token/literal/span/trivia validation evidence. |
| `FSPE_003_VALIDATION_RESULT.md` | `FSPE-003` | CST/AST source-artifact validation evidence. |
| `FSPE_004_VALIDATION_RESULT.md` | `FSPE-004` | Expression/builtin registry descriptor and exact behavior/refusal validation evidence. |
| `FSPE_005_VALIDATION_RESULT.md` | `FSPE-005` | Statement-family registry descriptor and AST handoff validation evidence. |
| `FSPE_006_VALIDATION_RESULT.md` | `FSPE-006` | Binder authority-safe BoundAST metadata and resolver-gate validation evidence. |
| `FSPE_007_VALIDATION_RESULT.md` | `FSPE-007` | Parser-side SBLR lowering envelope and verifier validation evidence. |
| `FSPE_008_VALIDATION_RESULT.md` | `FSPE-008` | Parser-support UDR trusted-context and fail-closed validation evidence. |
| `FSPE_009_VALIDATION_RESULT.md` | `FSPE-009` | Engine-owned API/SBLR behavior-family and public ABI operation-envelope validation evidence. |
| `FSPE_010_VALIDATION_RESULT.md` | `FSPE-010` | Server SBLR admission/runtime route and full-route validation evidence. |
| `FSPE_010A_VALIDATION_RESULT.md` | `FSPE-010A` | Diagnostic/message-vector, parser-rendering, no-raw-string, and validation caveat evidence. |
| `FSPE_010B_VALIDATION_RESULT.md` | `FSPE-010B` | Partial streaming cursor lifecycle validation evidence; does not close full P10B scope. |
| `FSPE_010B1_VALIDATION_RESULT.md` | `FSPE-010B1` | Engine-backed row batch streaming validation evidence. |
| `FSPE_010B2_VALIDATION_RESULT.md` | `FSPE-010B2` | Server cursor protocol completion validation evidence. |
| `FSPE_010B3_VALIDATION_RESULT.md` | `FSPE-010B3` | Parser/client full-route streaming validation evidence. |
| `FSPE_010B4_VALIDATION_RESULT.md` | `FSPE-010B4` | Chunked SBPS payload assembly validation evidence. |
| `FSPE_010B5_VALIDATION_RESULT.md` | `FSPE-010B5` | COPY/import/export/load streaming validation evidence. |
| `FSPE_010B6_VALIDATION_RESULT.md` | `FSPE-010B6` | Multi-result statement sequencing validation evidence. |
| `FSPE_010B7_VALIDATION_RESULT.md` | `FSPE-010B7` | Warning-chain and partial-result diagnostic validation evidence. |
| `FSPE_010B8_VALIDATION_RESULT.md` | `FSPE-010B8` | Timeout/cancel/drain/parser-kill finality validation evidence. |
| `FSPE_010B9_VALIDATION_RESULT.md` | `FSPE-010B9` | FSPE-010B completion gate and parent closure evidence. |
| `FSPE_011_VALIDATION_RESULT.md` | `FSPE-011` | Generated full-surface conformance command and result evidence. |
| `PARSER_COVERAGE_REPORT.md` | `FSPE-011` | Mechanical registry, operation matrix, engine gap, donor alias, message-vector, and batch coverage evidence. |
| `FSPE_011A_VALIDATION_RESULT.md` | `FSPE-011A` | Reusable regression test-bed policy and manifest gate evidence. |
| `SERVER_ENGINE_GAP_CLOSURE_REPORT.md` | `FSPE-009` / `FSPE-010` | Engine gap closure evidence and server handoff boundary report. |
| `FULL_ROUTE_CONFORMANCE_REPORT.md` | `FSPE-010` | Listener/parser/server/engine full-route smoke and server admission conformance evidence. |
| `MESSAGE_VECTOR_COVERAGE_BACKLOG.csv` | `FSPE-000I` / `FSPE-010A` | Seed and closure diagnostic/message-vector coverage for parser, UDR, server, engine, and agents. |
| `MESSAGE_VECTOR_COVERAGE_REPORT.md` | `FSPE-010A` | Message-vector backlog, runtime shape, parser rendering, and no-raw-string diagnostic gate evidence. |
| `STREAMING_RESULT_PROTOCOL_REPORT.md` | `FSPE-010B` | Partial streaming cursor protocol report and remaining P10B gap inventory. |
| `REGRESSION_TEST_BED_PLAN.md` | `FSPE-011A` | Shardable CTest-ready regression fixture policy. |
| `FSPE_011B_VALIDATION_RESULT.md` | `FSPE-011B` | Donor alias mapping/rendering gate evidence. |
| `DONOR_ALIAS_RENDERING_REPORT.md` | `FSPE-011B` | Donor profile alias rendering fixture coverage and result-shape policy. |
| `SEMANTIC_ORACLE_AUTHORITY_MAP.csv` | `FSPE-011C` | Expected-result authority for generated tests. |
| `SEMANTIC_ORACLE_AUTHORITY_REPORT.md` | `FSPE-011C` | Semantic oracle authority coverage and validation summary. |
| `FSPE_011C_VALIDATION_RESULT.md` | `FSPE-011C` | Semantic oracle authority gate evidence. |
| `PERSISTENCE_RESTART_CONFORMANCE_REPORT.md` | `FSPE-011D` | Persistence/restart conformance coverage and validation summary. |
| `FSPE_011D_VALIDATION_RESULT.md` | `FSPE-011D` | Persistence/restart CTest, full parser-worker label, and P0 validation evidence. |
| `CONCURRENT_SESSION_TRANSACTION_REPORT.md` | `FSPE-011E` | Concurrent session, transaction, lock, savepoint, disconnect, stale-cache, and cache-epoch conformance coverage. |
| `FSPE_011E_VALIDATION_RESULT.md` | `FSPE-011E` | Concurrent session transaction CTest, full parser-worker label, and P0 validation evidence. |
| `DIFFERENTIAL_REPLAY_HARNESS_PLAN.md` | `FSPE-011F` | Replay route and fixture schema. |
| `DIFFERENTIAL_REPLAY_HARNESS_REPORT.md` | `FSPE-011F` | Replay execution result and failure closure report. |
| `FSPE_011F_VALIDATION_RESULT.md` | `FSPE-011F` | Differential replay harness CTest, full parser-worker label, and P0 validation evidence. |
| `HARDENING_GATE_REPORT.md` | `FSPE-012` | No-spin/no-WAL/no-direct-parser-DB, resource-budget, and metrics hardening evidence. |
| `FSPE_012_VALIDATION_RESULT.md` | `FSPE-012` | P12 hardening CTest, adjacent boundary gates, and full parser-worker label evidence. |
| `CACHE_EPOCH_CORRECTNESS_REPORT.md` | `FSPE-012A` | Parser SBLR cache key dimensions, invalidation, and metrics evidence. |
| `FSPE_012A_VALIDATION_RESULT.md` | `FSPE-012A` | Cache epoch CTest, concurrent cache regression, and full parser-worker label evidence. |
| `FUZZ_MALICIOUS_INPUT_REPORT.md` | `FSPE-012B` | Malicious SQL, resource abuse, hostile SBPS packet, UDR fail-closed, and server SBLR admission evidence. |
| `FSPE_012B_VALIDATION_RESULT.md` | `FSPE-012B` | Fuzz malicious-input CTest and full parser-worker label evidence. |
| `DETERMINISTIC_NO_NETWORK_REPORT.md` | `FSPE-012C` | Deterministic generated artifact and no-network build/test evidence. |
| `FSPE_012C_VALIDATION_RESULT.md` | `FSPE-012C` | Deterministic no-network CTest and full parser-worker label evidence. |
| `CROSS_PLATFORM_IPC_REPORT.md` | `FSPE-012D` | Cross-platform path process IPC inherited-handle cleanup and endpoint behavior evidence. |
| `FSPE_012D_VALIDATION_RESULT.md` | `FSPE-012D` | Cross-platform IPC CTest, deterministic manifest repair, full parser-worker label, and P0 validation evidence. |
| `LOCALE_CHARSET_COLLATION_TIMEZONE_REPORT.md` | `FSPE-012E` | Locale charset collation timezone parser and engine regression evidence. |
| `FSPE_012E_VALIDATION_RESULT.md` | `FSPE-012E` | Locale charset collation timezone CTest, deterministic manifest repair, full parser-worker label, and P0 validation evidence. |
| `METADATA_RESULT_SHAPE_REPORT.md` | `FSPE-012F` | Metadata result-shape command-tag warning cursor and donor-rendering evidence. |
| `FSPE_012F_VALIDATION_RESULT.md` | `FSPE-012F` | Metadata result-shape CTest, deterministic manifest repair, full parser-worker label, and P0 validation evidence. |
| `SECURITY_REDACTION_SIDE_CHANNEL_REPORT.md` | `FSPE-012G` | Hidden-object, metadata, diagnostic, cache, timing, and donor-rendering leakage gate. |
| `FSPE_012G_VALIDATION_RESULT.md` | `FSPE-012G` | Security redaction side-channel CTest, deterministic manifest repair, full parser-worker label, and P0 validation evidence. |
| `UPGRADE_MIGRATION_COMPATIBILITY_REPORT.md` | `FSPE-012H` | Registry, SBLR, catalog seed, cache, and database compatibility gate. |
| `FSPE_012H_VALIDATION_RESULT.md` | `FSPE-012H` | Upgrade/migration compatibility CTest, deterministic manifest, full parser-worker label, and adjacent lifecycle evidence. |
| `FIXED_UUID_CATALOG_SEED_REPORT.md` | `FSPE-013A` | Standard surface UUID seed and catalog lookup verification evidence. |
| `FSPE_013A_VALIDATION_RESULT.md` | `FSPE-013A` | Fixed UUID catalog seed CTest, deterministic manifest, full parser-worker label, and P0 validation evidence. |
| `SPEC_SYNCHRONIZATION_AUDIT.md` | `FSPE-013` | Manifest, matrix, generated artifact, execution_plan evidence, and authority-boundary synchronization audit. |
| `FSPE_013_VALIDATION_RESULT.md` | `FSPE-013` | Spec synchronization audit command, full parser-worker label, deterministic manifest, and P0 validation evidence. |
| `DEVELOPER_HANDOFF_IMPLEMENTATION_MAP.csv` | `FSPE-013B` | Surface-family to parser/UDR/SBLR/server/engine/test file map. |
| `developer_handoff_map_gate.py` | `FSPE-013B` | Runnable map coverage, path, generated-vs-handwritten, count, batch, and validation label gate. |
| `FSPE_013B_VALIDATION_RESULT.md` | `FSPE-013B` | Developer handoff implementation map gate evidence. |
| `SOURCE_SIZE_MAINTAINABILITY_REPORT.md` | `FSPE-014A` | Source-size and logical-family split gate evidence. |
| `source_size_maintainability_gate.py` | `FSPE-014A` | Runnable handwritten/generated source-size threshold and exception gate. |
| `FSPE_014A_VALIDATION_RESULT.md` | `FSPE-014A` | Source-size maintainability gate evidence. |
| `ZERO_SQL_ENGINE_BOUNDARY_AUDIT.md` | `FSPE-014B` | Static/source audit proving SQL text does not enter engine execution as authority. |
| `zero_sql_engine_boundary_gate.py` | `FSPE-014B` | Runnable server/UDR/SBLR/engine zero-SQL boundary gate. |
| `FSPE_014B_VALIDATION_RESULT.md` | `FSPE-014B` | Zero-SQL engine-boundary gate evidence. |
| `FULL_REGRESSION_SUITE_PUBLICATION.md` | `FSPE-014C` | Full regression commands labels fixture roots runtime triage and cleanup process. |
| `full_regression_suite_publication_gate.py` | `FSPE-014C` | Runnable publication completeness gate. |
| `FSPE_014C_VALIDATION_RESULT.md` | `FSPE-014C` | Full regression suite publication gate evidence. |
| `KNOWN_RISK_BURN_DOWN_REPORT.md` | `FSPE-014D` | Corrected issues and residual non-blocking risk closure evidence. |
| `known_risk_burn_down_gate.py` | `FSPE-014D` | Runnable known-risk closure assertion gate. |
| `FSPE_014D_VALIDATION_RESULT.md` | `FSPE-014D` | Known-risk burn-down gate evidence. |
| `CLEANUP_ARTIFACT_RETENTION_POLICY.md` | `FSPE-000L` / `FSPE-014E` | Pre-generation and final cleanup, redaction, evidence retention, and disk growth rules. |
| `cleanup_artifact_retention_gate.py` | `FSPE-014E` | Runnable final cleanup and artifact-retention completeness gate. |
| `FSPE_014E_VALIDATION_RESULT.md` | `FSPE-014E` | Cleanup and artifact-retention gate evidence. |
| `BENCHMARK_BASELINE_REPORT.md` | `FSPE-014F` | Non-tuning timing/memory baseline for regression tracking. |
| `benchmark_baseline_gate.py` | `FSPE-014F` | Runnable benchmark baseline completeness and non-placeholder gate. |
| `FSPE_014F_VALIDATION_RESULT.md` | `FSPE-014F` | Benchmark baseline command, timing, memory, and gate evidence. |
| `EXHAUSTIVE_E2E_REGRESSION_REPORT.md` | `FSPE-014G` | Registered-surface route, dynamic UDR/SBLR, server admission, and engine execution regression evidence. |
| `FSPE_014G_VALIDATION_RESULT.md` | `FSPE-014G` | Exhaustive E2E CTest and deterministic manifest validation evidence. |
| `final_zero_unimplemented_audit.py` | `FSPE-014` | Runnable final status, count, no-defer, engine-gap, authority, and boundary closure gate. |

## Closure Rule

No artifact may be left as a placeholder at final audit. Each required artifact must contain the evidence fields named by its owning slice and must be linked from the final execution_plan audit.
