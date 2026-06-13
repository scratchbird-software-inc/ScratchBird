# Agent Management and Operational Framework

<!--
Copyright (c) 2025-2026 Dalton Calford. All rights reserved.

TRADE SECRET / PRIVATE / CONFIDENTIAL.

This file contains private ScratchBird specification material.
No license or rights are granted to any person or entity except by specific
written permission from the author/owner.

Unauthorized use, copying, modification, distribution, disclosure, publication,
or creation of derivative works is prohibited.
-->

Status: accepted controlling private specification.
Search key: `AGH_AGENT_MANAGEMENT_OPERATIONAL_FRAMEWORK`.

## Purpose

This specification defines ScratchBird operational agents as engine-owned control-loop actors with explicit registry identity, policy ownership, metric dependencies, security grants, actions, evidence, SBSQL management, and conformance requirements.

## Authority model

Agents observe current state through metrics and other engine-owned surfaces, compare it to policy-defined desired state, and request or perform bounded actions. Agents are never correctness authority. Transaction finality, row visibility, catalog UUID identity, MGA cleanup safety, parser trust, and cluster decision proofs remain owned by their native subsystems.

## Runtime placement authority

All agents execute inside the database-owned SBsvr/SBcore runtime for the
database whose state they observe or manage. In cluster mode, cluster-only
agents execute inside the admitted cluster member database's SBsvr/SBcore runtime
after SBccore and the admitted cluster libraries are loaded by that database
server instance.

Managers, listeners, parsers, drivers, and external tools are not agent hosts.
They may request agent work through authenticated management/SBLR/API surfaces,
but they do not run agent loops, load agent libraries, activate cluster agents,
or make agent decisions. Agent code may be built into SBcore or supplied by
admitted cluster libraries; both cases still run inside the database server
instance.

## Control-loop rule

Every agent decision loop must execute these steps in order:

1. Validate agent registry row, runtime identity, component version, and scope.
2. Validate lifecycle state permits evaluation.
3. Load active policy and applicable sysarch override objects.
4. Read metric dependencies through the metric registry and required grants.
5. Reject stale, missing, untrusted, or schema-incompatible inputs unless policy allows advisory use.
6. Compute candidate health state, recommendation, or action.
7. Submit action candidate to arbitration when action may mutate runtime state or suppress another agent.
8. Validate actuator authority, security rights, policy gate, safety preconditions, and cluster fences when applicable.
9. Write evidence before reporting action success.
10. Emit agent metrics and update `SHOW`/`sys.*` surfaces.

## Required agent inventory

The canonical agent registry is defined in [`appendix-agent-canonical-registry.md`](./appendix-agent-canonical-registry.md). The initial registry includes local, dual-scope, and cluster-only agents for resources, metrics, storage, memory, indexes, autoscale, admission, parser/interface, transactions, cleanup/archive, policy recommendations, distributed query, routing, runtime learning, support, jobs, backup, archive, restore, PITR, identity, sessions, alerts, exports, and upgrades.

## Management surfaces

SBSQL management and inspection commands are defined in [`appendix-agent-sbsql-management-commands.md`](./appendix-agent-sbsql-management-commands.md). All commands lower to SBLR/API operations defined in [`appendix-agent-sblr-api-operation-matrix.md`](./appendix-agent-sblr-api-operation-matrix.md). Parsers are clients to these surfaces and cannot enforce agent authority.

## Mandatory zero-grey templates

The output contract is defined in [`appendix-agent-zero-grey-output-contract-and-templates.md`](./appendix-agent-zero-grey-output-contract-and-templates.md). Any new agent, command, action, policy, evidence schema, or conformance test that does not satisfy the templates is invalid.
