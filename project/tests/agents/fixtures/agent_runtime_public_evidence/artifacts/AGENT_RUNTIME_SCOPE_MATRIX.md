# Agent Scope Matrix

Search keys: `AEIC_AGENT_SCOPE_MATRIX`, `AEIC_AGENT_RUNTIME_PUBLIC_SCOPE_MATRIX`

This matrix is the public runtime-scope evidence fixture for the agent gate
suite. Completion is asserted by the paired status and traceability matrices.

## Non-Cluster Agents To Fully Implement

| Agent/module | Scope | Required enterprise outcome |
| --- | --- | --- |
| `node_resource_agent` | local node resources | Real node resource snapshot, durable health/evidence, strict metrics, support-bundle redaction. |
| `metrics_registry_manager` | node/database metrics | Real metric registry validation, descriptor generation audit, no cluster metrics implementation in core. |
| `storage_health_manager` | storage health | Real storage health evaluator using storage/page metrics and durable recommendations. |
| `filespace_capacity_manager` | filespace capacity | Real capacity-window recommendations and real local actuator handoff where supported. |
| `page_allocation_manager` | page preallocation | Real page preallocation actuator, no default-live proof, durable replay/outcome verification. |
| `memory_governor` | memory/resource pressure | Real memory quota/reservation control tied to memory workplan evidence. |
| `index_health_manager` | index health | Real index health observations, rebuild recommendations, durable evidence. |
| `admission_control_manager` | workload admission | Real admission/resource route with strict metrics and durable refusal evidence. |
| `parser_interface_manager` | parser/package interface | Parser package health/control evidence only; no parser execution authority. |
| `transaction_pressure_manager` | MGA pressure | Real MGA pressure recommendations without finality/visibility authority. |
| `storage_version_cleanup_agent` | version cleanup | Real MGA-safe cleanup batches with horizon/legal-hold/finality non-authority proof. |
| `cleanup_archive_manager` | archive cleanup | Real local cleanup/archive lifecycle behavior with recovery-authority non-drift proof. |
| `policy_recommendation_manager` | policy recommendations | Durable advisory evidence with policy-generation safety. |
| `runtime_learning_agent` | optimizer/runtime learning | Durable metric-backed recommendations only; optimizer may reject unsafe evidence. |
| `support_bundle_triage_agent` | support bundle triage | Real redaction/tamper/retention triage; no protected material leakage. |
| `job_control_manager` | local job control | Real local job control routes with durable approvals and idempotency. |
| `backup_manager` | local backup | Real local backup workflow, MGA holds, durable evidence, restore compatibility proof. |
| `archive_manager` | local archive | Real local archive lifecycle and cleanup with recovery/MGA authority non-drift. |
| `restore_drill_manager` | restore drill | Real local restore drill route with no production data mutation authority drift. |
| `pitr_manager` | local PITR | Real local PITR evidence and exact refusal where unsupported. |
| `identity_manager` | local identity/security operations | Real local identity management routes with security-negative tests. |
| `session_control_manager` | local session control | Real session control routes with durable audit and cancellation/race tests. |
| `alert_manager` | alert lifecycle | Real alert emission/suppression lifecycle with durable evidence. |
| `export_adapter_manager` | local export | Real local export route with redaction/security checks and externalized cluster branches. |

## Compiled Noncanonical Internal Helper Modules

These modules are compiled helper implementations used by canonical agent
families. They are not canonical production agents, are absent from
`CanonicalAgentManifest()`, are absent from production exposure matrices, and
must not appear on management/sys/support-bundle production surfaces as
independent live agents.

| Module | Required disposition |
| --- | --- |
| `nosql_family_maintenance_agent` | Internal helper for NoSQL maintenance scheduling. It executes authoritative local maintenance decisions only when called by the owning NoSQL/maintenance route and is blocked from canonical production exposure. |
| `nosql_backpressure_debt_agent` | Internal helper for NoSQL/vector/search/graph backpressure debt evaluation. It is metric/evidence authority bounded and blocked from canonical production exposure. |
| `index_garbage_cleanup_agent` | Internal helper for secondary-index garbage cleanup batches under MGA cleanup-horizon authority. It is not an independent production agent surface. |
| `shadow_index_build_agent` | Internal helper for shadow-index publish lifecycle under explicit MGA authority and validation evidence. It is not an independent production agent surface. |

## Cluster-Only Agents Excluded From Core Implementation

| Agent/module | Core repository outcome | Live behavior owner |
| --- | --- | --- |
| `cluster_autoscale_manager` | disabled/fail-closed or public stub boundary only | external `sb_cluster_provider` library |
| `distributed_query_metrics_agent` | disabled/fail-closed or public stub boundary only | external `sb_cluster_provider` library |
| `remote_query_routing_agent` | disabled/fail-closed or public stub boundary only | external `sb_cluster_provider` library |
| `cluster_scheduler_manager` | disabled/fail-closed or public stub boundary only | external `sb_cluster_provider` library |
| `cluster_upgrade_manager` | disabled/fail-closed or public stub boundary only | external `sb_cluster_provider` library |

## Both-Deployment Rule

Agents declared with `AgentDeployment::both` must implement complete local
single-node behavior in this repository. Any cluster-specific branch must route
through the external cluster provider boundary and must return exact fail-closed
diagnostics in no-cluster and public-stub builds.
