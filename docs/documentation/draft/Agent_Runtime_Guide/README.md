# ScratchBird Agent Runtime Guide

## Purpose

This guide is the authoritative deep-dive reference for the ScratchBird Agent Runtime — the subsystem by which the engine manages itself. Operators, advanced integrators, and anyone who needs to understand how ScratchBird maintains convergent multi-model data without constant human intervention should read it.

Audience: operators responsible for production deployments, integrators embedding ScratchBird, and security reviewers evaluating autonomous-action scope.

**Draft status.** Every concrete claim here has been verified against the project source tree. No claim constitutes a production readiness statement or an autonomous-behavior guarantee for any specific build configuration.

---

## What the Agent Runtime Is

ScratchBird is a Convergent Data Engine (CDE): a single engine that manages relational, key-value, document, search, vector, graph, and time-series models under a unified transactional foundation. The breadth of that convergence creates a management surface far larger than any single-model system. Keeping it operational — cleaning old row versions, draining index debt across all model families, managing storage capacity, running backup-readiness drills, monitoring node health, tuning approximate-search parameters, handling session pressure — would require continuous human attention if managed manually.

The Agent Runtime is the answer: a governed set of autonomous agents that observe engine state, decide what to do, and execute within strictly bounded authority. Agents are first-class engine citizens. They are declared in a single machine-readable manifest, each assigned a deployment scope, authority class, and default activation profile. Their actions are arbitrated, resource-governed, dry-run tested, evidence-backed, and optionally subject to operator approval before execution.

---

## Safety Philosophy

The central design principle of the Agent Runtime is that **autonomy is bounded**. Specifically:

**The engine owns. Agents never do.**

- **Transaction finality** belongs to the engine's Multi-Generational Architecture (MGA). No agent can commit or abort a transaction on behalf of a user.
- **Catalog visibility** is owned by the engine. An agent observing catalog state reads from the engine's catalog surfaces; it does not get to decide what other sessions can see.
- **Parser authority** belongs to the registered parser package. Agents cannot interpret or modify SQL text, cannot alter the parse-to-SBLR pipeline, and cannot route queries.
- **WAL/recovery authority** belongs to the engine's recovery subsystem. Agents can observe recovery state and recommend action; they cannot drive WAL replay or crash recovery.
- **Storage identity** is UUID-tracked by the engine. Agent persistence uses the engine's storage authority, not a write-ahead log of its own.

Beyond these hard prohibitions, agents operate under an authority ladder (observe only → recommend only → request action → direct bounded action), fail closed on any missing proof, produce tamper-evident evidence for every decision, and require explicit operator approval before transitioning from dry-run to live-action.

Conventional database background-maintenance workers typically operate with broad, implicit engine privilege and fail silently or cause indeterminate state when something goes wrong. ScratchBird agents instead carry no implicit privilege: every action is policy-gated, arbitrated, resource-budgeted, and evidence-recorded. This makes the autonomous subsystem auditable, reversible where possible, and observable without special tooling.

---

## Structure of This Guide

| Page | Contents |
|------|----------|
| [authority_and_activation_model.md](authority_and_activation_model.md) | 4-tier authority ladder, 5 activation profiles, 14 lifecycle states, action result classes, hard authority boundaries |
| [agent_catalog.md](agent_catalog.md) | The complete verified agent set (29 agents), grouped by domain, with deployment class, scope, authority, default activation |
| [action_lifecycle_and_arbitration.md](action_lifecycle_and_arbitration.md) | Action lifecycle from proposal to evidence, arbitration priority ordering, overrides, break-glass |
| [governance_and_resource_control.md](governance_and_resource_control.md) | 14-dimension resource budget, decision kinds, foreground protection, worker capacity, rollout profiles, tenant coordination |
| [evidence_explainability_and_safety.md](evidence_explainability_and_safety.md) | Tamper-evident evidence chain, dry-run, simulation, replay quarantine, explainability, fault injection, safe mode |
| [maintenance_and_tuning_agents.md](maintenance_and_tuning_agents.md) | Deep dives: transaction pressure manager, cleanup debt scheduler, online maintenance progress, adaptive tuning, restore drills |
| [observability_and_control.md](observability_and_control.md) | sys.information.* agent projections, sys.agents surface, SBsql control surface, operator levers |

---

## Cross-References

- **SBsql control surface** (agent statements, ALTER AGENT, SHOW AGENTS, override management): [../Language_Reference/syntax_reference/agent.md](../Language_Reference/syntax_reference/agent.md). This guide does not restate syntax; it explains the architecture behind it.
- **CDE-level summary** (why autonomous operation matters in a CDE, orientation): [../CDE_Concepts/autonomous_operation.md](../CDE_Concepts/autonomous_operation.md)
- **Security Guide** (rights required for agent control, evidence redaction, protected material): [../Security_Guide/README.md](../Security_Guide/README.md)
- **Operations and Administration** (monitoring, health, readiness, backup/restore runbooks): [../Operations_Administration/README.md](../Operations_Administration/README.md)
- **Transaction pressure escalation ladder** (warn/restart/reauth/cancel/force thresholds): [../Getting_Started/core_concepts/understanding_mga.md](../Getting_Started/core_concepts/understanding_mga.md)
