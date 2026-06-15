# ScratchBird CDE Concepts Manual

## Purpose

This manual explains what a Convergent Data Engine (CDE) is as a product class,
and why ScratchBird is designed as one. It is the architectural through-line that
ties together the deep reference manuals. It does not replace them: it summarizes
each pillar and links down to the authoritative pages.

**This is a draft.** No claim herein constitutes a production certification,
performance benchmark, compatibility guarantee, or external audit statement.
Every concrete subsystem claim has been verified against the source tree. Where a
claim could not be fully verified, this manual stays at the conceptual level.

---

## What Is A Convergent Data Engine?

A CDE is an engine design that unifies multiple data models, multiple client
dialects, autonomous operation, and layered acceleration under a single authority
line — one UUID/MGA/SBLR authority that fails closed. Instead of routing different
data shapes to separate products, a CDE lets them share one catalog, one
transaction system, one security model, one backup-and-restore path, and one
diagnostic surface.

The defining headline: **one engine converges many models, many dialects,
autonomous self-management, and adaptive acceleration under a single
authority that always rechecks and always fails closed.**

See [what_is_a_convergent_data_engine.md](what_is_a_convergent_data_engine.md)
for the full architectural-altitude definition and what the term explicitly
does not promise.

---

## The CDE Pillars

This manual is organized around eight architectural pillars. Each chapter
summarizes one pillar and links to the deep reference for full detail.

| Chapter | Pillar |
|---------|--------|
| [what_is_a_convergent_data_engine.md](what_is_a_convergent_data_engine.md) | What converges and what does not |
| [convergent_multi_model.md](convergent_multi_model.md) | Many data models under one catalog, transaction, and security authority |
| [multi_generational_foundation.md](multi_generational_foundation.md) | MGA: versions, not overwrites; no write-ahead log; snapshots; cleanup; time-travel |
| [identity_and_recursive_schema.md](identity_and_recursive_schema.md) | UUID-anchored durable identity and the recursive schema tree |
| [dialect_plurality_and_parser_separation.md](dialect_plurality_and_parser_separation.md) | Many client languages lowering to one engine representation through untrusted parser packages |
| [autonomous_operation.md](autonomous_operation.md) | Governed autonomous agent runtime with scoped authority |
| [adaptive_acceleration.md](adaptive_acceleration.md) | Layered execution: interpreter → superinstruction → JIT/AOT/GPU, accelerators as candidates only |
| [authority_and_trust_model.md](authority_and_trust_model.md) | The single authority line; SQL text is not authority; fail-closed; time is not authority |

A dedicated chapter covers the scope boundaries of the CDE category:

| Chapter | Purpose |
|---------|---------|
| [what_a_cde_is_not.md](what_a_cde_is_not.md) | Non-goals, scope limits, no-silent-substitution policy, how to verify features |

---

## How This Manual Relates To The Other Manuals

**Getting Started Guide** (`../Getting_Started/`) is the gentle on-ramp. If you
are new to ScratchBird, start there. It introduces the same concepts with more
analogy and step-by-step flow. This manual assumes you have read the Getting
Started overview and are ready for the architectural detail behind it.

**Deep reference manuals** cover the full detail of each pillar:

- Language Reference (`../Language_Reference/`) — SBsql syntax, data types,
  catalog tables, core paradigms including MGA, identity, parser pipeline, and
  trust architecture.
- Security Guide (`../Security_Guide/`) — trust and separation architecture,
  authentication providers, authorization, policies, cryptographic configuration.
- Operations and Administration Guide (`../Operations_Administration/`) —
  installation, configuration, service lifecycle, storage, backup, restore,
  diagnostics, parser registration.
- Agent Runtime Guide (`../Agent_Runtime_Guide/`) — full treatment of the
  autonomous agent runtime: agent types, authority ladder, activation profiles,
  policy governance, dry-run, approval, and evidence.

This manual does not duplicate those pages. It summarizes each pillar and links
down. When you need the full specification, follow the cross-links.

---

## Draft Status

Technical claims in this manual have been verified against the source tree at
`project/src`. Conceptual framing is expository. Build-configuration-dependent
behaviors are noted where relevant. No claim constitutes a production readiness,
certification, or benchmark statement.
