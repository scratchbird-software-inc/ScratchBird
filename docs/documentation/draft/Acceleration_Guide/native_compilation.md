# Native Compilation

**Status: draft — subject to revision before stable release.**

## Purpose

This document describes ScratchBird's LLVM-backed native compilation layer
(JIT and AOT) and the native SBLR specialization mechanism that feeds it. It
covers policy profiles, the multi-epoch SHA-256 cache key, AOT artifact format
and location, lowerability classification and ban codes, specialization kinds
and hotness thresholds, and the SBsql management surface.

Source references verified: `src/engine/native_compile/native_compile.hpp`,
`src/engine/native_compile/native_compile.cpp`,
`src/engine/native_compile/native_sblr_specialization.hpp`,
`src/engine/sblr/sblr_opcode_registry.cpp`,
`src/parsers/sbsql_worker/lowering/lowering.cpp`,
`src/engine/internal_api/extensibility/llvm_api.hpp`.

---

## Policy Profiles

The `NativeCompilePolicyProfile` enum controls whether and how native
compilation is attempted for a given execution context. The six profiles are
(verified in `native_compile.hpp`):

| Profile name | Constant | Behavior |
|-------------|----------|----------|
| `native_compile.disabled` | `disabled` | No native compilation. Interpreter always used. |
| `native_compile.jit_optional` | `jit_optional` | JIT compilation attempted when hotness thresholds are met; interpreter fallback permitted on failure. |
| `native_compile.jit_required_for_declared_units` | `jit_required_for_declared_units` | JIT must succeed for units that request it. If backend is unavailable, returns a hard refusal. |
| `native_compile.aot_optional` | `aot_optional` | AOT and JIT are both permitted; interpreter fallback permitted on failure. |
| `native_compile.aot_package_required` | `aot_package_required` | AOT package must be available. Hard refusal if artifact cannot be written or loaded. |
| `native_compile.dev_debug_ir_export` | `dev_debug_ir_export` | Development mode; IR export permitted. Not for production use. |

The `invalid` value is an error sentinel, not a valid operating profile.

The effective mode reported in `NativeCompileResult.effective_mode` reflects
what actually ran: `interpreter`, `jit`, `aot`, or `refused`. This may differ
from the requested mode when fallback or policy override applies.

Policy profiles are selected at request time based on the `policy_profiles`
vector in `NativeCompileRequest`. The selection order (verified in
`SelectPolicy`) is:

1. `dev_debug_ir_export` (highest priority)
2. `jit_required_for_declared_units`
3. `aot_package_required`
4. `aot_optional`
5. `jit_optional` (default when no profile or `policy=default` present)
6. `disabled`

---

## Multi-Epoch SHA-256 Cache Key

Every compiled artifact is identified by a cache key of the form
`"llvm-" + SHA256(material_string)`. The material string is a concatenation
of all the following dimensions (verified in `CacheKeyMaterial` in
`native_compile.cpp`):

**SBLR content dimensions:**
- SHA-256 digest of the SBLR module payload (`sblr_hash`)
- SBLR version string (e.g., `sblr_v3`)
- Opcode registry epoch (e.g., `static_v3`)
- Target object UUID and principal UUID

**Epoch dimensions — changes to any of these invalidate the artifact:**
- Catalog generation ID (`catalog_generation_id`)
- Security epoch (`security_epoch`)
- Policy epoch (`policy_epoch`)
- Resource epoch (`resource_epoch`)

**Engine identity dimensions:**
- Engine ABI identifier (e.g., `sb_engine_abi_v3`)
- Numeric backend profile (e.g., `sbl_numeric:int128,uint128,real128`)

**Backend provenance dimensions:**
- Backend provider string and version
- LLVM load mode (static or dynamic)
- SHA-256 of LLVM library path
- SHA-256 of LLVM source root
- SHA-256 of LLVM tools root
- SHA-256 of LLVM staging build directory
- Target triple (e.g., `x86_64-scratchbird-linux`)
- Target feature set (comma-joined CPU feature flags)

**Compilation mode and structural dimensions:**
- Compile mode (`jit` or `aot`)
- Unit kind (from lowerability classification)
- Descriptor set: per-descriptor UUID, kind, canonical type name, and
  SHA-256 of encoded descriptor content
- All active policy profile strings
- All active physical profile strings
- SHA-256 of each option envelope

An artifact is considered stale when any dimension in its cache key material
differs from the current values. The function `NativeArtifactInvalidatedByDependency`
performs dependency-specific invalidation: it checks whether a given
dependency family and value appear in the cached key material string. If they
do not, the artifact is invalidated.

The engine also computes a `provenance_hash` that covers the material string
combined with LLVM source root and library path digests. This hash allows
independent verification that an AOT artifact was produced by a known compiler
installation.

---

## AOT Artifact Format and Location

When the effective mode is `aot`, the engine writes an artifact to:

```
<database_path>.sb.native_aot/<cache_key>.native_aot.meta
```

The artifact file is a plain-text metadata record (verified in `WriteAotArtifact`
in `native_compile.cpp`) containing:

```
artifact_kind=metadata_only_aot_evidence
cache_key=<cache_key>
provenance_hash=<provenance_hash>
effective_mode=<effective_mode>
backend_provider=<backend_provider>
llvm_version=<llvm_version>
llvm_load_mode=<llvm_load_mode>
llvm_library_path_digest=<sha256>
llvm_source_root_digest=<sha256>
target_triple=<target_triple>
target_feature_set=<feature_set>
unit_kind=<unit_kind>
sblr_or_ir_digest=<sha256>
descriptor_set_digest=<sha256>
cache_key_material_hash=<sha256_of_material>
```

The `artifact_kind=metadata_only_aot_evidence` field identifies this as
planning and provenance evidence, not a binary machine-code image. It is used
to determine whether a unit has been compiled before and whether the artifact
remains valid under the current multi-epoch key.

If the artifact directory does not exist, `create_directories` is called.
If the write fails and `allow_interpreter_fallback` is `true`, the engine
falls back to the interpreter rather than returning an error.

---

## Lowerability Classification and Ban Codes

Before any unit is admitted for LLVM compilation, `ClassifyLowerability` runs
on the `module_payload` and `option_envelopes` fields of the request. The
following ban codes cause refusal (verified in `native_compile.cpp`):

| Ban code | Trigger condition |
|----------|-----------------|
| `module_payload_required` | Empty payload |
| `sql_compile_forbidden` | Payload contains SQL text (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, or `sql:` prefix) |
| `parser_authority_forbidden` | Payload or options reference `parser_ast`, `parse_tree`, or `parser_authority` |
| `reference_authority_forbidden` | Payload or options reference reference authority, plan, or result |
| `protocol_or_client_authority_forbidden` | Payload or options reference `protocol_frame`, `wire_frame`, or `client_ir` |
| `engine_ir_validation_required` | Engine IR present but not marked validated via `engine_ir_validated` or `engine_ir:validated` option |
| `sblr_or_engine_ir_required` | Payload contains neither `sblr` nor `engine_ir` markers |
| `authority_check_forbidden` | Payload contains `catalog_security`, `grant_check`, or `rls_check` |
| `mga_visibility_forbidden` | Payload contains `mga_visibility` |
| `mutation_side_effect_forbidden` | Payload contains `dml_mutation`, `commit`, or `rollback` |
| `udr_call_interpreter_only` | Payload contains `udr_call` |
| `logging_interpreter_only` | Payload contains logging function references |
| `cluster_operation_forbidden_noncluster` | Payload contains `cluster` references |

Units that pass all checks are classified as `predicate`, `projection`, or
`expression` and assigned the lowerability reason `llvm_safe`.

If a ban fires on a non-required profile with `allow_interpreter_fallback`,
the engine silently uses the interpreter and records the ban code as the
`lowerability` field. On required profiles, the engine returns a hard refusal.

---

## Native SBLR Specialization

Native SBLR specialization is a lighter-weight form of native acceleration
that targets hot operation patterns within the SBLR execution path. It is
modeled separately from full LLVM compilation (verified in
`src/engine/native_compile/native_sblr_specialization.hpp`).

### Specialization Kinds

Six kinds of SBLR operations are eligible for native specialization:

| Kind constant | Description |
|---------------|-------------|
| `kPredicate` | Filter conditions (boolean expressions over rows) |
| `kProjection` | Column projection and expression evaluation |
| `kRowDecode` | Row format decoding |
| `kPathExtraction` | Path-based field extraction (e.g., from structured documents) |
| `kDistanceScoring` | Distance and similarity scoring expressions |
| `kAggregate` | Aggregate function evaluation |

An `kUnknown` value is the zero/error sentinel.

### Hotness Thresholds

Specialization is gated on observed invocation and data volume thresholds
in `NativeSblrHotness`:

- `observed_invocations` must exceed `minimum_invocations`
- `observed_rows` must exceed `minimum_rows`
- `observed_expressions` must exceed `minimum_expressions`

All three thresholds must be met before a unit becomes eligible. Thresholds
are set through the policy profile system.

### Epoch Binding

Each specialization request carries an `NativeSblrEpochBinding`:

- `security_epoch` / `expected_security_epoch`
- `redaction_epoch` / `expected_redaction_epoch`

If either observed epoch does not match the expected value, the specialization
result is discarded and the scalar reference is used instead. This ensures that
a specialization compiled under one security or redaction policy is never used
after those policies change.

### Routes

A specialization attempt produces one of three routes
(`NativeSblrSpecializationRoute`):

- `kNative` — native specialized execution was used
- `kScalarFallback` — scalar reference was used (success path with fallback)
- `kRefused` — specialization was refused (no fallback available or policy
  prevents fallback)

The `fail_closed` field in `NativeSblrSpecializationResult` records whether
the engine enforced the fail-closed path after detecting a specialization
failure.

---

## SBsql Management Surface

The following SBsql statements control and inspect the native compile subsystem.
All are verified against `lowering.cpp` and `sblr_opcode_registry.cpp`.

