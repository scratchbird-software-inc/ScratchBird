# Acceleration Authority Model

**Status: draft — subject to revision before stable release.**

## Purpose

This document defines the authority model that governs every tier of
ScratchBird's acceleration stack. All acceleration mechanisms — superinstruction
fusion, LLVM JIT/AOT compilation, native SBLR specialization, and GPU/SIMD
scoring kernels — share this model. Understanding it is prerequisite to reading
the tier-specific documents.

The model answers one question: **when can the engine trust an accelerated
result instead of the interpreter?**

The short answer is: it can use an accelerated result when the accelerator is
not claiming any authority that belongs exclusively to the reference path, when
all epoch bindings match, and when the policy profile permits it. If any of
these conditions fails, the engine fails closed and uses the interpreter.

---

## The Candidate-Accelerator Principle

The source comment in
`src/engine/native_compile/native_compile.hpp` states the principle precisely:

> Native compilation is acceleration evidence only. SBLR interpreter semantics
> remain authoritative for values, diagnostics, transactions, MGA visibility,
> security, and side effects.

A parallel statement appears in
`src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`:

> SIMD/GPU scoring kernels are optional accelerators over materialized,
> authorized executor batches. Exact scalar executor evaluation remains the
> authority for results, transaction finality, MGA visibility, security,
> redaction, parser boundaries, reference compatibility, recovery, pages, and
> catalog state.

The GPU acceleration header (`src/engine/gpu_acceleration/gpu_acceleration.hpp`)
makes this concrete with a structured comment:

> GPU is never semantic, security, transaction, MGA, catalog, cleanup, recovery,
> parser, or cluster-decision authority.

These are not aspirational statements. They are enforced through the refusal
codes and manifest fields described below.

---

## Authority Boundaries by Domain

The following domains always belong to the reference path (SBLR interpreter and
engine-owned subsystems). No accelerator may claim, influence, or replace any
of these:

| Domain | What it means | Accelerator role |
|--------|---------------|-----------------|
| Semantic values | The correct output of any expression or query | Candidate — must match reference |
| Transaction finality | Whether a commit or rollback takes effect | Forbidden entirely |
| MGA visibility | Which data version is visible in a snapshot | Forbidden entirely |
| Security policy | Grant checks, privilege enforcement | Forbidden entirely |
| Redaction policy | Column masking, row filtering for redaction | Forbidden entirely |
| Parser authority | Parsing, AST construction, SQL text interpretation | Forbidden entirely |
| Reference compatibility | Determining what constitutes a correct result | Forbidden entirely |
| Recovery | Crash recovery, WAL application, redo | Forbidden entirely |
| Page or catalog access | Direct storage page reads, catalog mutation | Forbidden entirely |
| Cluster placement | Which node handles a workload | Forbidden entirely |

These boundaries are enforced structurally. The `ScoringKernelProviderManifest`
struct (verified in `src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`)
carries explicit boolean fields that a provider must set to `false`:

- `claims_transaction_finality_authority`
- `claims_visibility_authority`
- `claims_security_policy_authority`
- `claims_redaction_policy_authority`
- `claims_parser_or_reference_authority`
- `claims_recovery_authority`
- `claims_page_or_catalog_authority`

The same fields appear in `NativeSblrProviderManifest`
(`src/engine/native_compile/native_sblr_specialization.hpp`):

- `claims_transaction_finality_authority`
- `claims_visibility_authority`
- `claims_security_policy_authority`
- `claims_redaction_policy_authority`
- `claims_parser_or_reference_authority`

A provider manifest with any of these set to `true` is rejected.

---

## Exact Scalar Reference Verification

Every accelerator path carries a `scalar_reference` — a callable that produces
the exact interpreter result for any input batch. This reference is supplied by
the engine, not the provider.

For scoring kernels (`ScoringKernelRequest.scalar_reference`), the reference
function is of type `ScoringKernelScalarReference`, defined as:

```
std::function<ScoringKernelValueBatch(const ScoringKernelInputBatch&)>
```

