# ScratchBird Functionality Support Matrix

## Purpose

This document is a quick-reference capability matrix for ScratchBird (a Convergent Data Engine). It shows, for each functional area, whether a capability is available in a **Local** deployment (single-node or embedded, open-source tier) or a **Cluster** deployment (with the commercial cluster provider present). Every mark in this document is derived directly from the SBLR opcode registry (`src/engine/sblr/sblr_opcode_registry.cpp`), the agent runtime manifest (`src/core/agents/agent_runtime_manifest.def`), and the cluster command boundary set (`src/cluster_provider/cluster_provider.hpp`). This is a capability reference, not a statement of production readiness; items may have additional build, configuration, or release-status prerequisites.

---

## Legend

| Symbol | Meaning |
|--------|---------|
| ✓ | Supported in this deployment profile |
| ✗ | Not supported in this deployment profile |

**Local** — Single-node engine in any of its open-source operating forms: embedded/library, single-node IPC server, standalone listener server, or managed group. No commercial cluster provider is present.

**Cluster** — The commercial cluster provider is present and the ABI handshake has been accepted. Cluster includes all Local functionality plus the distributed operations listed in Group 17. There are **no** Local-only capabilities that are refused in a cluster; the cluster tier is strictly additive.

---

## Group 1 — Data Definition (DDL)

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Create / alter / drop database | ✓ | ✓ |
| Create / alter / drop schema | ✓ | ✓ |
| Create / alter / drop table | ✓ | ✓ |
| Create / alter / drop index | ✓ | ✓ |
| Create / alter / drop view | ✓ | ✓ |
| Create / alter / drop materialized view (with refresh) | ✓ | ✓ |
| Create / alter / drop domain | ✓ | ✓ |
| Create / alter / drop type (composite, enum, range, etc.) | ✓ | ✓ |
| Create / alter / drop sequence | ✓ | ✓ |
| Create / alter / drop trigger | ✓ | ✓ |
| Create / alter / drop event trigger | ✓ | ✓ |
| Create / alter / drop function | ✓ | ✓ |
| Create / alter / drop procedure | ✓ | ✓ |
| Create / alter / drop package (spec + body) | ✓ | ✓ |
| Create / alter / drop aggregate | ✓ | ✓ |
| Create / alter / drop operator | ✓ | ✓ |
| Create / alter / drop operator class / family | ✓ | ✓ |
| Create / alter / drop cast | ✓ | ✓ |
| Create / alter / drop collation | ✓ | ✓ |
| Create / alter / drop extension | ✓ | ✓ |
| Create / alter / drop synonym | ✓ | ✓ |
| Create / alter / drop foreign table + foreign-data wrapper | ✓ | ✓ |
| Create / alter / drop publication | ✓ | ✓ |
| Create / alter / drop subscription | ✓ | ✓ |
| Create / alter / drop rule | ✓ | ✓ |
| Create / alter / drop dictionary | ✓ | ✓ |
| Create / alter / drop named collection | ✓ | ✓ |
| Add / alter / drop table constraint | ✓ | ✓ |
| Rename object | ✓ | ✓ |
| Comment on object | ✓ | ✓ |
| Drop object (generic) | ✓ | ✓ |
| Create / alter statistics | ✓ | ✓ |
| Create key-value store object | ✓ | ✓ |
| Create time-series object | ✓ | ✓ |
| Create document collection | ✓ | ✓ |
| Create graph / graph node / graph edge / graph index | ✓ | ✓ |
| Cluster placement policy (create / alter / drop) | ✗ | ✓ |
| Declare cluster region | ✗ | ✓ |
| Declare availability zone | ✗ | ✓ |
| Declare data placement policy | ✗ | ✓ |

---

## Group 2 — Data Manipulation (DML)