### Control Statements (ALTER NATIVE COMPILE)

These statements require `sysarch_authorized` security class and use the
`sblr.acceleration.llvm.v3` SBLR family.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `ALTER NATIVE COMPILE AOT REBUILD <unit_ref>` | `alter.native_compile.aot_rebuild` | `SBLR_OP_NATIVE_COMPILE_AOT_REBUILD` | `sys.native_compile.aot_units` |
| `ALTER NATIVE COMPILE ARTIFACT <ref> QUARANTINE` | `alter.native_compile.artifact_quarantine` | `SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE` | `sys.native_compile.artifacts` |
| `ALTER NATIVE COMPILE CACHE INVALIDATE` | `alter.native_compile.cache_invalidate` | `SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE` | `sys.native_compile.cache` |
| `ALTER NATIVE COMPILE PROFILE <ref> DISABLE` | `alter.native_compile.profile_disable` | `SBLR_OP_NATIVE_COMPILE_PROFILE_DISABLE` | `sys.native_compile.profiles` |
| `ALTER NATIVE COMPILE PROFILE <ref> ENABLE` | `alter.native_compile.profile_enable` | `SBLR_OP_NATIVE_COMPILE_PROFILE_ENABLE` | `sys.native_compile.profiles` |

All control statements return result shape `rs.acceleration.control.v1` and
route through `EngineControlNativeCompile`.

### Inspect Statements (SHOW)

These statements do not require a transaction context and route through
`EngineInspectNativeCompile` using the `sblr.acceleration.llvm.v3` SBLR family.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `SHOW AOT ARTIFACTS` | `show.aot_artifacts` | `SBLR_OP_SHOW_AOT_ARTIFACTS` | `sys.native_compile.artifacts` |
| `SHOW LLVM` | `show.llvm` | `SBLR_OP_SHOW_LLVM` | `sys.native_compile.llvm` |
| `SHOW LLVM PROVENANCE` | `show.llvm_provenance` | `SBLR_OP_SHOW_LLVM_PROVENANCE` | `sys.native_compile.llvm` |
| `SHOW LLVM TARGETS` | `show.llvm_targets` | `SBLR_OP_SHOW_LLVM_TARGETS` | `sys.native_compile.llvm` |
| `SHOW NATIVE COMPILE` | `show.native_compile` | `SBLR_OP_SHOW_NATIVE_COMPILE` | `sys.native_compile.runtime` |
| `SHOW NATIVE COMPILE CACHE` | `show.native_compile_cache` | `SBLR_OP_SHOW_NATIVE_COMPILE_CACHE` | `sys.native_compile.cache` |

### Opcode Registry Entries

The SBLR opcode registry (verified in `sblr_opcode_registry.cpp`) defines four
acceleration management opcodes for the LLVM tier, all with category
`management` and security class `sysarch_authorized`:

- `acceleration.llvm.compile` → `SBLR_ACCEL_LLVM_COMPILE`
- `acceleration.llvm.inspect` → `SBLR_ACCEL_LLVM_INSPECT`
- `acceleration.llvm.invalidate` → `SBLR_ACCEL_LLVM_INVALIDATE`
- `acceleration.llvm.policy_set` → `SBLR_ACCEL_LLVM_POLICY_SET`

The op-level opcodes (used by the SBsql surface above) are category
`extensibility`:

- `op.native_compile.aot_rebuild` → `SBLR_OP_NATIVE_COMPILE_AOT_REBUILD`
- `op.native_compile.artifact_quarantine` → `SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE`
- `op.native_compile.cache_invalidate` → `SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE`
- `op.native_compile.profile_disable` → `SBLR_OP_NATIVE_COMPILE_PROFILE_DISABLE`
- `op.native_compile.profile_enable` → `SBLR_OP_NATIVE_COMPILE_PROFILE_ENABLE`

### Engine Internal API

(Verified in `src/engine/internal_api/extensibility/llvm_api.hpp`.)

| Entry point | Purpose |
|-------------|---------|
| `EngineCompileLlvmModule` | Submit an SBLR or engine IR module for JIT or AOT compilation |
| `EngineControlNativeCompile` | Execute ALTER NATIVE COMPILE control statements |
| `EngineInspectNativeCompile` | Execute SHOW LLVM / SHOW AOT ARTIFACTS / SHOW NATIVE COMPILE inspect statements |

---

## Cross-Links

- [README.md](README.md) — guide overview and candidate-accelerator principle
- [acceleration_authority_model.md](acceleration_authority_model.md) — epoch binding, lowerability bans in the authority context
- [execution_tiers.md](execution_tiers.md) — where native compilation fits in the three-tier stack
- [gpu_and_simd_acceleration.md](gpu_and_simd_acceleration.md) — GPU policy profiles and scoring kernels
- [operating_acceleration.md](operating_acceleration.md) — operator workflow for managing native compile
- [../Language_Reference/syntax_reference/ebnf/acceleration_statement.md](../Language_Reference/syntax_reference/ebnf/acceleration_statement.md)
