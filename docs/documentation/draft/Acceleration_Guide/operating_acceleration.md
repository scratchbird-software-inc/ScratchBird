# Operating Acceleration

**Status: draft — subject to revision before stable release.**

## Purpose

This document is the operator-facing reference for managing ScratchBird's
acceleration stack. It covers the full workflow: enabling policy profiles,
quarantining problematic artifacts or devices, clearing caches, inspecting
current state through the SBsql surface, and understanding what the opt-in
posture means for operational safety.

The central guarantee for operators is stated here and detailed in
[acceleration_authority_model.md](acceleration_authority_model.md):

> **Disabling acceleration never changes query results. It changes only execution
> speed.**

All acceleration is opt-in and fail-closed. Removing or disabling any
accelerator causes the engine to use the SBLR interpreter, which always
produces correct results.

---

## The Opt-In Posture

Every acceleration mechanism is disabled by default. The policy profile for
GPU acceleration is `gpu_accel.disabled`. The policy profile for native
compilation is `native_compile.disabled`. Superinstruction fusion at Tier 2
operates within the SBLR execution model and does not require explicit opt-in,
but it too falls back safely when conditions are not met.

This means:

- A freshly configured instance has no acceleration active.
- An operator must explicitly enable a policy profile to permit acceleration.
- An operator can revert to full interpreter execution at any time by setting
  or restoring a disabled profile.
- Reversion is safe: interpreter results are always correct.

---

## Enabling and Disabling Policy Profiles

### Native Compile Profiles

To enable JIT compilation for units that qualify:

```sql
ALTER NATIVE COMPILE PROFILE <profile_ref> ENABLE;
```

To disable a profile:

```sql
ALTER NATIVE COMPILE PROFILE <profile_ref> DISABLE;
```

The named profiles (verified in `native_compile.hpp` and `lowering.cpp`) are:

| Profile | Effect when enabled |
|---------|---------------------|
| `native_compile.jit_optional` | JIT attempted; interpreter fallback on failure |
| `native_compile.jit_required_for_declared_units` | JIT required for declared units; hard refusal if backend unavailable |
| `native_compile.aot_optional` | AOT and JIT attempted; interpreter fallback on failure |
| `native_compile.aot_package_required` | AOT package required; hard refusal on artifact failure |
| `native_compile.dev_debug_ir_export` | Debug mode; not for production |

Disabling a profile causes subsequent compilations to use the interpreter.
Previously compiled artifacts remain in the cache under their cache keys but
are not loaded under the `disabled` profile.

### GPU Profiles

To enable GPU acceleration for batch workloads:

```sql
ALTER GPU PROFILE <profile_ref> ENABLE;
```

To disable:

```sql
ALTER GPU PROFILE <profile_ref> DISABLE;
```

Named GPU profiles (verified in `gpu_acceleration.cpp`):

| Profile | Effect when enabled |
|---------|---------------------|
| `gpu_accel.optional_batch` | GPU attempted; CPU fallback on failure |
| `gpu_accel.required_for_declared_workload` | GPU required for declared workloads |
| `gpu_accel.cluster_optional` | Cluster-aware GPU; CPU fallback |
| `gpu_accel.cluster_required` | Cluster-aware GPU required |
| `gpu_accel.dev_kernel_debug` | Debug mode; not for production |

The default is `gpu_accel.disabled`. Enabling any other profile takes effect
for workloads submitted after the profile is active.

---

## Quarantine Operations

Quarantine removes a specific artifact, device, or kernel from active use.
The quarantined object is not deleted; it is marked unavailable. The engine
falls back to the interpreter (for artifacts) or refuses GPU workloads that
require that device or kernel.

### Quarantine a Native Compile Artifact

```sql
ALTER NATIVE COMPILE ARTIFACT <artifact_ref> QUARANTINE;
```

Routes to `sys.native_compile.artifacts`. Opcode:
`SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE`.

Use this when an artifact is suspected of producing incorrect results or when
an artifact was built against a now-stale configuration. After quarantine, the
engine will recompile the unit or fall back to the interpreter on the next
invocation.