For native SBLR specialization (`NativeSblrSpecializationRequest.scalar_reference`):

```
std::function<NativeSblrValueBatch(const NativeSblrInputBatch&)>
```

The engine uses this reference to verify accelerator output or to substitute
the correct result when the accelerator path is unavailable. The
`ScoringKernelResult.fail_closed` field records whether the engine fell back
to the reference after an accelerator failure.

Batch dispatch at Tier 2 verifies the result against a contract hash:
`SblrHotPathBatchPlan.result_contract_hash_matches` must be `true` before a
batched result is accepted as equivalent to the scalar interpretation.

---

## Epoch Binding and Artifact Invalidation

Compiled native artifacts (JIT modules and AOT artifacts) are bound to a
multi-epoch cache key. The key is computed in `native_compile.cpp` as a SHA-256
digest over the following dimensions (verified in `CacheKeyMaterial`):

| Dimension | Field |
|-----------|-------|
| SBLR module payload digest | SHA-256 of `module_payload` |
| SBLR version string | `sblr_version` (e.g., `sblr_v3`) |
| Opcode registry epoch | `opcode_registry_epoch` (e.g., `static_v3`) |
| Target object UUID | `target_object_uuid` |
| Principal UUID | `principal_uuid` |
| Catalog generation ID | `catalog_generation_id` |
| Security epoch | `security_epoch` |
| Policy epoch | `policy_epoch` |
| Resource epoch | `resource_epoch` |
| Engine ABI identifier | `engine_abi_id` (e.g., `sb_engine_abi_v3`) |
| Numeric backend profile | `numeric_backend_profile` |
| Backend provider and version | from `BackendInfo` |
| LLVM load mode | `llvm_load_mode` |
| LLVM library path digest | SHA-256 of library path |
| LLVM source root digest | SHA-256 of source root |
| LLVM tools root digest | SHA-256 of tools root |
| LLVM staging build directory digest | SHA-256 of staging build dir |
| Target triple | `target_triple` |
| Target feature set | comma-joined CPU feature flags |
| Compile mode | `jit` or `aot` |
| Unit kind | from lowerability classification |
| Descriptor set | per-descriptor UUID, kind, type, encoded digest |
| Policy profiles | all active policy profile strings |
| Physical profiles | all active physical profile strings |
| Option envelope hashes | SHA-256 per option envelope |

The final cache key is `"llvm-" + SHA256(material_string)`.

An artifact is invalidated whenever any dimension in its cache key material
changes. The function `NativeArtifactInvalidatedByDependency` performs this
check: if a dependency family and value do not appear in the cache key material,
the artifact is considered stale. Any change to `security_epoch`,
`catalog_generation_id`, `policy_epoch`, or `opcode_registry_epoch` invalidates
every artifact that was compiled under the previous values.

Native SBLR specialization carries a tighter per-invocation epoch check
(`NativeSblrEpochBinding`):

- `security_epoch` / `expected_security_epoch`
- `redaction_epoch` / `expected_redaction_epoch`

If the observed epoch does not match the expected epoch, the specialization
result is discarded and the scalar reference is used instead.

---

## Lowerability Bans

Before a unit is admitted for LLVM compilation, `ClassifyLowerability`
classifies it. The following ban codes cause an immediate refusal to compile
(verified in `native_compile.cpp`):

| Ban code | What triggers it |
|----------|-----------------|
| `sql_compile_forbidden` | Module payload contains SQL text (SELECT, INSERT, UPDATE, DELETE, or `sql:` prefix) |
| `parser_authority_forbidden` | Payload or options reference `parser_ast`, `parse_tree`, or `parser_authority` |
| `reference_authority_forbidden` | Payload or options reference `reference` authority, plan, or result |
| `protocol_or_client_authority_forbidden` | Payload or options reference `protocol_frame`, `wire_frame`, or `client_ir` |
| `engine_ir_validation_required` | Engine IR input present but not marked validated |
| `sblr_or_engine_ir_required` | Payload contains neither `sblr` nor `engine_ir` tokens |
| `authority_check_forbidden` | Payload contains `catalog_security`, `grant_check`, or `rls_check` |
| `mga_visibility_forbidden` | Payload contains `mga_visibility` |
| `mutation_side_effect_forbidden` | Payload contains `dml_mutation`, `commit`, or `rollback` |
| `udr_call_interpreter_only` | Payload contains `udr_call` |
| `logging_interpreter_only` | Payload contains logging function references |
| `cluster_operation_forbidden_noncluster` | Payload contains `cluster` references |