| Functionality | Local | Cluster |
|---|:---:|:---:|
| INSERT | ✓ | ✓ |
| UPDATE | ✓ | ✓ |
| DELETE | ✓ | ✓ |
| MERGE / UPSERT | ✓ | ✓ |
| TRUNCATE | ✓ | ✓ |
| COPY (bulk import / export stream) | ✓ | ✓ |
| Native bulk ingest | ✓ | ✓ |
| Batch statement execution | ✓ | ✓ |
| Atomic compare-and-set | ✓ | ✓ |
| Atomic read-modify-write | ✓ | ✓ |
| Advisory lock acquire / release | ✓ | ✓ |
| RETURNING clause | ✓ | ✓ |

---

## Group 3 — Query

| Functionality | Local | Cluster |
|---|:---:|:---:|
| SELECT with projection, filtering, aliases | ✓ | ✓ |
| Joins (inner, outer, cross, lateral) | ✓ | ✓ |
| CTE (WITH clause) | ✓ | ✓ |
| Recursive CTE | ✓ | ✓ |
| Set operations (UNION, INTERSECT, EXCEPT) | ✓ | ✓ |
| Window functions | ✓ | ✓ |
| GROUP BY / HAVING | ✓ | ✓ |
| ORDER BY / LIMIT / OFFSET | ✓ | ✓ |
| Subqueries (scalar, correlated, lateral) | ✓ | ✓ |
| VALUES clause | ✓ | ✓ |
| PIVOT / UNPIVOT | ✓ | ✓ |
| MATCH_RECOGNIZE (row-pattern recognition) | ✓ | ✓ |
| Table functions | ✓ | ✓ |
| Prepared statement (prepare / execute / free) | ✓ | ✓ |
| Query explain / plan inspection | ✓ | ✓ |
| Optimizer adaptive feedback | ✓ | ✓ |
| Statement cache | ✓ | ✓ |
| Cross-node distributed query planning | ✗ | ✓ |
| Cross-node shard-read routing | ✗ | ✓ |
| Cross-node query fragment execution | ✗ | ✓ |
| Distributed query fan-out / result merge | ✗ | ✓ |
| Distributed partial aggregation | ✗ | ✓ |

---

## Group 4 — Transactions and MGA

| Functionality | Local | Cluster |
|---|:---:|:---:|
| BEGIN / COMMIT / ROLLBACK | ✓ | ✓ |
| Savepoints (create / release / rollback-to) | ✓ | ✓ |
| Isolation levels (read committed, repeatable read, serializable) | ✓ | ✓ |
| Snapshot / MGA visibility control | ✓ | ✓ |
| Autocommit mode | ✓ | ✓ |
| Table lock / unlock (advisory) | ✓ | ✓ |
| Named lock / unlock | ✓ | ✓ |
| EXECUTE BLOCK (anonymous block) | ✓ | ✓ |
| PREPARE TRANSACTION (two-phase prepare) | ✓ | ✓ |
| Point-in-time / AS OF history query (bitemporal) | ✓ | ✓ |
| MGA checkpoint / sweep / cleanup | ✓ | ✓ |
| MGA archive-stream verification | ✓ | ✓ |
| MGA audit legal hold | ✓ | ✓ |
| MGA archive orphan recovery | ✓ | ✓ |
| Distributed transaction begin / 2PC barrier | ✗ | ✓ |
| Remote participant prepare / commit / rollback barrier | ✗ | ✓ |
| Distributed limbo participant recovery | ✗ | ✓ |
| Distributed transaction finality proof | ✗ | ✓ |
| Cluster cleanup low-water advance | ✗ | ✓ |
| Cluster MGA transaction inspect / resolve / quarantine | ✗ | ✓ |
| Cluster write admission | ✗ | ✓ |

---

