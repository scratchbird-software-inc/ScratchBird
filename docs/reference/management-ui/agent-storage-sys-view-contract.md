# Agent and Storage `sys.*` View Contract

This management-UI reference projects the manifest-listed Core contract
`Specifications/Core/chapters/agents/appendix-agent-show-and-sys-surfaces.md`
(search keys `AGH_005_AGENT_SHOW_SYS_SURFACES` and
`AGH_FILESPACE_PAGE_SHOW_SYS_SURFACES`). Core remains authoritative if this
reference differs from the specification.

These views are projections only over engine-owned agent, policy, action,
evidence, metric, filespace, and page-state records. They do not create an
independent catalog or action path. Every read uses MGA snapshot visibility;
authorization and redaction are engine-owned. A client, parser, driver, or UI
has no storage authority, transaction authority, or finality authority.

UUID columns are database-generated or resolver-sourced identities. They may
be redacted when the caller lacks the exact object, scope, evidence, audit, or
storage visibility right. Nullable means the authoritative record can omit the
value; it does not permit a client to synthesize one.

## `sys.agents`

Requires `OBS_AGENT_STATE_READ`. Durable health, queue, failure, retry, and
decision fields are engine-owned runtime rollups; diagnostic and evidence
fields remain subject to evidence visibility.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `agent_uuid` | `uuid` | NOT NULL | Durable agent instance identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `agent_type_id` | `text` | NOT NULL | Canonical agent type identifier. | Authorized projection; engine-owned redaction applies. |
| `scope_kind` | `text` | NOT NULL | Kind of scope governed by the instance. | Authorized projection; engine-owned redaction applies. |
| `scope_uuid` | `uuid` | NULL | Durable governed-scope identity. | Resolver-sourced database-generated UUID; redacted when scope visibility is absent. |
| `component` | `text` | NOT NULL | Owning engine component. | Authorized projection; engine-owned redaction applies. |
| `state` | `text` | NOT NULL | Agent lifecycle state. | Authorized projection; engine-owned redaction applies. |
| `health_state` | `text` | NOT NULL | Current durable health classification. | Authorized projection; engine-owned redaction applies. |
| `enabled` | `yes_no` | NOT NULL | Whether policy permits the agent to run. | Authorized projection; engine-owned redaction applies. |
| `policy_uuid` | `uuid` | NULL | Attached policy identity. | Resolver-sourced database-generated UUID; redacted when policy visibility is absent. |
| `last_transition_at` | `text` | NULL | Time of the latest durable lifecycle transition. | Authorized projection; engine-owned redaction applies. |
| `last_diagnostic_code` | `text` | NULL | Latest canonical diagnostic code. | Redacted without agent-evidence visibility. |
| `last_evidence_uuid` | `uuid` | NULL | Latest durable health or transition evidence identity. | Resolver-sourced database-generated UUID; redacted when evidence visibility is absent. |
| `policy_generation` | `uint64` | NULL | Applied policy generation. | Authorized projection; engine-owned redaction applies. |
| `queue_depth` | `uint64` | NULL | Current lease and action queue pressure. | Authorized aggregate only; protected queue entries are not exposed. |
| `action_backlog` | `uint64` | NULL | Pending and running action count. | Authorized aggregate only; action bodies remain redacted. |
| `failure_count` | `uint64` | NULL | Combined crash and supervision failure count. | Authorized aggregate; diagnostic detail remains redacted. |
| `quarantine_count` | `uint64` | NULL | Quarantined action count. | Authorized aggregate only. |
| `retry_not_before` | `text` | NULL | Latest cooldown or restart boundary. | Authorized projection; engine-owned redaction applies. |
| `last_decision` | `text` | NULL | Latest durable action and state summary. | Redacted when action visibility is absent. |
| `overhead_budget_units` | `uint64` | NULL | Engine-computed management overhead pressure. | Authorized aggregate only. |
| `diagnostic_redaction_state` | `text` | NULL | Whether the diagnostic is visible or redacted. | Always reflects engine-owned redaction outcome. |

```sql
SELECT agent_type_id, state, health_state FROM sys.agents WHERE state = 'running';
```

Key filters: `agent_type_id`, `scope_kind`, `state`, `health_state`, and
`component` support local management inspection.

## `sys.agent_metric_dependencies`

