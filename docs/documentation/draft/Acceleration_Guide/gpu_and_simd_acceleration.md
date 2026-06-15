# GPU and SIMD Acceleration

**Status: draft — subject to revision before stable release.**

## Purpose

This document describes ScratchBird's GPU/SIMD acceleration layer: the GPU
gate that controls whether and how batched workloads are offered to a hardware
accelerator, and the SIMD/GPU scoring kernels that provide data-parallel
execution for specific operation kinds. It covers policy profiles, memory
budgets, determinism requirements, refusal codes, scoring kernel kinds and
routes, provider manifest authority fields, and the full SBsql management
surface.

Source references verified: `src/engine/gpu_acceleration/gpu_acceleration.hpp`,
`src/engine/gpu_acceleration/gpu_acceleration.cpp`,
`src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`,
`src/engine/executor/scoring_kernel_executor.hpp`,
`src/engine/sblr/sblr_opcode_registry.cpp`,
`src/parsers/sbsql_worker/lowering/lowering.cpp`,
`src/engine/internal_api/extensibility/gpu_api.hpp`.

---

## The GPU Gate

The GPU gate (`EvaluateGpuAcceleration`, defined in `gpu_acceleration.hpp` and
implemented in `gpu_acceleration.cpp`) is the admission function for all GPU
workloads. It enforces authority boundaries before any batch is offered to a
hardware provider.

### What the GPU Layer Is Not

The header comment in `gpu_acceleration.hpp` states:

> GPU is never semantic, security, transaction, MGA, catalog, cleanup,
> recovery, parser, or cluster-decision authority.

The gate enforces this with pre-flight refusal checks that run before any
provider is consulted. See the refusal codes section below.

### Policy Profiles

The `GpuPolicyProfile` enum controls whether the GPU layer is active and
how failures are handled (verified in `gpu_acceleration.hpp` and named strings
verified in `GpuPolicyProfileName` in `gpu_acceleration.cpp`):

| Profile enum | Named string | Description |
|-------------|-------------|-------------|
| `disabled` | `gpu_accel.disabled` | No GPU execution. All requests fall back to CPU reference. Default. |
| `optional_batch` | `gpu_accel.optional_batch` | GPU attempted; CPU fallback permitted if provider is unavailable or workload fails. |
| `required_for_declared_workload` | `gpu_accel.required_for_declared_workload` | GPU required for workloads that declare it; hard refusal if backend unavailable. |
| `cluster_optional` | `gpu_accel.cluster_optional` | Cluster-aware GPU dispatch; CPU fallback permitted. |
| `cluster_required` | `gpu_accel.cluster_required` | Cluster-aware GPU dispatch required; hard refusal if cluster authority unavailable. |
| `dev_kernel_debug` | `gpu_accel.dev_kernel_debug` | Development and debug mode. Not for production use. Treated as optional. |

### Effective Paths

After the gate evaluates a request, it returns one of four effective paths
(`GpuEffectivePath`, verified in `gpu_acceleration.hpp` and
`GpuEffectivePathName`):

| Path | Meaning |
|------|---------|
| `inspect_only` | Request was informational; no execution attempted |
| `cpu_fallback` | Policy is optional and provider is unavailable, or workload requires CPU equivalence |
| `gpu_provider_admitted` | Request was passed to the GPU provider |
| `refused` | Request was refused due to a policy violation or authority check failure |

### Batch and Memory Budgets

Before a batch is offered to the GPU provider, the gate verifies that the batch
fits within memory budgets (verified in `BatchFitsBudget` in
`gpu_acceleration.cpp`):

- `device_memory_budget_bytes` — default 64 MiB; the batch (combined size of
  `values` and `rhs_values` as `double` arrays) must not exceed this limit.
- `pinned_host_memory_budget_bytes` — default 16 MiB; the same batch size
  must not exceed this limit.

Both checks use the same batch size calculation. If either limit is exceeded,
the gate returns `GPU.DEVICE_MEMORY_POLICY_VIOLATION`.

### Determinism Requirement

The `deterministic_equivalence_required` field on a request controls how the
gate handles workloads that cannot be proven equivalent between GPU and CPU
execution. By default it is `true`.

The gate applies a CPU-required check (`ExactWorkloadRequiresCpu`) for workloads
`aggregate`, `columnar_scan`, and `sort` when `deterministic_equivalence_required`
is `true` and `approximate_declared` is `false`. If a non-optional profile is
active and this check fires, the gate returns `GPU.DETERMINISM_NOT_PROVEN`. If
the profile is optional (including `cluster_optional` and `dev_kernel_debug`),
the gate silently uses the CPU reference path and reports `fallback_used = true`.

