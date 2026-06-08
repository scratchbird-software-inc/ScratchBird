# DBLC-013T Threat Model And Abuse Cases

Gate labels: `DBLC_P13T_THREAT_MODEL_COMPLETE`, `database_lifecycle_threat_model`, `DBLC_STATIC_THREAT_MODEL_ABUSE_CASES`.

Search key: `DBLC_P13T_THREAT_MODEL_COMPLETE`.

## Authority Boundary

Engine remains the authentication and authorization authority. Parser/listener/driver state is never finality or security authority. Manager, listener, parser, driver, UDR, plugin, package, service-file, health, backup, restore, support-bundle, and quota evidence may support an engine decision, but it cannot grant rights or transaction finality.

## Covered Abuse Cases

| Surface | Abuse case | Required fail-closed behavior |
| --- | --- | --- |
| force shutdown | Force termination with ambiguous process association, missing force policy, or missing MGA recovery evidence | Refuse before target mutation; preserve unknown transaction finality evidence. |
| IPC authentication | Bound-operation frame without a bound session or security context | Refuse with session/security diagnostic. |
| IPC authorization | Management route with insufficient role/right | Refuse with engine authorization denial. |
| manager supervision | Manager action with ambiguous ownership or stale heartbeat | Refuse supervision action until association and restart policy are current. |
| listener supervision | Listener stop/restart/drain without current association proof | Refuse operation; listener state is not authority. |
| parser supervision | Parser fallback without current engine-visible association | Refuse operation; parser state is never finality authority. |
| UDR loading | UDR/plugin/package load without policy, trusted signature, or parser-package admission | Refuse load before execution or registration. |
| health reporting | Health/status response attempts to expose protected material or local paths | Return least-privilege redacted health only. |
| backup/restore | Backup or restore without lifecycle policy, engine-owned path, or restore inspection-open classification | Refuse before visible backup/restore success. |
| support bundles | Bundle export without engine authorization, retention/redaction policy, flush, or redaction | Refuse before bundle index or descriptor visibility. |
| service files | Lifecycle state or endpoint service file with public permissions, unverified owner, or protected material | Refuse or constrain to private engine-owned service file evidence. |
| resource quota | Parser/listener/manager/UDR route attempts work without reservation or beyond hard limit | Refuse before work starts and record redacted quota evidence. |

## Implementation Evidence

The conformance gate `project/tests/database_lifecycle/threat_model_conformance.cpp` covers the central threat gate and direct runtime paths:

| Requirement | Evidence path |
| --- | --- |
| Force shutdown least privilege and recovery evidence | `ApplyDatabaseShutdownOperation` refuses ambiguous scope and unsafe force termination. |
| IPC authentication/authorization | `HandleServerManagementRequest` refuses missing session and insufficient role; health response carries engine authority and redaction state. |
| Manager/listener/parser supervision | `EvaluateDatabaseLifecycleThreatModelGate` requires exact process association, current heartbeat, and restart policy. |
| UDR/plugin/package loading | `EvaluateFeatureGateRequest` refuses missing security context, non-engine authority, missing policy, and untrusted package signature. |
| Backup/restore | `EvaluateBackupArchiveLifecycleAdmission` refuses missing security context, missing policy, non-engine-owned path, and restore without inspection-open evidence. |
| Support bundles | `EnginePrepareSupportBundle` refuses missing security context, missing engine authorization, missing policy, and redaction bypass. |
| Service files | `ipc_server.cpp` constrains lifecycle state service file mode to owner read/write; static gate checks the private-mode call. |
| Resource quotas | `WorkloadResourceQuotaController` refuses hard-limit and empty-reservation route attempts before work starts. |

## Static Gate

`project/tests/database_lifecycle/threat_model_static.py` validates the required labels, diagnostics, redaction markers, service-file private mode evidence, and absence of threat-gate shortcut markers in P13T-owned files.

No SQLite/PRAGMA/donor shortcut behavior is introduced. No parser, listener, driver, or donor state is accepted as finality or security authority.