Requires `OBS_AGENT_STATE_READ`; raw freshness values additionally require the
applicable metric-family read right.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `agent_uuid` | `uuid` | NOT NULL | Agent instance that consumes the metric. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `metric_family` | `text` | NOT NULL | Canonical metric family. | Authorized projection; engine-owned redaction applies. |
| `namespace` | `text` | NOT NULL | Engine metric namespace. | Authorized projection; engine-owned redaction applies. |
| `required_or_optional` | `text` | NOT NULL | Dependency requirement class. | Authorized projection; engine-owned redaction applies. |
| `freshness_limit` | `text` | NOT NULL | Maximum admitted sample age. | Authorized projection; engine-owned redaction applies. |
| `current_freshness` | `text` | NULL | Age or freshness state of the current sample. | Redacted without the metric-family read right. |
| `quality_state` | `text` | NOT NULL | Trusted, stale, missing, or degraded quality state. | Authorized projection; engine-owned redaction applies. |
| `fail_behavior` | `text` | NOT NULL | Required fail-closed behavior. | Authorized projection; engine-owned redaction applies. |

```sql
SELECT agent_uuid, metric_family, quality_state FROM sys.agent_metric_dependencies WHERE quality_state = 'stale';
```

Key filters: `agent_uuid`, `metric_family`, and `quality_state` identify stale
or missing dependencies without exposing raw metric values.

## `sys.agent_policies`

Requires `OBS_POLICY_READ`. Policy bodies are never columns of this view.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `agent_uuid` | `uuid` | NOT NULL | Agent instance receiving the policy. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `policy_uuid` | `uuid` | NOT NULL | Durable policy identity. | Resolver-sourced database-generated UUID; redacted when policy visibility is absent. |
| `policy_family` | `text` | NOT NULL | Canonical policy family. | Authorized projection; engine-owned redaction applies. |
| `version_uuid` | `uuid` | NULL | Attached policy-version identity. | Resolver-sourced database-generated UUID; redacted when version visibility is absent. |
| `active_state` | `text` | NOT NULL | Attachment activation state. | Authorized projection; engine-owned redaction applies. |
| `validation_state` | `text` | NOT NULL | Engine validation outcome. | Authorized projection; engine-owned redaction applies. |
| `attached_at` | `text` | NULL | Durable attachment time. | Authorized projection; engine-owned redaction applies. |
| `attached_by` | `text` | NULL | Redacted actor summary for the attachment. | Actor detail is redacted without audit visibility. |

```sql
SELECT agent_uuid, policy_family, validation_state FROM sys.agent_policies WHERE active_state = 'active';
```

Key filters: `agent_uuid`, `policy_family`, and `validation_state` support
attachment and validation inspection.

## `sys.agent_actions`

Requires `OBS_AGENT_RECOMMENDATION_READ`. Reading a row does not approve,
cancel, retry, suppress, or execute the action.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `action_uuid` | `uuid` | NOT NULL | Durable action identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `agent_uuid` | `uuid` | NOT NULL | Recommending agent identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `action_id` | `text` | NOT NULL | Canonical action operation identifier. | Authorized projection; engine-owned redaction applies. |
| `state` | `text` | NOT NULL | Pending, running, completed, refused, or quarantined state. | Authorized projection; engine-owned redaction applies. |
| `risk_class` | `text` | NOT NULL | Engine policy risk classification. | Authorized projection; engine-owned redaction applies. |
| `created_at` | `text` | NOT NULL | Durable creation time. | Authorized projection; engine-owned redaction applies. |
| `expires_at` | `text` | NULL | Expiration boundary. | Authorized projection; engine-owned redaction applies. |
| `approval_required` | `yes_no` | NOT NULL | Whether explicit approval is required. | Authorized projection; engine-owned redaction applies. |
| `actor_uuid` | `uuid` | NULL | Actor associated with the action state. | Resolver-sourced database-generated UUID; redacted without audit visibility. |
| `diagnostic_code` | `text` | NULL | Canonical refusal or outcome diagnostic. | Redacted without agent-evidence visibility. |

```sql
SELECT action_uuid, action_id, state FROM sys.agent_actions WHERE state = 'pending';
```

Key filters: `action_id`, `state`, `risk_class`, and `agent_uuid` support action
review before a separate authorized command.

## `sys.agent_overrides`

