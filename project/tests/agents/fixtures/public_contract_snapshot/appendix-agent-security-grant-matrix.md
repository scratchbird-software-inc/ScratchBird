# Agent Security Grant Matrix

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.
-->

Status: accepted controlling private specification.
Search key: `AGH_006_AGENT_SECURITY_GRANT_MATRIX`.

## Rights

| right | allows | does_not_allow |
| --- | --- | --- |
| OBS_AGENT_STATE_READ | read permitted agent state, lifecycle, health, policy UUID, and recommendation counts | evidence payloads, control, raw restricted metrics |
| OBS_AGENT_EVIDENCE_READ | read permitted evidence rows and audit rows after redaction | control, raw secrets, unredacted support bundles |
| OBS_AGENT_RECOMMENDATION_READ | read pending recommendations/actions visible to caller | approve, cancel, execute, or suppress actions |
| OBS_AGENT_CONTROL | start, stop, drain, restart, enable, disable, quarantine allowed agents | approve high-risk actions, edit policy, bypass leases |
| OBS_AGENT_ACTION_APPROVE | approve pending action within granted scope | grant actuator permission or skip safety checks |
| OBS_AGENT_ACTION_CANCEL | cancel pending/running cancellable action | rollback completed irreversible action |
| OBS_AGENT_OVERRIDE | create/update/drop bounded override or suppression | permanent bypass, correctness override, or hidden suppression |
| OBS_SUPPORT_BUNDLE_READ | read support-bundle-safe agent evidence | read unredacted secret/security payloads |
| OBS_POLICY_READ | read policy identity, version, attachment, validation state | apply, edit, delete, or read restricted policy body |
| OBS_POLICY_SIMULATE | simulate policy against visible/redacted metrics | activate policy or invoke actuator |
| OBS_POLICY_EDIT_DRAFT | create or edit draft policy objects | approve or apply policy |
| OBS_POLICY_VALIDATE | validate draft policy and receive diagnostics | activate policy |
| OBS_POLICY_APPROVE | approve a validated policy version | apply policy without OBS_POLICY_APPLY |
| OBS_POLICY_APPLY | attach/apply approved policy | bypass validation/evidence |
| OBS_POLICY_ROLLBACK | roll back to prior valid policy version | roll back to invalid/unapproved version |
| OBS_POLICY_DELETE | delete unattached draft/retired policy versions | delete active required baseline without replacement |
| OBS_CLUSTER_HEALTH_INSPECT | read cluster health/agent summary | topology details or control |
| OBS_CLUSTER_TOPOLOGY_INSPECT | read cluster agent node/topology placement | control or membership mutation |
| OBS_CLUSTER_CONTROL | control cluster agents explicitly granted to caller | bypass decision service, lease, fence, or epoch checks |
| SEC_AUTH_METRICS_READ | read security-sensitive auth/session metrics | mutate identity, grants, or sessions |
| SEC_REDACTION_POLICY_EDIT | edit redaction policy objects | bypass redaction or support-bundle policy |
| SEC_EXPORT_POLICY_APPROVE | approve export policies and residency gates | bypass export adapter evidence |

## Default groups

| group | default agent access |
| --- | --- |
| PUBLIC | no agent rights. |
| APP | own session-facing agent state only through self-filtered views. |
| DEV | dev-scope diagnostics only; granting index/parser/runtime diagnostic rights must emit explicit warning evidence. |
| ANL | no operational agent rights by default. |
| ETL | assigned job and scheduler recommendations for owned/assigned jobs. |
| SCH | assigned job and scheduler recommendations/control for scheduled jobs where job rights allow. |
| DBA | database agent state, recommendations, evidence, policy simulation, and database-scope action approval. |
| AUD | read-only evidence, policy history, action history, and redacted support evidence. |
| SUP | read-only operational state, recommendations, management inspect, and support-bundle-safe evidence. |
| OPS | operational control, action approval, export control, overrides, and cluster control within policy. |
| SEC | identity, security, redaction, auth/session, and export-policy evidence/control; no generic platform control unless dual-hatted. |
| ROOT | break-glass only with expiration, evidence, and review. |

## Command to right mapping

| command family | minimum rights |
| --- | --- |
| SHOW AGENTS / SHOW AGENT | OBS_AGENT_STATE_READ |
| SHOW AGENT METRICS | OBS_AGENT_STATE_READ plus metric-family read right |
| SHOW AGENT POLICY | OBS_POLICY_READ |
| SHOW AGENT EVIDENCE / AUDIT | OBS_AGENT_EVIDENCE_READ |
| ALTER AGENT lifecycle | OBS_AGENT_CONTROL |
| ALTER AGENT POLICY VALIDATE/SIMULATE | OBS_POLICY_VALIDATE or OBS_POLICY_SIMULATE |
| ALTER AGENT POLICY ATTACH/DETACH/APPLY/ROLLBACK | OBS_AGENT_CONTROL plus relevant OBS_POLICY_* right |
| ALTER AGENT ACTION APPROVE/CANCEL/RETRY/SUPPRESS | OBS_AGENT_ACTION_APPROVE, OBS_AGENT_ACTION_CANCEL, or OBS_AGENT_OVERRIDE |
| CREATE/ALTER/DROP AGENT OVERRIDE | OBS_AGENT_OVERRIDE |
| SHOW/ALTER CLUSTER AGENT | OBS_CLUSTER_HEALTH_INSPECT or OBS_CLUSTER_CONTROL plus agent-specific right |

## Security examples

Denied: an `APP` user attempts `SHOW AGENT ACTIONS` for `storage_health_manager`. The request fails with `AGENT.PERMISSION_DENIED` and returns no hidden candidate action rows.

Allowed: an `OPS` user with `OBS_AGENT_CONTROL` drains `parser_interface_manager`; `agent_transition_evidence` is written before the command returns success.