Supported workload classes (verified in `GpuWorkloadSupported`) are:
`vector`, `search`, `columnar_scan`, `aggregate`, `sort`, `index_build`,
`timeseries_transform`, `compression_transform`, `graph_batch`.

### Batch Operations

The `GpuBatchOperation` enum identifies the operation to execute on the batch
(verified in `gpu_acceleration.hpp` and `GpuBatchOperationName`):

| Operation | Description |
|-----------|-------------|
| `none` | No batch operation |
| `filter_positive` | Retain values greater than zero |
| `project_scale` | Multiply each value by a scale factor |
| `aggregate_sum` | Sum all values |
| `vector_dot` | Dot product of two equal-length vectors |

The `vector_dot` operation requires that `values` and `rhs_values` have equal
length; a mismatch produces `GPU.INVALID_ACCELERATION_POLICY`.

---

## GPU Refusal Codes

All refusal codes are verified in `EvaluateGpuAcceleration` in
`gpu_acceleration.cpp`. They are returned in `GpuAccelerationResult.diagnostic_code`.

| Code | Condition |
|------|-----------|
| `GPU.AUTHORITY_BYPASS_REFUSED` | Request set `authority_bypass_requested = true`; the GPU layer cannot be a transaction, security, visibility, catalog, MGA, or cluster authority |
| `GPU.UNSAFE_PAGE_ACCESS_REFUSED` | Request set `direct_page_or_catalog_access_requested = true`; only materialized, authorized batches are permitted |
| `GPU.SBLR_ONLY_KERNELS_REQUIRED` | Request set `raw_client_or_parser_input_requested = true`; kernels must originate from validated SBLR or engine-internal sources |
| `GPU.CLUSTER_PLACEMENT_UNAVAILABLE` | Cluster dispatch requested or a cluster profile is active, but cluster authority is not available |
| `GPU.SECURITY_CONTEXT_REQUIRED` | GPU control requires an engine security context (enforced when `security_context_present = false` and execution is requested) |
| `GPU.DISABLED_BY_POLICY` | Active profile is `disabled` |
| `GPU.WORKLOAD_UNSUPPORTED` | The workload class is not in the supported set |
| `GPU.INVALID_ACCELERATION_POLICY` | Structural policy violation (e.g., vector length mismatch for `vector_dot`) |
| `GPU.DEVICE_MEMORY_POLICY_VIOLATION` | Batch exceeds device or pinned-memory budget |
| `GPU.BACKEND_UNAVAILABLE` | A non-optional profile requires a provider but none is available |
| `GPU.DETERMINISM_NOT_PROVEN` | Exact workload requires CPU equivalence proof; non-optional profile active |
| `GPU.RUNTIME_COMPATIBILITY_REFUSED` | Runtime compatibility negotiation failed closed |
| `GPU.RUNTIME_COMPATIBILITY_FALLBACK` | Runtime compatibility negotiation requested scalar fallback (informational) |
| `GPU.FALLBACK_USED` | CPU reference path used by optional GPU policy (informational, not an error) |

---

## Scoring Kernels

Scoring kernels are the mechanism by which specific operation kinds are
accelerated using SIMD or GPU hardware. They are distinct from the full GPU
batch gate: they operate on materialized, authorized executor batches and
are routed through the executor hook `ExecuteOptionalScoringKernel`
(verified in `src/engine/executor/scoring_kernel_executor.hpp`).

### Kernel Kinds

The `ScoringKernelKind` enum defines six supported operation kinds (verified in
`src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`):

| Kind constant | Description |
|---------------|-------------|
| `kVectorDistance` | Vector similarity and distance computation |
| `kBm25` | BM25 text relevance scoring |
| `kBitmapIntersection` | Bitmap set intersection |
| `kTimeAggregate` | Time-series aggregation |
| `kJsonPath` | Structured document path evaluation |
| `kGraphMembership` | Graph reachability and membership queries |

The `kUnknown` value is the zero/error sentinel.

### Kernel Routes

After the executor evaluates a scoring kernel request, it returns one of three
routes (`ScoringKernelRoute`):

| Route | Meaning |
|-------|---------|
| `kAccelerator` | Hardware-accelerated kernel was used |
| `kScalarFallback` | Scalar reference path was used (success path with fallback) |
| `kRefused` | Kernel was refused; result is not available |

### Provider Manifest Authority Fields

