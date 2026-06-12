# Database Lifecycle Authority Drift Report

Search key: `DATABASE-LIFECYCLE-AUTHORITY-DRIFT-REPORT`

Gate: `DBLC_STATIC_AUTHORITY_DRIFT_GATES`

## Scanned Authority Surfaces

The DBLC-017 authority gate scans these accepted surfaces:

| Surface | Drift blocked |
| --- | --- |
| Storage lifecycle and dirty-manifest recovery | SQLite, PRAGMA, journal-mode, write-ahead, redo/undo authority, and non-MGA recovery language unless used as explicit forbidden-drift evidence. |
| Server SBLR admission and dispatch | Raw SQL, SQL-text envelopes, parser finality authority, and bypass of engine/public-ABI dispatch for transaction control. |
| Server session/authentication | Parser-created sessions after denied auth and non-engine authentication authority. |
| Firebird and SBSQL parser mapping | Reference SQL execution, reference file effects, and source SQL leakage into admitted engine envelopes. |
| MGA cluster transaction and cluster route APIs | Standalone cluster route or cluster transaction admission without cluster authority. |
| Backup/archive lifecycle authority | Authoritative write-ahead backup/archive shortcuts. |

## Closure Markers

Required markers enforced by the static gate include:

| Marker | Meaning |
| --- | --- |
| `RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN` | Dirty recovery evidence rejects write-ahead confusion. |
| `SBLR.SQL_TEXT_FORBIDDEN` | Server admission rejects SQL text at the engine boundary. |
| `requires_public_abi_dispatch` | Transaction control must be routed to engine/public ABI dispatch. |
| `SECURITY.AUTHENTICATION.FAILED` | Engine security denial remains explicit and auditable. |
| `reference_engine_sql_executed:false` | Reference dialect mapping has no reference SQL execution authority. |
| `SB-CLUSTER-MAPPING-UNAVAILABLE` | Standalone cluster transaction path fails closed. |

Final pass evidence is produced by `database_lifecycle_authority_drift_static`.
