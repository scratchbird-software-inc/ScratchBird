# Agent Examples and Evidence Payload Reference

This page is the management-UI and operator reference projection of the
manifest-listed Core contract
`Specifications/Core/chapters/agents/appendix-agent-examples-and-evidence-payload-pack.md`
(search key `AGH_019_AGENT_EXAMPLES_EVIDENCE_PAYLOADS`). The Core contract is
authoritative if this reference and the specification ever differ.

The examples are ScratchBird SQL (`sbsql`) inputs. SBsql parses and lowers each
command to SBLR; it does not execute the command. The engine remains the sole
authority for authentication, authorization, catalog UUID resolution, MGA
transaction state and finality, redaction, evidence persistence, and subsystem
mutation.

Values written as `<engine-generated:...>` are runtime placeholders for
engine-generated UUIDv7 identities. Scripts, drivers, documentation, and UI
clients must resolve those identities through engine catalog surfaces and must
not hard-code catalog UUID values.

## Runnable command routes

Each example is a semicolon-terminated SBsql input line.

| Purpose | SBsql command | Operation ID | SBLR opcode | Expected surface |
| --- | --- | --- | --- | --- |
| Inspect metric dependencies | `SHOW AGENT memory_governor METRICS` | `agents.metrics.get` | `SBLR_AGENT_METRICS_GET` | `sys.agent_metric_dependencies` |
| Inspect active policy | `SHOW AGENT memory_governor POLICY` | `agents.policy.get` | `SBLR_AGENT_POLICY_GET` | `sys.agent_policies` |
| Inspect evidence inventory | `SHOW AGENT memory_governor EVIDENCE` | `agents.evidence.list` | `SBLR_AGENT_EVIDENCE_LIST` | `sys.agent_evidence` |
| Inspect pending actions | `SHOW AGENT ACTIONS` | `agents.actions.list` | `SBLR_AGENT_ACTION_LIST` | `sys.agent_actions` |
| Approve an action | `ALTER AGENT ACTION action_uuid APPROVE` | `agents.action.approve` | `SBLR_AGENT_ACTION_APPROVE` | `sys.agent_actions` |
| Inspect cluster agents | `SHOW CLUSTER AGENTS` | `cluster.agent.list` | `SBLR_CLUSTER_AGENT_LIST` | cluster-provider boundary |
| Inspect filespaces | `SHOW FILESPACES` | `filespaces.show` | `SBLR_SHOW_FILESPACES` | filespace management |
| Inspect page allocation | `SHOW PAGE ALLOCATION BY FAMILY` | `pages.allocation.family.show` | `SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY` | page-allocation management |

```sbsql
SHOW AGENT memory_governor METRICS;
SHOW AGENT memory_governor POLICY;
SHOW AGENT memory_governor EVIDENCE;
SHOW AGENT ACTIONS;
SHOW CLUSTER AGENTS;
SHOW FILESPACES;
SHOW PAGE ALLOCATION BY FAMILY;
```

## Result contract

Agent command results use an explicit state: `success`, `refused`, `denied`,
`empty`, `redacted`, `unsupported`, or `operator_required`. A result exposes
only fields authorized for the caller.

| Field | Contract |
| --- | --- |
| `operation_id` | Canonical engine operation identity. |
| `sblr_operation` | Canonical SBLR opcode identity. |
| `api_call` | Internal engine API route that owned execution. |
| `sys_surface` | End-user projection consumed by management clients. |
| `agent_uuid` | `<engine-generated:agent_uuid>` resolved by the engine. |
| `policy_uuid` | `<engine-generated:policy_uuid>` when a visible policy is associated. |
| `evidence_uuid` | `<engine-generated:evidence_uuid>` when evidence is written. |
| `result_state` | One explicit result state from the contract above. |
| `diagnostic_code` | Exact canonical diagnostic code. |
| `payload_redacted` | Whether protected payload fields were removed before publication. |

### Policy inspection with no visible policy

```json
{
  "operation_id": "agents.policy.get",
  "sblr_operation": "SBLR_AGENT_POLICY_GET",
  "api_call": "sb_api_agent_policy_get",
  "sys_surface": "sys.agent_policies",
  "agent_uuid": "<engine-generated:agent_uuid>",
  "policy_uuid": "<engine-generated:policy_uuid>",
  "result_state": "refused",
  "diagnostic_code": "POLICY.NOT_FOUND",
  "evidence_uuid": null,
  "payload_redacted": "YES"
}
```

### Evidence inspection with no visible evidence