## Group 5 — Multi-Model Data

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Relational tables, views, constraints | ✓ | ✓ |
| Document insert / find / update / delete | ✓ | ✓ |
| Key-value get / put / multiget / pipeline / atomic program | ✓ | ✓ |
| Key-value structured scan / stream-append | ✓ | ✓ |
| Graph traverse / optional match | ✓ | ✓ |
| Graph DML (create, merge, set, remove, delete, detach-delete) | ✓ | ✓ |
| Graph query via graph query language (nosql bridge) | ✓ | ✓ |
| Vector ANN search | ✓ | ✓ |
| Vector hybrid search | ✓ | ✓ |
| Vector similarity expression | ✓ | ✓ |
| Vector index load / release | ✓ | ✓ |
| Vector collection operations (nosql bridge) | ✓ | ✓ |
| Time-series append / structured time-series | ✓ | ✓ |
| Full-text scoring (relevance, phrase, multi-field) | ✓ | ✓ |
| Full-text regex / wildcard / prefix match | ✓ | ✓ |
| Full-text analyzer application | ✓ | ✓ |
| Full-text / search-engine query (nosql bridge) | ✓ | ✓ |
| Columnar-style analytical query (via compatible dialect) | ✓ | ✓ |
| Bitemporal / versioned-history query | ✓ | ✓ |

---

## Group 6 — Procedural SQL

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Procedural blocks (anonymous and named routines) | ✓ | ✓ |
| Control flow (IF, LOOP, WHILE, FOR, CASE, EXIT) | ✓ | ✓ |
| Cursors (open / fetch / close) | ✓ | ✓ |
| Exception handling (SIGNAL / RAISE / RESIGNAL) | ✓ | ✓ |
| Procedure invocation | ✓ | ✓ |
| Function invocation (including UDF/UDR) | ✓ | ✓ |
| Aggregate function invocation | ✓ | ✓ |
| Trigger (DML trigger execution) | ✓ | ✓ |
| Event trigger (DDL event execution) | ✓ | ✓ |
| Domain operations and validation | ✓ | ✓ |
| Sequence NEXTVAL / CURRVAL / SETVAL | ✓ | ✓ |

---

## Group 7 — Security and Identity

| Functionality | Local | Cluster |
|---|:---:|:---:|
| User create / alter / drop | ✓ | ✓ |
| Role create / alter / drop | ✓ | ✓ |
| Group mapping create / drop | ✓ | ✓ |
| GRANT / REVOKE privileges | ✓ | ✓ |
| Session role switch (SET ROLE) | ✓ | ✓ |
| Authentication (session open / credential validation) | ✓ | ✓ |
| Identity provider management | ✓ | ✓ |
| Principal create / alter | ✓ | ✓ |
| Deep security enforcement evaluation | ✓ | ✓ |
| Object visibility evaluation | ✓ | ✓ |
| Encryption key admit / rotate | ✓ | ✓ |
| Encrypted filespace open | ✓ | ✓ |
| Protected material create / version / resolve / release | ✓ | ✓ |
| Protected material package export / import | ✓ | ✓ |
| Audit event emission | ✓ | ✓ |
| Audit log inspection (SHOW AUDIT) | ✓ | ✓ |
| Sandboxed trust separation (security context enforcement) | ✓ | ✓ |
| Cluster epoch validation / fence token lifecycle | ✗ | ✓ |
| Cluster policy version validation | ✗ | ✓ |
| Cluster provider handshake admission | ✗ | ✓ |
| Cluster route authority validation | ✗ | ✓ |

---

## Group 8 — Policy, Mask, and Row-Level Security Lifecycle

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Security policy create / alter / drop | ✓ | ✓ |
| Policy attach / activate / deactivate | ✓ | ✓ |
| Policy validate / simulate | ✓ | ✓ |
| Policy show / inspect | ✓ | ✓ |
| Row-level security inspection (SHOW RLS) | ✓ | ✓ |
| Column mask inspection (SHOW MASKS) | ✓ | ✓ |
| Discovery rights inspection | ✓ | ✓ |
| Object visibility inspection | ✓ | ✓ |
| Policy recommendation (via agent) | ✓ | ✓ |

---