### Quarantine a GPU Artifact

```sql
ALTER GPU ARTIFACT <artifact_ref> QUARANTINE;
```

Routes to `sys.acceleration.gpu_artifacts`. Opcode:
`SBLR_OP_GPU_ARTIFACT_QUARANTINE`.

### Quarantine a GPU Device

```sql
ALTER GPU DEVICE <device_ref> QUARANTINE;
```

Routes to `sys.acceleration.gpu_devices`. Opcode:
`SBLR_OP_GPU_DEVICE_QUARANTINE`.

After a device is quarantined, batch workloads that would have dispatched to
that device will use the CPU reference path or another available device,
depending on the active policy profile.

### Quarantine a GPU Kernel

```sql
ALTER GPU KERNEL <kernel_ref> QUARANTINE;
```

Routes to `sys.acceleration.gpu_kernels`. Opcode:
`SBLR_OP_GPU_KERNEL_QUARANTINE`.

Kernels are the executable units within the GPU acceleration subsystem.
Quarantining a kernel prevents it from being dispatched. The scoring kernel
executor will use the scalar reference for any kernel kind that has been
quarantined.

---

## Cache Operations

### Invalidate the Native Compile Cache

```sql
ALTER NATIVE COMPILE CACHE INVALIDATE;
```

Routes to `sys.native_compile.cache`. Opcode:
`SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE`.

This forces all cached compiled artifacts to be re-evaluated on next use.
It does not delete artifact files but marks them as requiring re-validation.
Use this after a change that the epoch system may not have detected, such as
a manual update to an LLVM installation.

### Clear the GPU Cache

```sql
ALTER GPU CACHE CLEAR;
```

Routes to `sys.acceleration.gpu_cache`. Opcode:
`SBLR_OP_GPU_CACHE_CLEAR`.

This clears the GPU kernel cache. Subsequent workloads recompile GPU kernels
as needed. The CPU reference path is used until the kernel cache is repopulated.

---

## AOT Rebuild

```sql
ALTER NATIVE COMPILE AOT REBUILD <unit_ref>;
```

Routes to `sys.native_compile.aot_units`. Opcode:
`SBLR_OP_NATIVE_COMPILE_AOT_REBUILD`.

This triggers a rebuild of the named AOT unit's compiled artifact. Use it
when an AOT artifact has been quarantined, when the LLVM installation has
changed, or when an artifact file has been manually removed. The rebuild
writes a new artifact file under `<database>.sb.native_aot/<cache_key>.native_aot.meta`.

---

## Inspection Surface

The inspection surface covers both the aggregate acceleration view and the
individual GPU and native compile subsystems.

### Aggregate Acceleration View

These statements produce a summary of all acceleration subsystems:

```sql
SHOW ACCELERATION;
SHOW ACCELERATION EXTENDED;
```

- `SHOW ACCELERATION` routes to operation ID
  `observability.show_acceleration`, opcode
  `SBLR_OBSERVABILITY_SHOW_ACCELERATION`.
- `SHOW ACCELERATION EXTENDED` routes to
  `observability.show_acceleration_extended`, opcode
  `SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED`.

Both are read-only observability operations that do not require a transaction
context.

### GPU Inspection Statements

| Statement | What it shows | Catalog table |
|-----------|--------------|--------------|
| `SHOW GPU` | Current GPU runtime state and policy | `sys.acceleration.gpu_runtime` |
| `SHOW GPU CAPABILITY` | Detected hardware capabilities | `sys.acceleration.gpu_capability` |
| `SHOW GPU DEVICES` | Known devices and quarantine status | `sys.acceleration.gpu_devices` |
| `SHOW GPU KERNELS` | Registered kernels and their status | `sys.acceleration.gpu_kernels` |
| `SHOW GPU ARTIFACTS` | Compiled GPU artifacts | `sys.acceleration.gpu_artifacts` |
| `SHOW GPU MEMORY` | Device and pinned memory usage and budgets | `sys.acceleration.gpu_memory` |