Requires `OBS_AGENT_STATE_READ`. Human reason text is omitted; only the
canonical reason code is projected.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `override_uuid` | `uuid` | NOT NULL | Durable override identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `target_uuid` | `uuid` | NOT NULL | Durable override target identity. | Resolver-sourced database-generated UUID; redacted when target visibility is absent. |
| `scope_uuid` | `uuid` | NULL | Scope identity for the override. | Resolver-sourced database-generated UUID; redacted when scope visibility is absent. |
| `suppression_class` | `text` | NOT NULL | Suppressed action or recommendation class. | Authorized projection; engine-owned redaction applies. |
| `starts_at` | `text` | NOT NULL | Override activation boundary. | Authorized projection; engine-owned redaction applies. |
| `expires_at` | `text` | NULL | Override expiration boundary. | Authorized projection; engine-owned redaction applies. |
| `state` | `text` | NOT NULL | Current override state. | Authorized projection; engine-owned redaction applies. |
| `reason_code` | `text` | NULL | Canonical redacted reason classification. | Raw reason text is omitted. |
| `created_by` | `text` | NULL | Redacted actor summary. | Actor detail is redacted without audit visibility. |

```sql
SELECT override_uuid, target_uuid, state FROM sys.agent_overrides WHERE state = 'active';
```

Key filters: `state`, `target_uuid`, and `scope_uuid` support active override
inspection.

## `sys.agent_evidence`

Requires `OBS_AGENT_EVIDENCE_READ`. Payload bodies are never published by this
projection; the digest and redaction marker describe the retained evidence.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `evidence_uuid` | `uuid` | NOT NULL | Durable evidence identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `agent_uuid` | `uuid` | NOT NULL | Producing agent identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `evidence_type` | `text` | NOT NULL | Canonical evidence class. | Authorized projection; engine-owned redaction applies. |
| `action_uuid` | `uuid` | NULL | Related action identity. | Resolver-sourced database-generated UUID; redacted when action visibility is absent. |
| `redaction_class` | `text` | NOT NULL | Applied evidence redaction policy class. | Always reflects engine-owned redaction outcome. |
| `created_at` | `text` | NOT NULL | Durable evidence creation time. | Authorized projection; engine-owned redaction applies. |
| `actor_uuid` | `uuid` | NULL | Actor identity retained with the evidence. | Resolver-sourced database-generated UUID; redacted without actor visibility. |
| `payload_digest` | `text` | NULL | Digest of the retained evidence payload. | Visible only when the evidence class permits it. |
| `payload_redacted` | `yes_no` | NOT NULL | Whether protected payload fields were removed. | Always reflects engine-owned redaction outcome. |

```sql
SELECT evidence_uuid, agent_uuid, evidence_type FROM sys.agent_evidence WHERE evidence_type = 'agent_action_evidence';
```

Key filters: `agent_uuid`, `evidence_type`, and `action_uuid` support evidence
inventory and audit correlation.

## `sys.agent_audit`

Requires `OBS_AGENT_EVIDENCE_READ`. Audit projection cannot authorize or replay
the recorded command.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `audit_uuid` | `uuid` | NOT NULL | Durable audit-record identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `evidence_uuid` | `uuid` | NULL | Related evidence identity. | Resolver-sourced database-generated UUID; redacted when evidence visibility is absent. |
| `actor_uuid` | `uuid` | NULL | Authorized actor identity. | Resolver-sourced database-generated UUID; redacted without actor visibility. |
| `command_name` | `text` | NOT NULL | Canonical management command name. | Authorized projection; engine-owned redaction applies. |
| `sblr_operation` | `text` | NOT NULL | Canonical SBLR opcode mnemonic. | Authorized projection; no source SQL is included. |
| `api_call` | `text` | NOT NULL | Engine API route that owned execution. | Authorized projection; engine-owned redaction applies. |
| `result_state` | `text` | NOT NULL | Exact canonical result state. | Authorized projection; engine-owned redaction applies. |
| `diagnostic_code` | `text` | NULL | Exact canonical diagnostic code. | Redacted when diagnostic visibility is absent. |
| `created_at` | `text` | NOT NULL | Durable audit creation time. | Authorized projection; engine-owned redaction applies. |

```sql
SELECT audit_uuid, command_name, result_state FROM sys.agent_audit WHERE result_state = 'denied';
```

Key filters: `actor_uuid`, `command_name`, and `result_state` support command
audit without exposing parser text or protected payloads.

## `sys.filespace_capacity_agent_state`