Units that pass lowerability are classified as `predicate`, `projection`, or
`expression`, with lowerability reason `llvm_safe`.

If a ban fires and `allow_interpreter_fallback` is `true` and the policy does
not require native compilation, the engine silently falls back to the
interpreter. If the policy is `jit_required_for_declared_units` or
`aot_package_required`, a hard refusal is returned instead.

---

## GPU Refusal Codes

The GPU gate (`EvaluateGpuAcceleration`, verified in
`src/engine/gpu_acceleration/gpu_acceleration.cpp`) produces the following
refusal codes:

| Code | Condition |
|------|-----------|
| `GPU.AUTHORITY_BYPASS_REFUSED` | Request attempted to claim authority over security, transaction, visibility, catalog, MGA, or cluster decisions |
| `GPU.UNSAFE_PAGE_ACCESS_REFUSED` | Request attempted direct page or catalog access |
| `GPU.SBLR_ONLY_KERNELS_REQUIRED` | Raw client or parser input requested; only validated SBLR or engine-internal kernel input is permitted |
| `GPU.CLUSTER_PLACEMENT_UNAVAILABLE` | Cluster dispatch requested but cluster authority is not available |
| `GPU.SECURITY_CONTEXT_REQUIRED` | GPU control requires an engine security context |
| `GPU.DISABLED_BY_POLICY` | The active policy profile is `disabled` |
| `GPU.WORKLOAD_UNSUPPORTED` | The workload class is not in the supported set |
| `GPU.INVALID_ACCELERATION_POLICY` | Structural policy violation (e.g., mismatched vector dimensions) |
| `GPU.DEVICE_MEMORY_POLICY_VIOLATION` | Materialized batch exceeds the device or pinned-memory budget |
| `GPU.BACKEND_UNAVAILABLE` | A non-optional profile requires a provider but none is available |
| `GPU.DETERMINISM_NOT_PROVEN` | An exact workload (aggregate, columnar scan, sort) requires CPU equivalence proof |
| `GPU.RUNTIME_COMPATIBILITY_REFUSED` | Runtime compatibility negotiation failed closed |
| `GPU.RUNTIME_COMPATIBILITY_FALLBACK` | Runtime compatibility negotiation requested scalar fallback |
| `GPU.FALLBACK_USED` | CPU reference path used by GPU policy (informational, not an error) |

---

## Policy Gates and Fail-Closed Guarantee

Acceleration is disabled by default. Every tier has a policy profile enum.
Setting the profile to `disabled` (the default) prevents any acceleration
from being attempted.

When an accelerator is attempted and fails for any reason not explicitly
permitted as a fallback, the engine fails closed: it returns an error or uses
the interpreter. It never silently returns an accelerated result that could
not be verified.

The `fail_closed` field in `ScoringKernelResult` and
`NativeSblrSpecializationResult` records when the engine detected a failure
and enforced the fail-closed path.

The operator-facing consequence is straightforward: disabling acceleration
cannot change query results. It can only change execution speed. This guarantee
holds for all tiers and for all combinations of policy settings.

---

## Cross-Links

- [README.md](README.md) — guide overview
- [execution_tiers.md](execution_tiers.md) — three-tier stack, hotness, escalation
- [native_compilation.md](native_compilation.md) — LLVM policy profiles, cache key detail, AOT artifacts
- [gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md) — GPU policy profiles, refusal codes, scoring kernels
- [operating_acceleration.md](operating_acceleration.md) — operator workflow
- [../Security_Guide/trust_and_separation_architecture.md](../Security_Guide/trust_and_separation_architecture.md)
