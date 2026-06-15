# Adaptive Acceleration

## Purpose

This page describes the layered execution and acceleration stack that ScratchBird
uses to execute operations efficiently, and explains why the layers above the
interpreter are *candidate accelerators* rather than authorities. The central
idea — that accelerators can speed execution but can never change the semantically
correct result — connects directly to the candidate-evidence / MGA-recheck
invariant described in [convergent_multi_model.md](convergent_multi_model.md).

**This is a draft.** Nothing here is a performance claim or a certification of
acceleration availability in any specific build or configuration.

---

## The Three-Tier Execution Stack

ScratchBird's execution architecture has three tiers, verified across
`src/engine/sblr/`, `src/engine/native_compile/`, and `src/engine/gpu_acceleration/`:

### Tier 1: SBLR Interpreter (Always Available)

The SBLR interpreter is the baseline execution tier. Every SBLR operation that
enters the engine can be executed by the interpreter. The interpreter is the
semantic authority: it defines correct behavior for values, diagnostics,
transaction side effects, MGA visibility decisions, and security evaluation.

The interpreter is always available — it does not depend on any external library,
hardware capability, or build option. A build without any acceleration capability
is a fully functional engine; it is simply not accelerated.

### Tier 2: Superinstruction Fusion And Batch Execution

The second tier operates on groups of SBLR operations that can be fused into
combined superinstructions, or on workloads that can be batched for more efficient
dispatch. This tier operates within the SBLR execution model — it produces the
same results as the interpreter, using the same transaction and security context,
by reorganizing the dispatch pattern to reduce overhead.

Superinstruction fusion and batch execution do not change semantic outcomes.
If a fused sequence would produce a different result than the interpreter for
any input, the fusion is invalid and must not be applied.

### Tier 3: JIT/AOT Native Compilation And GPU/SIMD Scoring Kernels

The third tier includes:

- **JIT (just-in-time) compilation**: SBLR units are compiled to native machine
  code at runtime using an LLVM-backed compilation pipeline. Compiled units
  execute the same semantics as the interpreter but using native machine
  instructions.
- **AOT (ahead-of-time) compilation**: SBLR units are compiled at deployment
  time or during a preparation phase and stored as native artifacts. At runtime,
  the native artifact is loaded and used in place of interpretation.
- **GPU and SIMD scoring kernels**: Specialized hardware acceleration for
  operations that benefit from data-parallel execution, particularly in vector
  similarity scoring and analytical operations.

Verified in `src/engine/native_compile/native_compile.hpp` and
`src/engine/gpu_acceleration/`.

---

## Accelerators Are Candidates, Not Authorities

The structural guarantee in the native compile subsystem is stated explicitly
in the source comment at `src/engine/native_compile/native_compile.hpp`:

> Native compilation is acceleration evidence only. SBLR interpreter semantics
> remain authoritative for values, diagnostics, transactions, MGA visibility,
> security, and side effects.

This is reflected in the `NativeCompileResult` struct: a compiled unit carries
fields for fallback behavior (`fallback_used`, `allow_interpreter_fallback`) and
an `effective_mode` that can be `refused` — the engine can decline to use a
compiled artifact and fall back to the interpreter.

The `NativeCompileRequest` carries explicit version bindings:

```
std::string engine_abi_id = "sb_engine_abi_v3";
std::string sblr_version = "sblr_v3";
std::string opcode_registry_epoch = "static_v3";
```

A compiled artifact that does not match the current engine ABI, SBLR version,
or opcode epoch is invalid and cannot be used. The engine does not execute
stale native artifacts.

### Why This Matters

An accelerated result that has not been validated against engine authority is a
candidate — a hint about what the answer might be, not a proven answer. This
mirrors the candidate-evidence / MGA-recheck invariant for data providers:

- A specialized data provider returns candidate rows that the engine rechecks
  for MGA visibility and security.
- A native-compiled execution unit computes candidate values that the engine
  validates for correctness against the SBLR semantic definition.

In both cases, the engine is the authority. Accelerators are optimization
mechanisms, not authority sources.

---

## Compile Policy Profiles

The native compile tier supports several policy profiles that control when and
how compilation is applied:

| Profile | Behavior |
|---------|---------|
| `disabled` | No compilation; interpreter only |
| `jit_optional` | JIT applied where available; interpreter fallback always permitted |
| `jit_required_for_declared_units` | Specific units must be JIT-compiled or they are refused |
| `aot_optional` | AOT applied where available; interpreter fallback permitted |
| `aot_package_required` | Package-level AOT is required |
| `dev_debug_ir_export` | Development mode; exports LLVM IR for inspection |

These profiles allow operators to configure exactly how much compilation is
applied and what the fallback behavior is. The `disabled` profile is a valid
production configuration — it simply means all execution goes through the
interpreter.

---

## GPU And SIMD Scoring Kernels

The GPU acceleration subsystem (`src/engine/gpu_acceleration/`) provides
scoring kernel acceleration, primarily for similarity scoring over large
vector sets and analytical aggregation. Like native compilation, GPU kernels
are candidate accelerators:

- They compute candidate scores or aggregates.
- The engine validates those candidates against the transaction snapshot and
  security policy before returning results.
- If the GPU backend is unavailable (missing hardware, missing library,
  unsupported build configuration), the engine falls back to non-GPU execution.

GPU acceleration is not a required component. Its availability depends on
build configuration and hardware. Do not rely on GPU acceleration without
verifying availability in the current build and environment.

---

## Connecting Acceleration To The CDE Design

The layered acceleration model serves the CDE design in a specific way: it
allows the engine to deliver execution efficiency across many different workloads
— relational aggregation, vector similarity search, full-text scoring, graph
traversal, time-series compression — without creating separate, incompatible
fast paths that bypass the engine's correctness guarantees.

Every fast path in ScratchBird is subject to:

1. The same SBLR semantic authority.
2. The same MGA recheck invariant.
3. The same security evaluation.
4. The same transaction boundary.

An accelerated vector similarity query and an unaccelerated relational join
participate in the same transaction. If a row is not visible to the current
snapshot, it is not returned by either path, regardless of whether the fast
path found it first.