## Group 9 — Backup, Restore, and Data Movement

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Logical backup (start / finish) | ✓ | ✓ |
| Logical backup restore | ✓ | ✓ |
| Delta-stream package / apply | ✓ | ✓ |
| Archive export / verify | ✓ | ✓ |
| COPY import / export stream | ✓ | ✓ |
| Change data capture (CDC start / read / apply) | ✓ | ✓ |
| Migration (begin from reference / alter / show) | ✓ | ✓ |
| Bridge cutover (migration cutover operation) | ✓ | ✓ |
| Bridge compare / validate (migration validation) | ✓ | ✓ |
| Catalog artifact export / import | ✓ | ✓ |
| External Git catalog snapshot export / diff / rollback plan | ✓ | ✓ |
| Point-in-time recovery management (via agent) | ✓ | ✓ |
| Cluster replication consumer subscribe / resume / pause / cancel | ✗ | ✓ |
| Cluster CDC receive / acknowledge | ✗ | ✓ |
| Cluster two-phase replication (prewrite / commit / cleanup / lock) | ✗ | ✓ |
| Cluster replication inspection | ✗ | ✓ |
| Cluster reconciliation (branch ledger / merge policy / conflict) | ✗ | ✓ |

---

## Group 10 — Storage and Filespaces

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Filespace create / preallocate / attach / detach | ✓ | ✓ |
| Filespace move / merge / promote / release / drop | ✓ | ✓ |
| Filespace verify / compact / fence | ✓ | ✓ |
| Filespace archive | ✓ | ✓ |
| Filespace quarantine / repair / rebuild / salvage | ✓ | ✓ |
| Filespace physical delete | ✓ | ✓ |
| Filespace snapshot (create / refresh / validate / retire) | ✓ | ✓ |
| Filespace shadow (create / refresh / validate / promote) | ✓ | ✓ |
| Filespace truncate | ✓ | ✓ |
| Filespace discovery (scan, orphan scan, stale scan) | ✓ | ✓ |
| Filespace package (export manifest / inspect / admit / reject) | ✓ | ✓ |
| Hot/cold storage tier (inspect / plan / stage / commit / rollback migration) | ✓ | ✓ |
| Index rebuild / rebalance / verify / validate / repair | ✓ | ✓ |
| Index statistics gather / MGA version cleanup | ✓ | ✓ |
| Shard placement (create / verify / move / split / merge / rebalance / archive) | ✓ | ✓ |

---

## Group 11 — Acceleration

| Functionality | Local | Cluster |
|---|:---:|:---:|
| SBLR bytecode interpreter | ✓ | ✓ |
| Superinstruction / batch fusion | ✓ | ✓ |
| LLVM JIT policy set / compile / inspect / invalidate | ✓ | ✓ |
| LLVM AOT rebuild / artifact management | ✓ | ✓ |
| GPU kernel compile / policy set / inspect / invalidate | ✓ | ✓ |
| GPU/SIMD vector scoring kernels | ✓ | ✓ |
| GPU device / artifact / kernel management | ✓ | ✓ |
| UDR package register / load / unload / invoke | ✓ | ✓ |
| LLVM module compile (extensibility) | ✓ | ✓ |

---

## Group 12 — Autonomous Agents

| Functionality | Local | Cluster |
|---|:---:|:---:|
| **Node-scope agents (deployment: local or both — run on all nodes)** | | |
| Node resource observer | ✓ | ✓ |
| Metrics registry manager | ✓ | ✓ |
| Storage health manager | ✓ | ✓ |
| Filespace capacity manager | ✓ | ✓ |
| Page allocation manager | ✓ | ✓ |
| Memory governor | ✓ | ✓ |
| Index health manager | ✓ | ✓ |
| Admission control manager | ✓ | ✓ |
| Parser interface manager | ✓ | ✓ |
| Transaction pressure manager | ✓ | ✓ |
| Storage version cleanup agent | ✓ | ✓ |
| Cleanup archive manager | ✓ | ✓ |
| Policy recommendation manager | ✓ | ✓ |
| Runtime learning agent | ✓ | ✓ |
| Support bundle triage agent | ✓ | ✓ |
| Job control manager | ✓ | ✓ |
| Backup manager | ✓ | ✓ |
| Archive manager | ✓ | ✓ |
| Restore drill manager | ✓ | ✓ |
| Point-in-time recovery manager | ✓ | ✓ |
| Identity manager | ✓ | ✓ |
| Session control manager | ✓ | ✓ |
| Alert manager | ✓ | ✓ |
| Export adapter manager | ✓ | ✓ |
| **Cluster-scope agents (deployment: cluster)** | | |
| Cluster autoscale manager | ✗ | ✓ |
| Distributed query metrics agent | ✗ | ✓ |
| Remote query routing agent | ✗ | ✓ |
| Cluster scheduler manager | ✗ | ✓ |
| Cluster upgrade manager | ✗ | ✓ |

