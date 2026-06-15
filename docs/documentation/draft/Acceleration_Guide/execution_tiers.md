# Execution Tiers

**Status: draft — subject to revision before stable release.**

## Purpose

This document describes the three-tier execution stack: how SBLR interpretation
works, how hot paths are detected and elevated to superinstruction fusion or
batch dispatch, and how those paths can be further elevated to native
compilation or GPU/SIMD scoring kernels. It also describes the fallback
discipline that ensures the interpreter is always the final safety net.

---

## Tier 1 — SBLR Interpreter

The SBLR (ScratchBird Bytecode Language Runtime) interpreter is the execution
baseline and the semantic authority. Every operation that enters the engine is
expressible as an SBLR instruction sequence, and every instruction in that
sequence can be evaluated by the interpreter.

The interpreter does not depend on any external library, hardware capability,
or build option. A build without any acceleration capability is a complete,
fully functional engine. It executes at interpreter speed, but it produces
correct results for all workloads.

The interpreter holds authority for:

- Expression values and query results
- Transaction side effects (begin, commit, rollback, savepoint)
- MGA (Multi-Generational Architecture) snapshot visibility
- Security policy checks and privilege enforcement
- Redaction and masking
- Parser boundary enforcement
- Catalog reads and mutation
- Error diagnostics

Higher tiers may produce the same values faster. They never produce different
values.

---

## Tier 2 — Superinstruction Fusion and Batch Dispatch

The second tier operates within the SBLR execution model. It does not cross
the boundary into native machine code or hardware acceleration. Its two
mechanisms are:

**Superinstruction fusion.** Adjacent SBLR opcodes that are safe to combine
into a single compound dispatch are fused into a superinstruction. The
superinstruction executes the same semantics as the original sequence but
reduces dispatch overhead. This is modeled by `SblrHotPathSuperinstructionPlan`
(verified in `src/engine/sblr/sblr_hot_path_execution.hpp`):

```
struct SblrHotPathSuperinstructionPlan {
  std::vector<std::string> fused_opcodes;   // opcodes that were combined
  std::string superinstruction_id;
  bool available;
  bool safe;
  bool exact_scalar_fallback_available;     // true when fallback is present
  std::uint64_t scalar_dispatches;          // per-iteration dispatches before fusion
  std::uint64_t fused_dispatches;           // per-iteration dispatches after fusion
};
```

Fusion is gated on `safe = true`. When fusion is not safe, the plan is not
applied and the interpreter dispatches each opcode individually.

**Batch dispatch.** When the same operation must be applied to many rows,
the dispatcher can accumulate a batch and dispatch once rather than once per
row. Batch dispatch is modeled by `SblrHotPathBatchPlan`:

```
struct SblrHotPathBatchPlan {
  std::uint64_t repeated_rows;
  std::uint64_t scalar_dispatches_per_row;
  std::uint64_t batched_dispatches_total;
  bool row_ordering_preserved;
  bool result_contract_hash_matches;        // must be true before result is accepted
  std::string expected_result_contract_hash;
  std::string observed_result_contract_hash;
};
```

The `result_contract_hash_matches` field is a structural safeguard. Before a
batched result is used, the engine verifies that the observed result contract
hash matches the expected value. If it does not match, the batch result is
discarded and the interpreter is used for each row individually.

**Savings counters.** The `SblrHotPathExecutionResult` struct records what
the tier achieved, in terms observable to the engine's profiling infrastructure:

- `dispatch_us_saved` — microseconds of dispatch overhead eliminated
- `opcode_dispatches_saved` — number of individual opcode dispatches avoided

These counters are evidence for further optimization decisions. They are not
performance guarantees or availability claims.

**Authority context.** The `SblrHotPathAuthorityContext` struct makes explicit
that no part of the hot path holds authority over security, MGA, transaction
visibility, or finality:

```
struct SblrHotPathAuthorityContext {
  bool parser_sql_execution_authority = false;
  bool reference_execution_authority = false;
  bool template_visibility_or_finality_authority = false;
  bool specialization_visibility_or_finality_authority = false;
  bool superinstruction_visibility_or_finality_authority = false;
  bool batch_visibility_or_finality_authority = false;
  // ...
};
```

All of these fields default to `false`. They are never set to `true` by the
engine for an accelerated path.

---

## Tier 3 — Native Compilation and GPU/SIMD Kernels

The third tier crosses into hardware-specific execution:

- **LLVM JIT compilation.** SBLR units are compiled to native machine code at
  runtime. The compiled module is loaded and used in place of the interpreter
  for that unit. Compilation is gated on lowerability classification, policy
  profile, epoch binding, and backend availability. Described in detail in
  [native_compilation.md](native_compilation.md).

- **LLVM AOT compilation.** Units are compiled ahead of time and stored as
  artifact files under `<database>.sb.native_aot/`. At runtime, the artifact
  is loaded and validated against the current multi-epoch cache key before use.

- **Native SBLR specialization.** The SBLR execution path itself can be
  specialized for specific kinds of hot operations (predicates, projections,
  row decode, path extraction, distance scoring, aggregates) using native
  machine code without a full LLVM compilation unit. Described in
  [native_compilation.md](native_compilation.md).

- **GPU/SIMD scoring kernels.** Materialized, authorized batches from the
  executor are offered to a scoring kernel provider. The kernel performs
  data-parallel computation on hardware-optimized code paths. The executor
  always holds the scalar reference and uses it for verification or fallback.
  Described in [gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md).

---

## Hotness Detection and Tier Escalation

A path becomes a candidate for Tier 3 when it crosses the hotness thresholds
defined in `NativeSblrHotness`
(verified in `src/engine/native_compile/native_sblr_specialization.hpp`):

```
struct NativeSblrHotness {
  std::uint64_t observed_invocations;
  std::uint64_t observed_rows;
  std::uint64_t observed_expressions;
  std::uint64_t minimum_invocations;   // threshold to cross
  std::uint64_t minimum_rows;
  std::uint64_t minimum_expressions;
};
```

When observed counts exceed the corresponding minimums, the path is eligible
for native specialization. The specific threshold values are configurable
through the policy profile system and are not hardcoded in the public surface.

For LLVM compilation, the `NativeCompileRequest` is submitted when a unit
crosses the hotness threshold. The engine selects the effective mode (JIT or
AOT) based on the active `NativeCompilePolicyProfile` and the requested mode.
If compilation succeeds, the artifact is used for subsequent invocations of
that unit. If compilation fails or the policy does not require native execution,
the interpreter continues to be used.

---

## Fallback Discipline

Every tier transitions downward on any failure:

1. If superinstruction fusion is unsafe or batch hash does not match, the
   interpreter handles each dispatch individually.
2. If native compilation fails and `allow_interpreter_fallback` is `true`, the
   engine records `fallback_used = true` and uses the interpreter.
3. If a GPU/SIMD scoring kernel is refused or fails, `fail_closed = true` is
   set in the result and the scalar reference is used.
4. If a policy profile is `disabled`, no tier above 1 is attempted.

In no case does a tier failure propagate as a query error unless the policy
explicitly requires native execution (profile `jit_required_for_declared_units`
or `aot_package_required`) and the backend is genuinely unavailable. In all
other configurations, a tier failure is silent to the query and the interpreter
takes over.

---

## Cross-Links

- [README.md](README.md) — guide overview and candidate-accelerator principle
- [acceleration_authority_model.md](acceleration_authority_model.md) — authority boundaries, epoch binding
- [native_compilation.md](native_compilation.md) — LLVM policy profiles, hotness kinds, AOT artifacts
- [gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md) — GPU gate, scoring kernels
- [operating_acceleration.md](operating_acceleration.md) — operator workflow