Every scoring kernel provider must supply a `ScoringKernelProviderManifest`
(verified in `scoring_kernel_acceleration.hpp`). The manifest carries explicit
authority denial fields that must all be `false`:

| Field | Authority denied |
|-------|-----------------|
| `claims_transaction_finality_authority` | Transaction commit/rollback decisions |
| `claims_visibility_authority` | MGA snapshot visibility |
| `claims_security_policy_authority` | Privilege and access control |
| `claims_redaction_policy_authority` | Column masking and row redaction |
| `claims_parser_or_reference_authority` | SQL parsing and reference-result determination |
| `claims_recovery_authority` | Crash recovery and WAL application |
| `claims_page_or_catalog_authority` | Direct page access and catalog mutation |

A provider that claims any of these authorities is rejected.

The manifest also carries capability fields: `provider_id`, `engine_abi_id`
(must be `sb_engine_abi_v3`), `runtime_identity_id`, `architecture`,
`cpu_capabilities`, `gpu_capabilities`, and the set of `supported_kinds`.
The `safe_to_execute` field must be `true`.

### Scalar Reference

Every `ScoringKernelRequest` carries a `scalar_reference` — a callable of type:

```
std::function<ScoringKernelValueBatch(const ScoringKernelInputBatch&)>
```

This reference is supplied by the engine (via the executor), not the provider.
If the accelerated kernel produces output that cannot be verified or the
accelerator fails, the engine calls this reference and uses its output. This
is what makes scoring kernels fail-closed: the interpreter-level reference is
always available.

### Input Batch Authorization

The `ScoringKernelInputBatch` struct carries two safety flags (verified in
`scoring_kernel_acceleration.hpp`):

- `materialized_authorized_batch` — must be `true`; the batch has been
  through the executor's authorization path
- `raw_client_or_parser_input` — must be `false`; raw input is never
  offered to a scoring kernel provider
- `direct_page_or_catalog_access` — must be `false`; providers never
  receive direct storage access

---

## SBsql Management Surface

All statements below are verified against `lowering.cpp` and
`sblr_opcode_registry.cpp`.

### GPU Control Statements (ALTER GPU)

These statements require `sysarch_authorized` security class and use the
`sblr.acceleration.gpu.v3` SBLR family. All return result shape
`rs.acceleration.control.v1` and route through `EngineControlGpuAcceleration`.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `ALTER GPU ARTIFACT <ref> QUARANTINE` | `alter.gpu.artifact_quarantine` | `SBLR_OP_GPU_ARTIFACT_QUARANTINE` | `sys.acceleration.gpu_artifacts` |
| `ALTER GPU CACHE CLEAR` | `alter.gpu.cache_clear` | `SBLR_OP_GPU_CACHE_CLEAR` | `sys.acceleration.gpu_cache` |
| `ALTER GPU DEVICE <ref> QUARANTINE` | `alter.gpu.device_quarantine` | `SBLR_OP_GPU_DEVICE_QUARANTINE` | `sys.acceleration.gpu_devices` |
| `ALTER GPU KERNEL <ref> QUARANTINE` | `alter.gpu.kernel_quarantine` | `SBLR_OP_GPU_KERNEL_QUARANTINE` | `sys.acceleration.gpu_kernels` |
| `ALTER GPU PROFILE <ref> DISABLE` | `alter.gpu.profile_disable` | `SBLR_OP_GPU_PROFILE_DISABLE` | `sys.acceleration.gpu_profiles` |
| `ALTER GPU PROFILE <ref> ENABLE` | `alter.gpu.profile_enable` | `SBLR_OP_GPU_PROFILE_ENABLE` | `sys.acceleration.gpu_profiles` |

### GPU Inspect Statements (SHOW GPU)

These statements do not require a transaction context. They route through
`EngineInspectGpuAcceleration` using the `sblr.acceleration.gpu.v3` SBLR family.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `SHOW GPU` | `show.gpu` | `SBLR_OP_SHOW_GPU` | `sys.acceleration.gpu_runtime` |
| `SHOW GPU ARTIFACTS` | `show.gpu_artifacts` | `SBLR_OP_SHOW_GPU_ARTIFACTS` | `sys.acceleration.gpu_artifacts` |
| `SHOW GPU CAPABILITY` | `show.gpu_capability` | `SBLR_OP_SHOW_GPU_CAPABILITY` | `sys.acceleration.gpu_capability` |
| `SHOW GPU DEVICES` | `show.gpu_devices` | `SBLR_OP_SHOW_GPU_DEVICES` | `sys.acceleration.gpu_devices` |
| `SHOW GPU KERNELS` | `show.gpu_kernels` | `SBLR_OP_SHOW_GPU_KERNELS` | `sys.acceleration.gpu_kernels` |
| `SHOW GPU MEMORY` | `show.gpu_memory` | `SBLR_OP_SHOW_GPU_MEMORY` | `sys.acceleration.gpu_memory` |