---

## Group 13 — Observability and Diagnostics

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Metrics read / reset | ✓ | ✓ |
| Diagnostic emit / reset | ✓ | ✓ |
| EXPLAIN operation | ✓ | ✓ |
| SHOW VERSION / DATABASE / SYSTEM / CATALOG | ✓ | ✓ |
| SHOW SESSIONS / TRANSACTIONS / LOCKS / STATEMENTS | ✓ | ✓ |
| SHOW JOBS / MANAGEMENT / DIAGNOSTICS | ✓ | ✓ |
| SHOW ARCHIVE REPLICATION / FILESPACE / ACCELERATION | ✓ | ✓ |
| SHOW AGENTS / DECISION SERVICE / METRICS | ✓ | ✓ |
| SHOW BUFFER POOL / CACHE / IO / PERFORMANCE / WAIT EVENTS | ✓ | ✓ |
| SHOW INDEX HEALTH / QUERY STORE / STATEMENT CACHE | ✓ | ✓ |
| SHOW GRANTS / ROLES / USERS / GROUPS / POLICIES / RLS / MASKS | ✓ | ✓ |
| SHOW SECURITY EVENTS / SECURITY PROFILES / AUDIT | ✓ | ✓ |
| SHOW IDENTITY PROVIDERS / OBJECT VISIBILITY | ✓ | ✓ |
| SHOW GPU / GPU DEVICES / GPU KERNELS / GPU MEMORY / GPU ARTIFACTS | ✓ | ✓ |
| SHOW LLVM / LLVM TARGETS / LLVM PROVENANCE / NATIVE COMPILE | ✓ | ✓ |
| SHOW AOT ARTIFACTS / NATIVE COMPILE CACHE | ✓ | ✓ |
| SHOW CAPABILITIES / CONTEXT / DIALECT / SCHEMA PATH / SEARCH PATH | ✓ | ✓ |
| Support bundle creation (prepare / show safety / collect) | ✓ | ✓ |
| sys.information catalog projections | ✓ | ✓ |
| Health / readiness inspection | ✓ | ✓ |
| Message vector (diagnostic envelope) | ✓ | ✓ |
| Cluster state / topology / members / capabilities inspection | ✗ | ✓ |
| Cluster routing plan inspection | ✗ | ✓ |
| Cluster shards / placement / archive / replication inspection | ✗ | ✓ |
| Cluster SLO / error budget / alerts / decisions inspection | ✗ | ✓ |
| Cluster limbo / recovery / admission status inspection | ✗ | ✓ |
| Cluster metrics snapshot / route trace / event emit | ✗ | ✓ |
| Cluster GPU placement inspection | ✗ | ✓ |
| Cluster provider inspection (ABI / handshake status) | ✓ | ✓ |

---

## Group 14 — Operating Modes and Transport

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Embedded engine (library/in-process) | ✓ | ✓ |
| Single-node IPC server (shared-memory transport) | ✓ | ✓ |
| Standalone network listener server | ✓ | ✓ |
| Managed group (coordinated local node set) | ✓ | ✓ |
| Database lifecycle (create / open / attach / detach / shutdown / drop) | ✓ | ✓ |
| Maintenance mode (enter / exit) | ✓ | ✓ |
| Restricted-open mode (enter / exit) | ✓ | ✓ |
| Session management (open / close / settings / discard / snapshot handle) | ✓ | ✓ |
| Listener drain / undrain | ✓ | ✓ |
| Manager restart / start / stop | ✓ | ✓ |
| Parser pool resize | ✓ | ✓ |
| Configuration inspect / set / reset / reload | ✓ | ✓ |
| Job scheduler (create / alter / run / pause / resume / cancel) | ✓ | ✓ |
| Event channel (create / listen / notify / unlisten / poll / ack) | ✓ | ✓ |
| Memory governor controls (profiles / cache / scavenge / grants) | ✓ | ✓ |
| Universal bridge ABI (connect / auth / execute / cursor / stream / CDC) | ✓ | ✓ |
| Bridge proxy routing | ✓ | ✓ |
| Cluster join / leave | ✗ | ✓ |
| Cluster route request / publish | ✗ | ✓ |
| Cluster node fence | ✗ | ✓ |
| Cluster reconcile branch | ✗ | ✓ |
| Cluster epoch publish | ✗ | ✓ |
| Cluster control (stop / start / alter topology) | ✗ | ✓ |

