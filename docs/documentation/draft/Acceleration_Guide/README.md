# ScratchBird Acceleration Guide

**Status: draft — subject to revision before stable release.**

## Purpose

This guide describes the layered execution-speed stack in ScratchBird, a
Convergent Data Engine (CDE). ScratchBird can accelerate query and computation
workloads through superinstruction fusion, LLVM-backed native compilation
(JIT and AOT), and GPU/SIMD scoring kernels. All of these mechanisms operate
under a single unifying safety principle described in this guide as the
**candidate-accelerator principle**.

Understanding acceleration at this level is not required for ordinary query
authoring or administration. It is intended for:

- Advanced operators tuning workloads or managing compiled artifacts
- Integrators building or certifying acceleration providers
- Security reviewers auditing the boundary between accelerated and authoritative
  execution paths

---

## The Candidate-Accelerator Principle

Acceleration in ScratchBird rests on one structural invariant:

> **Accelerators are candidates. The exact SBLR interpreter reference path is
> always the source of truth.**

Every acceleration layer — superinstruction fusion, native JIT/AOT compilation,
GPU/SIMD scoring kernels — produces candidate output. That output is only used
when it can be verified or substituted safely. If an accelerator is unavailable,
fails, is quarantined, or is disabled by policy, the engine falls back to the
interpreter. No query result, transaction outcome, security decision, or catalog
state ever depends exclusively on an accelerated path.

This is enforced by five structural properties, each described in detail in
[acceleration_authority_model.md](acceleration_authority_model.md):

1. **Never authority.** No accelerator holds semantic, transaction, visibility,
   security, redaction, recovery, parser, catalog, or cluster-decision authority.
2. **Exact reference is truth.** The scalar SBLR interpreter result is the
   correctness oracle. Accelerated paths are verified against it or replaced by it.
3. **Opt-in.** All acceleration is disabled by default. Operators explicitly
   enable it through policy profiles.
4. **Policy-gated.** Each tier has named policy profiles that control whether
   acceleration may run, is required, or is refused.
5. **Epoch-bound and fail-closed.** Compiled artifacts carry multi-epoch cache
   keys. Any change to a security, catalog, policy, or opcode-registry epoch
   invalidates dependent artifacts. On any verification failure or ambiguity,
   the engine fails closed and uses the interpreter.

---

## The Three Execution Tiers

ScratchBird's execution stack has three tiers, each building on the one below.

| Tier | Mechanism | Source reference |
|------|-----------|-----------------|
| 1 | SBLR interpreter — always available; semantic authority | `src/engine/sblr/sblr_runtime.hpp` |
| 2 | Superinstruction fusion and batch dispatch | `src/engine/sblr/sblr_hot_path_execution.hpp` |
| 3 | LLVM JIT/AOT native compilation; GPU/SIMD scoring kernels | `src/engine/native_compile/`, `src/engine/gpu_acceleration/` |

Tier 1 is never absent. Tiers 2 and 3 are optional accelerators. The engine
always has a Tier 1 path available and uses it whenever a higher tier is
unavailable, disabled, or fails verification.

---

## Guide Structure

| Document | Contents |
|----------|----------|
| [acceleration_authority_model.md](acceleration_authority_model.md) | The unifying safety model: authority boundaries, epoch binding, lowerability bans, fail-closed refusal |
| [execution_tiers.md](execution_tiers.md) | How the three tiers work, hotness detection, escalation, and fallback |
| [native_compilation.md](native_compilation.md) | LLVM JIT/AOT: policy profiles, cache key dimensions, AOT artifact format, specialization kinds |
| [gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md) | GPU gate, scoring kernels, policy profiles, refusal codes, SBsql surface |
| [operating_acceleration.md](operating_acceleration.md) | Operator workflow: enabling, quarantine, cache management, inspection, diagnostics |

---

## Cross-Links

- **Up (CDE concepts):** [../CDE_Concepts/adaptive_acceleration.md](../CDE_Concepts/adaptive_acceleration.md)
- **EBNF grammar pages:**
  - [../Language_Reference/syntax_reference/ebnf/acceleration_statement.md](../Language_Reference/syntax_reference/ebnf/acceleration_statement.md)
  - [../Language_Reference/syntax_reference/ebnf/alter_acceleration.md](../Language_Reference/syntax_reference/ebnf/alter_acceleration.md)
  - [../Language_Reference/syntax_reference/ebnf/show_acceleration.md](../Language_Reference/syntax_reference/ebnf/show_acceleration.md)
  - [../Language_Reference/syntax_reference/ebnf/alter_management.md](../Language_Reference/syntax_reference/ebnf/alter_management.md)
- **SBLR envelope:** [../Embedding_API_Reference/sblr_envelope.md](../Embedding_API_Reference/sblr_envelope.md)
- **Operations administration:** [../Operations_Administration/README.md](../Operations_Administration/README.md)
- **Security:** [../Security_Guide/trust_and_separation_architecture.md](../Security_Guide/trust_and_separation_architecture.md)