### Native Compile Inspection Statements

| Statement | What it shows | Catalog table |
|-----------|--------------|--------------|
| `SHOW LLVM` | LLVM backend state: version, load mode, paths | `sys.native_compile.llvm` |
| `SHOW LLVM PROVENANCE` | SHA-256 provenance hashes for the LLVM installation | `sys.native_compile.llvm` |
| `SHOW LLVM TARGETS` | Available compilation target triples and feature sets | `sys.native_compile.llvm` |
| `SHOW AOT ARTIFACTS` | AOT artifact files: cache key, effective mode, provenance | `sys.native_compile.artifacts` |
| `SHOW NATIVE COMPILE` | Native compile subsystem runtime state | `sys.native_compile.runtime` |
| `SHOW NATIVE COMPILE CACHE` | Current cache state and contents | `sys.native_compile.cache` |

---

## Capacity and Budget Management

GPU memory limits are configured through the `device_memory_budget_bytes`
and `pinned_host_memory_budget_bytes` fields of `GpuAccelerationRequest`
(defaults: 64 MiB and 16 MiB respectively). Batches that exceed either limit
produce `GPU.DEVICE_MEMORY_POLICY_VIOLATION`.

Use `SHOW GPU MEMORY` to observe current device memory usage relative to these
budgets. If workloads are consistently hitting memory limits with an optional
profile, the engine will fall back to the CPU reference path. If a required
profile is active, workloads that exceed the budget will be refused.

LLVM memory accounting is tracked through the `LlvmMemoryAccountingRequest`
system. The `SHOW LLVM` output includes memory reservation counts and sizes
when memory accounting is active.

---

## Diagnostics

Acceleration failures appear in the `diagnostic_code` field of the relevant
result struct and are exposed through the observability surface.

For GPU failures, the refusal codes (listed in full in
[gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md)) indicate the
specific reason. Codes beginning with `GPU.` are GPU-gate refusals. The
`GPU.FALLBACK_USED` and `GPU.RUNTIME_COMPATIBILITY_FALLBACK` codes are
informational — they record that the engine chose the CPU reference path
without indicating an error.

For native compile failures, codes beginning with `NATIVE.` indicate the
specific failure. `NATIVE.COMPILE_FAILED_FALLBACK` records a successful
fallback to the interpreter; it is not an error in optional profiles.

Both subsystems emit metrics under the `sb_gpu_*` and `sb_native_compile_*`
metric families. These are accessible through `SHOW ACCELERATION EXTENDED`
and through the standard metrics surface.

---

## Operator Safety Summary

| Action | Effect on results |
|--------|------------------|
| Disable any acceleration profile | No effect on results; speed may decrease |
| Quarantine any artifact, device, or kernel | No effect on results; engine falls back |
| Clear or invalidate any cache | No effect on results; recompilation occurs on next use |
| Remove AOT artifact files | No effect on results; AOT rebuild or JIT occurs on next use |
| Revert to `disabled` profile (all tiers) | Interpreter used; results guaranteed correct |

The fundamental invariant is: **the SBLR interpreter result is always the
correct result**. Acceleration changes how that result is computed, never what
it is.

---

## Cross-Links

- [README.md](README.md) — guide overview
- [acceleration_authority_model.md](acceleration_authority_model.md) — the fail-closed guarantee in detail
- [execution_tiers.md](execution_tiers.md) — three-tier stack and fallback discipline
- [native_compilation.md](native_compilation.md) — profile names, AOT artifact format
- [gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md) — GPU refusal codes, catalog tables
- [../Operations_Administration/README.md](../Operations_Administration/README.md)
- [../Language_Reference/syntax_reference/ebnf/alter_acceleration.md](../Language_Reference/syntax_reference/ebnf/alter_acceleration.md)
- [../Language_Reference/syntax_reference/ebnf/show_acceleration.md](../Language_Reference/syntax_reference/ebnf/show_acceleration.md)