The SHOW GPU statements use result shapes `rs.show.gpu.v1` (SHOW GPU,
SHOW GPU CAPABILITY, SHOW GPU DEVICES, SHOW GPU MEMORY) and
`rs.show.native_compile.v1` (SHOW GPU ARTIFACTS, SHOW GPU KERNELS).

`EngineInspectGpuCapability` is also used by inspect statements that query
hardware capability without requiring a full acceleration context (verified in
`gpu_api.hpp` and referenced in `lowering.cpp` at lines 4918, 4930, 4942).

### Opcode Registry Entries

The SBLR opcode registry defines four acceleration management opcodes for the
GPU tier, all with category `management` and security class `sysarch_authorized`
(verified in `sblr_opcode_registry.cpp`):

- `acceleration.gpu.compile` → `SBLR_ACCEL_GPU_COMPILE`
- `acceleration.gpu.inspect` → `SBLR_ACCEL_GPU_INSPECT`
- `acceleration.gpu.invalidate` → `SBLR_ACCEL_GPU_INVALIDATE`
- `acceleration.gpu.policy_set` → `SBLR_ACCEL_GPU_POLICY_SET`

The op-level opcodes (used by the SBsql surface above) are category
`extensibility`:

- `op.gpu.artifact_quarantine` → `SBLR_OP_GPU_ARTIFACT_QUARANTINE`
- `op.gpu.cache_clear` → `SBLR_OP_GPU_CACHE_CLEAR`
- `op.gpu.device_quarantine` → `SBLR_OP_GPU_DEVICE_QUARANTINE`
- `op.gpu.kernel_quarantine` → `SBLR_OP_GPU_KERNEL_QUARANTINE`
- `op.gpu.profile_disable` → `SBLR_OP_GPU_PROFILE_DISABLE`
- `op.gpu.profile_enable` → `SBLR_OP_GPU_PROFILE_ENABLE`
- `op.show.gpu` → `SBLR_OP_SHOW_GPU`
- `op.show.gpu_artifacts` → `SBLR_OP_SHOW_GPU_ARTIFACTS`
- `op.show.gpu_capability` → `SBLR_OP_SHOW_GPU_CAPABILITY`
- `op.show.gpu_devices` → `SBLR_OP_SHOW_GPU_DEVICES`
- `op.show.gpu_kernels` → `SBLR_OP_SHOW_GPU_KERNELS`
- `op.show.gpu_memory` → `SBLR_OP_SHOW_GPU_MEMORY`

### Engine Internal API

(Verified in `src/engine/internal_api/extensibility/gpu_api.hpp`.)

| Entry point | Purpose |
|-------------|---------|
| `EngineInspectGpuCapability` | Query hardware capability without full acceleration context |
| `EngineControlGpuAcceleration` | Execute ALTER GPU control statements |
| `EngineInspectGpuAcceleration` | Execute SHOW GPU inspect statements |

---

## Catalog Tables Reference

| Table | Contents |
|-------|----------|
| `sys.acceleration.gpu_runtime` | Current GPU runtime state |
| `sys.acceleration.gpu_artifacts` | Compiled GPU artifacts |
| `sys.acceleration.gpu_capability` | Detected hardware capabilities |
| `sys.acceleration.gpu_devices` | Known GPU devices and their status |
| `sys.acceleration.gpu_kernels` | Registered GPU kernels |
| `sys.acceleration.gpu_memory` | Device and pinned memory usage |
| `sys.acceleration.gpu_profiles` | Policy profile records |
| `sys.acceleration.gpu_cache` | GPU kernel cache state |

---

## Cross-Links

- [README.md](README.md) — guide overview
- [acceleration_authority_model.md](acceleration_authority_model.md) — refusal codes in authority context
- [execution_tiers.md](execution_tiers.md) — where GPU fits in the three-tier stack
- [native_compilation.md](native_compilation.md) — LLVM JIT/AOT
- [operating_acceleration.md](operating_acceleration.md) — operator workflow for GPU management
- [../Language_Reference/syntax_reference/ebnf/acceleration_statement.md](../Language_Reference/syntax_reference/ebnf/acceleration_statement.md)
- [../Security_Guide/trust_and_separation_architecture.md](../Security_Guide/trust_and_separation_architecture.md)