```json
{
  "operation_id": "agents.evidence.list",
  "sblr_operation": "SBLR_AGENT_EVIDENCE_LIST",
  "api_call": "sb_api_agent_evidence_list",
  "sys_surface": "sys.agent_evidence",
  "agent_uuid": "<engine-generated:agent_uuid>",
  "evidence_uuid": "<engine-generated:evidence_uuid>",
  "result_state": "refused",
  "diagnostic_code": "AGENT.EVIDENCE_NOT_FOUND",
  "evidence_kind": "agent_read_evidence",
  "payload_redacted": "YES"
}
```

### Empty action list

```json
{
  "operation_id": "agents.actions.list",
  "sblr_operation": "SBLR_AGENT_ACTION_LIST",
  "api_call": "sb_api_agent_action_list",
  "sys_surface": "sys.agent_actions",
  "result_state": "empty",
  "diagnostic_code": "AGENT.NONE",
  "payload_redacted": "NO"
}
```

## Authorization refusal

Without `OBS_AGENT_ACTION_APPROVE`, this command must fail before any actuator
call:

```sbsql
ALTER AGENT ACTION action_uuid APPROVE;
```

The route remains `agents.action.approve` / `SBLR_AGENT_ACTION_APPROVE` and
returns denial evidence without approval evidence:

```json
{
  "operation_id": "agents.action.approve",
  "result_state": "denied",
  "diagnostic_code": "ACTION.PERMISSION_DENIED",
  "evidence_kind": "agent_denial_evidence",
  "action_approval_evidence_written": false
}
```

## Page and filespace requests

Page preallocation is engine-owned. The page-allocation manager requests the
operation through `agents.request_page_preallocation` /
`SBLR_AGENT_REQUEST_PAGE_PREALLOCATION`. A successful path records both
`agent_hook` evidence and `storage_executor=PreallocatePageFamilyPool`.

```json
{
  "operation_id": "agents.request_page_preallocation",
  "sblr_operation": "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION",
  "agent_uuid": "<engine-generated:agent_uuid>",
  "filespace_uuid": "<engine-generated:filespace_uuid>",
  "policy_uuid": "<engine-generated:policy_uuid>",
  "storage_executor": "PreallocatePageFamilyPool",
  "storage_execution": "completed",
  "page_preallocation_ledger_mutated": "true",
  "diagnostic_code": "SB-STORAGE-PAGE-PREALLOCATION-PREALLOCATED"
}
```

Filespace growth and preallocation remain filespace-subsystem owned. Their
routes are `agents.request_filespace_growth` /
`SBLR_AGENT_REQUEST_FILESPACE_GROWTH` and `filespace.preallocate` /
`SBLR_FILESPACE_PREALLOCATE`. Successful preallocation records
`storage_executor=PreallocateFilespace`.

## Cluster-provider boundary

`SHOW CLUSTER AGENTS` lowers to `cluster.agent.list` /
`SBLR_CLUSTER_AGENT_LIST` and crosses only the compile-time cluster-provider
boundary.

A non-cluster build returns:

```json
{
  "operation_id": "cluster.agent.list",
  "result_state": "unsupported",
  "diagnostic_code": "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"
}
```

A compile-link stub build returns:

```json
{
  "operation_id": "cluster.agent.list",
  "result_state": "unsupported",
  "diagnostic_code": "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
  "provider_name": "scratchbird.cluster.compile_link_stub_provider"
}
```

The compile-link stub proves routing only. It does not provide cluster
execution authority.

## Support-bundle evidence

Support-bundle collection exports agent evidence only after engine
authorization and after redaction. Raw paths, principal tokens, credentials,
and unredacted payload fields are not published.

```json
{
  "bundle_record_kind": "agent_runtime_evidence",
  "evidence_kind": "support_bundle_agent_runtime_evidence",
  "agent_type_id": "page_allocation_manager",
  "agent_uuid": "<engine-generated:agent_uuid>",
  "filespace_uuid": "<engine-generated:filespace_uuid>",
  "policy_uuid": "<engine-generated:policy_uuid>",
  "evidence_uuid": "<engine-generated:evidence_uuid>",
  "result_state": "success",
  "diagnostic_code": "AGENT.PAGE_PREALLOCATION.COMPLETED",
  "payload_digest": "sha256:<digest>",
  "payload_redacted": "YES",
  "physical_path": "<redacted>",
  "support_marker": "support_bundle_agent_runtime_evidence"
}
```

Clients must treat operation IDs, opcodes, diagnostics, generated identities,
result states, and redaction markers as contract fields. They must not infer
engine execution or finality from source SQL text, display names, parser state,
or client-side timestamps.