---

## Group 15 — Parser and Compatibility

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Native SBsql parser | ✓ | ✓ |
| Relational dialect compatibility (multiple SQL generations) | ✓ | ✓ |
| Analytical / columnar dialect compatibility | ✓ | ✓ |
| Document store protocol compatibility | ✓ | ✓ |
| Key-value protocol compatibility | ✓ | ✓ |
| Graph query language compatibility | ✓ | ✓ |
| Search / analytics protocol compatibility | ✓ | ✓ |
| Time-series protocol compatibility | ✓ | ✓ |
| Vector / ANN protocol compatibility | ✓ | ✓ |
| Wide-column / distributed-SQL dialect compatibility | ✓ | ✓ |
| Language profiles / locale resource packs | ✓ | ✓ |
| Parser package registration (extensible parser ABI) | ✓ | ✓ |
| Capability-reference dialect conformance tracking | ✓ | ✓ |
| Wire protocol session (bridge-universal-ABI) | ✓ | ✓ |

---

## Group 16 — AI / MCP Integration

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Native MCP tool surface (query / DML / schema / admin) | ✓ | ✓ |
| MCP authentication (remote and local) | ✓ | ✓ |
| MCP governance, quotas, and audit | ✓ | ✓ |
| AI integration architecture adapter / bridge | ✓ | ✓ |
| AI integration runtime configuration | ✓ | ✓ |
| AI integration trust and authority model | ✓ | ✓ |

---

## Group 17 — Cluster Operations (Additive: Local ✗ / Cluster ✓ Only)

These operations are available exclusively with the commercial cluster provider. None of them are available in Local deployments; the cluster tier makes them available without removing any Local capability.

### Topology and Membership

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Cluster state / topology inspect | ✗ | ✓ |
| Define region / shard profile | ✗ | ✓ |
| Publish topology manifest | ✗ | ✓ |
| Validate topology schema version | ✗ | ✓ |
| Inspect filespace shards | ✗ | ✓ |
| Admit / remove / drain member node | ✗ | ✓ |
| Set node role | ✗ | ✓ |
| Inspect node health | ✗ | ✓ |
| Validate node role suitability | ✗ | ✓ |
| Cluster join / leave | ✗ | ✓ |
| Cluster node fence | ✗ | ✓ |
| Cluster epoch publish | ✗ | ✓ |

### Routing and Placement

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Publish / reject stale route owner | ✗ | ✓ |
| Inspect routing plan | ✗ | ✓ |
| Place object | ✗ | ✓ |
| Rebalance shards | ✗ | ✓ |
| Validate partition distribution | ✗ | ✓ |
| Assign tablet range | ✗ | ✓ |
| Cluster route request / publish | ✗ | ✓ |
| Cluster placement move / admission tune | ✗ | ✓ |
| Cluster placement policy (create / alter / drop) | ✗ | ✓ |
| Declare region / availability zone / data placement | ✗ | ✓ |

### Distributed Transactions

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Begin distributed transaction | ✗ | ✓ |
| Remote participant prepare | ✗ | ✓ |
| Publish commit barrier | ✗ | ✓ |
| Publish rollback barrier | ✗ | ✓ |
| Recover limbo participant | ✗ | ✓ |
| Advance cleanup low-water mark | ✗ | ✓ |
| Validate finality proof | ✗ | ✓ |
| Cluster write admission | ✗ | ✓ |
| Route fence validation (insert path) | ✗ | ✓ |
| Distributed MGA transaction inspect / resolve / quarantine | ✗ | ✓ |
| MGA transaction retry decision | ✗ | ✓ |

