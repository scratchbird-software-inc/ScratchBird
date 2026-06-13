# Agent Cross-Reference and No-Implicit-Defaults Validation

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.
-->

Status: accepted controlling private specification.
Search key: `AGH_020_AGENT_CROSS_REFERENCE_VALIDATION`.

## Closure matrix

| item class | owning appendix | closure requirement | failure diagnostic |
| --- | --- | --- | --- |
| agent_type_id | appendix-agent-canonical-registry.md | every agent has a registry row and conformance probes | AGENT.REGISTRY_INCOMPLETE |
| metric_family | appendix-agent-metric-dependency-contracts.md and metrics registry | every consumed metric names namespace, freshness, quality, fail behavior | AGENT.METRIC_DEPENDENCY_INCOMPLETE |
| policy_family | appendix-agent-policy-attachment-and-baseline-policies.md | every agent has baseline policy, validation, simulate, apply, rollback | AGENT.POLICY_DEPENDENCY_INCOMPLETE |
| right | appendix-agent-security-grant-matrix.md | every command/action/view/evidence operation references exact right | AGENT.RIGHT_UNMAPPED |
| command | appendix-agent-sbsql-management-commands.md | every command has grammar, result, SBLR, API, evidence, diagnostic | AGENT.COMMAND_INCOMPLETE |
| SBLR operation | appendix-agent-sblr-api-operation-matrix.md | every operation has envelope, binding, API call, failure rule | AGENT.SBLR_OPERATION_INCOMPLETE |
| API call | appendix-agent-sblr-api-operation-matrix.md | every API call returns canonical EngineApiResult | AGENT.API_INCOMPLETE |
| action_id | appendix-agent-action-contract-matrix.md | every action has actuator, permission, policy, metrics, evidence, failure | AGENT.ACTION_CONTRACT_INCOMPLETE |
| evidence_type | appendix-agent-evidence-audit-redaction-retention.md | every evidence type has schema, redaction, retention, support-bundle rule | AGENT.EVIDENCE_SCHEMA_INCOMPLETE |
| diagnostic_code | diagnostic registry and relevant appendix | every failure path emits exact diagnostic | AGENT.DIAGNOSTIC_UNMAPPED |
| cluster lease/fence | appendix-cluster-agent-leadership-lease-and-failover.md | every cluster action names lease, epoch, fence, failover behavior | AGENT.CLUSTER_AUTHORITY_INCOMPLETE |
| external source | appendix-agent-external-research-and-reference-map.md | every material source is summarized under docs/reference and indexed | AGENT.REFERENCE_UNMAPPED |

## Validation algorithm

1. Load the manifest-listed agent spec files.
2. Extract all generated names: agents, metrics, policies, rights, commands, SBLR operations, API calls, actions, evidence names, diagnostics, and conformance probes.
3. Verify each generated name appears in its owning appendix.
4. Verify each reference from one appendix resolves to an owning appendix row.
5. Reject text containing prohibited ambiguity tokens unless the line names an explicit profile with exact behavior.
6. Emit one diagnostic per missing or ambiguous item and fail the release gate.

## Required result

A release-complete agent implementation must pass this validation before any agent runtime code is considered conformant.