Requires `OBS_AGENT_STATE_READ`. Physical filespace paths are not projected.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `agent_uuid` | `uuid` | NOT NULL | Filespace-capacity agent identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `filespace_uuid` | `uuid` | NOT NULL | Durable filespace identity. | Resolver-sourced database-generated UUID; redacted when storage visibility is absent. |
| `policy_uuid` | `uuid` | NULL | Applied capacity policy identity. | Resolver-sourced database-generated UUID; redacted when policy visibility is absent. |
| `mode` | `text` | NOT NULL | Current agent operating mode. | Authorized projection; engine-owned redaction applies. |
| `health_state` | `text` | NOT NULL | Current capacity-agent health. | Authorized projection; engine-owned redaction applies. |
| `last_capacity_metric_at` | `text` | NULL | Time of the latest accepted capacity sample. | Metric detail is redacted without the metric-family right. |
| `last_health_metric_at` | `text` | NULL | Time of the latest accepted health sample. | Metric detail is redacted without the metric-family right. |
| `last_recommendation_code` | `text` | NULL | Latest canonical capacity recommendation. | Authorized projection; recommendation payload remains redacted. |
| `last_refusal_code` | `text` | NULL | Latest canonical refusal code. | Redacted when diagnostic visibility is absent. |

```sql
SELECT filespace_uuid, health_state, mode FROM sys.filespace_capacity_agent_state WHERE health_state <> 'healthy';
```

Key filters: `filespace_uuid`, `health_state`, and `mode` support capacity and
health triage without conferring storage authority.

## `sys.page_allocation_agent_state`

Requires `OBS_AGENT_STATE_READ`. Blocker principal and object details are not
published without their exact visibility rights.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `agent_uuid` | `uuid` | NOT NULL | Page-allocation agent identity. | Resolver-sourced database-generated UUID; redacted when not authorized. |
| `filespace_uuid` | `uuid` | NOT NULL | Durable filespace identity. | Resolver-sourced database-generated UUID; redacted when storage visibility is absent. |
| `page_family` | `text` | NOT NULL | Canonical page family. | Authorized projection; engine-owned redaction applies. |
| `page_type` | `text` | NOT NULL | Canonical page type. | Authorized projection; engine-owned redaction applies. |
| `policy_uuid` | `uuid` | NULL | Applied allocation policy identity. | Resolver-sourced database-generated UUID; redacted when policy visibility is absent. |
| `mode` | `text` | NOT NULL | Current page-allocation mode. | Authorized projection; engine-owned redaction applies. |
| `last_scan_generation` | `uint64` | NULL | Latest accepted allocation scan generation. | Authorized aggregate; scan payload remains redacted. |
| `last_shrink_ready_state` | `text` | NULL | Latest shrink-readiness summary. | Blocker detail is redacted. |
| `last_refusal_code` | `text` | NULL | Latest canonical refusal code. | Redacted when diagnostic visibility is absent. |

```sql
SELECT filespace_uuid, page_family, page_type, mode FROM sys.page_allocation_agent_state WHERE mode = 'active';
```

Key filters: `filespace_uuid`, `page_family`, `page_type`, and `mode` support
allocation triage without conferring page or transaction authority.

## `sys.filespace_shrink_readiness`

Requires `OBS_METRICS_READ_FAMILY`. Direct detail access records
`page_shrink_ready_evidence`; blocker payload remains redacted by evidence
class.

| Column | Logical type | Nullability | Meaning | Visibility/redaction |
| --- | --- | --- | --- | --- |
| `filespace_uuid` | `uuid` | NOT NULL | Durable filespace identity. | Resolver-sourced database-generated UUID; redacted when storage visibility is absent. |
| `safe_start_byte` | `uint64` | NOT NULL | Start of the engine-proven safe range. | Authorized aggregate; physical path is never included. |
| `safe_end_byte` | `uint64` | NOT NULL | End of the engine-proven safe range. | Authorized aggregate; physical path is never included. |
| `truncate_ready_bytes` | `uint64` | NOT NULL | Bytes currently proven safe to truncate. | Authorized aggregate; does not authorize truncation. |
| `blocker_count` | `uint64` | NOT NULL | Count of active shrink blockers. | Blocker identities and payloads remain redacted. |
| `readiness_state` | `text` | NOT NULL | Ready, blocked, stale, or unknown state. | Authorized projection; engine-owned redaction applies. |
| `scan_generation` | `uint64` | NOT NULL | Engine scan generation supporting the result. | Authorized projection; engine-owned redaction applies. |
| `evidence_uuid` | `uuid` | NULL | Durable shrink-readiness evidence identity. | Resolver-sourced database-generated UUID; redacted when evidence visibility is absent. |

```sql
SELECT filespace_uuid, readiness_state, truncate_ready_bytes FROM sys.filespace_shrink_readiness WHERE readiness_state = 'blocked';
```

Key filters: `filespace_uuid` and `readiness_state` support operator review.
This view is observational and never grants storage, transaction, or finality
authority.