### Cluster Replication and Reconciliation

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Replication consumer subscribe / resume / pause / cancel | ✗ | ✓ |
| CDC receive / acknowledge | ✗ | ✓ |
| Two-phase replication (prewrite / commit / cleanup) | ✗ | ✓ |
| Two-phase pessimistic lock / rollback / heartbeat / status | ✗ | ✓ |
| Cluster replication inspect | ✗ | ✓ |
| Reconcile branch ledger | ✗ | ✓ |
| Apply merge policy | ✗ | ✓ |
| Report reconciliation conflict | ✗ | ✓ |
| Classify non-mergeable data | ✗ | ✓ |
| Publish reconciled finality | ✗ | ✓ |
| Cluster reconcile branch (MGA) | ✗ | ✓ |

### Cluster Security

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Validate cluster epoch | ✗ | ✓ |
| Issue / revoke fence token | ✗ | ✓ |
| Validate policy version | ✗ | ✓ |
| Cluster provider handshake admission | ✗ | ✓ |
| Cluster route authority validation | ✗ | ✓ |
| Cluster agent list / get / control | ✗ | ✓ |
| Cluster sys.agents projection | ✗ | ✓ |

### Distributed Query Execution

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Plan distributed query | ✗ | ✓ |
| Admit cross-node query | ✗ | ✓ |
| Route shard read | ✗ | ✓ |
| Execute query fragment | ✗ | ✓ |
| Fan-out search (distributed full-text / vector) | ✗ | ✓ |
| Merge distributed results | ✗ | ✓ |
| Aggregate partial results | ✗ | ✓ |
| Validate safe read | ✗ | ✓ |
| Remote optimizer operator | ✗ | ✓ |
| Cluster bridge route | ✗ | ✓ |
| Cluster bridge distributed / cross-node query | ✗ | ✓ |

### Cluster Administration and Metrics

| Functionality | Local | Cluster |
|---|:---:|:---:|
| Cluster admin inspect status | ✗ | ✓ |
| Cluster admin run maintenance | ✗ | ✓ |
| Cluster config | ✗ | ✓ |
| Cluster recovery resolution | ✗ | ✓ |
| Cluster metrics snapshot / trace route / emit event | ✗ | ✓ |
| Cluster support bundle collect | ✗ | ✓ |
| Cluster job start / cancel / throttle | ✗ | ✓ |
| Cluster-scope agents (autoscale / scheduler / query metrics / routing / upgrade) | ✗ | ✓ |

---

## Closing Note

The presence of a ✓ in this matrix indicates that the underlying SBLR opcode or agent manifest entry is in the `implemented` or `both`/`local` deployment state. Actual availability of any specific item still depends on build configuration, enabled feature flags, licensing, release status, and operational policy. Some capabilities (for example, LLVM JIT, GPU acceleration) require optional build dependencies. Cluster-tier items require a commercially licensed cluster provider that passes the ABI handshake.

For deeper detail, consult the relevant manuals:

- **CDE Concepts**: [CDE_Concepts/README.md](CDE_Concepts/README.md)
- **Agent Runtime Guide**: [Agent_Runtime_Guide/README.md](Agent_Runtime_Guide/README.md)
- **Operations and Administration**: [Operations_Administration/README.md](Operations_Administration/README.md)
- **Cluster-Gated Statements**: [Language_Reference/syntax_reference/cluster_gated_statements.md](Language_Reference/syntax_reference/cluster_gated_statements.md)
- **Security Guide**: [Security_Guide/README.md](Security_Guide/README.md)
- **Acceleration Guide**: [Acceleration_Guide/README.md](Acceleration_Guide/README.md)
- **AI Integration Guide**: [AI_Integration_Guide/README.md](AI_Integration_Guide/README.md)
- **Language Support**: [Language_Support/README.md](Language_Support/README.md)
