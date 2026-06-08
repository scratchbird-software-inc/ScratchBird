# DBLC-015 Observability Diagnostics Audit Metrics Cache Invalidation Report

Status: implementation evidence prepared for `DBLC_P15_OBSERVABILITY_COMPLETE`.

This report is traceability evidence only. It records the implementation anchors for lifecycle diagnostics, message vectors, metrics, audit evidence, and cache invalidation markers delivered in the DBLC-015 owned scope. Execution_Plan trackers and acceptance gates remain coordinator-owned.

## Evidence Summary

| Requirement | Evidence |
| --- | --- |
| Canonical public/private diagnostic vectors | `project/src/server/diagnostics.cpp` implements `ToMessageVectorJsonLine` and `ToPrivateMessageVectorJsonLine`, with public redaction, private correlation evidence, diagnostic shape IDs, retryability, and parser/donor finality denial booleans. |
| Server lifecycle metrics/audit/cache markers | `project/src/server/server_observability.cpp` implements `RecordServerLifecycleObservability`, `CanonicalLifecycleObservabilityOperations`, `LifecycleOperationRequiresCacheInvalidation`, lifecycle metric families, audit events, message-vector counters, and `ServerCacheInvalidationMarker` storage. |
| Management lifecycle route integration | `project/src/server/manager_control.cpp` records lifecycle observability for create/open/attach/detach, maintenance/restricted-open, inspect/diagnose/verify/repair, shutdown/force shutdown, drop, parser/session/metrics/supportability routes, and refusal paths before error responses. |
| Maintenance coordinator traceability | `project/src/server/maintenance_coordinator.cpp` includes `DBLC_P15_OBSERVABILITY_COMPLETE`, message-vector shape, cache-invalidation requirement, and parser/donor finality-denial fields in coordinator records. |
| Engine metrics API evidence | `project/src/engine/internal_api/observability/metrics_api.cpp` implements `EngineRecordLifecycleMetric`, registering and updating `sb_lifecycle_operation_total`, `sb_lifecycle_diagnostic_total`, `sb_lifecycle_cache_invalidation_total`, and audit-linked lifecycle metrics under `sys.metrics.lifecycle.*`. |
| Engine audit API evidence | `project/src/engine/internal_api/security/audit_api.cpp` implements `EngineEmitLifecycleAuditEvent` with redacted audit rows, lifecycle cache marker linkage, public/private shape separation evidence, and no parser/donor finality authority. |
| Parser diagnostic rendering | `project/src/engine/internal_api/diagnostics/diagnostic_rendering.cpp` marks lifecycle diagnostics with public/private shape IDs, retryability, redaction class, correlation evidence, and validation failures for parser or donor finality authority claims. |
| Conformance coverage | `project/tests/database_lifecycle/observability_conformance.cpp` covers diagnostic redaction, success/refusal lifecycle observability, metrics exposure, audit emission, cache invalidation, parser rendering, retryability, and no-authority-drift checks. |
| Static gate | `project/tests/database_lifecycle/observability_static.py` implements `DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT` and verifies required anchors and forbidden shortcut tokens. |

## Lifecycle Surface Coverage

Covered operation families: create database, open database, attach, detach, begin transaction, commit transaction, rollback transaction, enter/exit maintenance, enter/exit restricted-open, inspect/diagnose, verify, repair, graceful shutdown, force shutdown, shutdown acknowledgement, drop, IPC/session route, parser package route, metrics/supportability route, upgrade/refusal evidence, and donor mapping render evidence.

The server recorder treats successful mutating lifecycle operations as cache-invalidation candidates and records explicit invalidation markers. Refusals emit diagnostics, audit, and metrics without publishing success cache markers. Parser and donor surfaces receive rendered diagnostics only; they do not become transaction, recovery, cache, or lifecycle finality authority.

## Gates

Required gate markers:

| Gate | Evidence |
| --- | --- |
| `DBLC_P15_OBSERVABILITY_COMPLETE` | Implementation anchors above plus observability conformance test. |
| `database_lifecycle_supportability_evidence` | Existing supportability label remains the required CTest label for DBLC-015 until coordinator wires direct observability CTest. |
| `DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT` | `observability_static.py` validates diagnostic/message-vector coverage. |

Suggested coordinator-owned CMake snippet:

```cmake
add_executable(database_lifecycle_observability_conformance
  observability_conformance.cpp
)
target_compile_features(database_lifecycle_observability_conformance PRIVATE cxx_std_23)
target_link_libraries(database_lifecycle_observability_conformance
  PRIVATE
    sb_engine_internal_api
    sb_server_core
)
add_test(
  NAME database_lifecycle_observability_conformance
  COMMAND database_lifecycle_observability_conformance
)
set_tests_properties(database_lifecycle_observability_conformance PROPERTIES
  LABELS "database_lifecycle_observability;database_lifecycle_supportability_evidence;DBLC_P15_OBSERVABILITY_COMPLETE;database_lifecycle;mga_transaction_regression"
)

add_test(
  NAME database_lifecycle_observability_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/observability_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_observability_static PROPERTIES
  LABELS "database_lifecycle_observability;database_lifecycle_supportability_evidence;DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT;DBLC_P15_OBSERVABILITY_COMPLETE;database_lifecycle"
)
```

## Authority Notes

No embedded-database shortcut, configuration-directive shortcut, authoritative write-ahead recovery shortcut, parser-managed finality, donor-managed finality, stubbed observability claim, or deferred evidence shortcut is used in the DBLC-015 owned implementation. Transaction finality remains engine/MGA-owned; observability records evidence and renders diagnostics only.
