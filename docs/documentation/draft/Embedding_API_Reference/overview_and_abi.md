# Overview and ABI

## Purpose

This chapter explains what the ScratchBird embedding API is, describes SBcore as the in-process engine library, documents the ABI version, lists every public header, and defines the distinction between public and internal surfaces. A reader finishing this chapter should understand where the embedding boundary sits and how to confirm at runtime that the library loaded is the one they compiled against.

## What the Embedding API Is

ScratchBird is a Convergent Data Engine (CDE). Its core engine — referred to here as **SBcore** — is a C-callable shared library (or static archive, depending on build configuration) that an application can link and call in-process without running a separate server process.

The embedding API is the set of C functions and C++ wrappers that an application uses to:

1. Open and close an engine handle against a database file path.
2. Begin and end sessions on that engine, with explicit user identity and trust mode.
3. Begin, commit, and roll back transactions within a session.
4. Construct and dispatch SBLR (ScratchBird Lowered Representation) execution envelopes to the engine.
5. Consume the results: row batches, command completions, execution summaries, and diagnostics.
6. Query capabilities and collect metrics.

The engine does not parse SQL text at the dispatch boundary. SQL text passed to a parser package is translation evidence only; it becomes engine authority only after the parser produces a validated SBLR envelope that is accepted through the dispatch gate. This is a documented architectural invariant (see `CORE_BETA_PUBLIC_API_ABI.md`, field `engine_execution_authority: engine_sblr_internal_api_only`).

## SBcore as the In-Process Engine Library

When an application links `ScratchBird::sb_engine` (the CMake target), it acquires the SBcore library. The CMake find-package entry point is `find_package(ScratchBirdEngine CONFIG REQUIRED)` as shown in the public consumer smoke example (`project/examples/public_engine_consumer_smoke/CMakeLists.txt`).

The engine library boundary is deliberately narrow: attach, session lifecycle, transaction lifecycle, SBLR dispatch, result consumption, capability inspection, and metrics. Cluster-positive behavior (multi-node operations) is outside core. Any operation that requires cluster authority either routes through an external cluster provider or fails closed with diagnostic code `SBLR.CLUSTER.SUPPORT_NOT_ENABLED`.

## ABI Version

The public ABI is versioned with three components encoded into a 32-bit packed value:

| Macro | Value |
| --- | --- |
| `SB_ENGINE_ABI_VERSION_MAJOR` | `1u` |
| `SB_ENGINE_ABI_VERSION_MINOR` | `0u` |
| `SB_ENGINE_ABI_VERSION_PATCH` | `0u` |
| `SB_ENGINE_ABI_VERSION_PACKED` | `((major << 16) | (minor << 8) | patch)` = `0x00010000` = 65536 |

Source: `project/include/scratchbird/engine/version.h`.

At runtime, an embedder should verify the loaded library matches the headers it compiled against by calling:

```c
uint32_t packed = sb_engine_abi_version_packed();
/* packed must equal SB_ENGINE_ABI_VERSION_PACKED from version.h */
```

A non-matching value indicates a library/header mismatch. The build ID string (an opaque UTF-8 string embedded in the build) can be retrieved via:

```c
const char* build_id = NULL;
uint64_t build_id_size = 0;
sb_engine_status_t st = sb_engine_abi_build_id(&build_id, &build_id_size);
```

Both functions are declared in `version.h` with `SCRATCHBIRD_ENGINE_EXTERN_C SCRATCHBIRD_ENGINE_API` linkage.

The ABI family name recorded in the freeze manifest is `sb_engine_public_abi`. The freeze ID is `core_beta_public_api_abi_freeze_2026_05`.

## Public Header Inventory

The following headers are part of the frozen public surface (`embedded_engine_c_abi_v1` and `embedded_engine_cpp_wrappers_v1`). All are installed under `project/include/scratchbird/engine/`.

| Header | Language | Surface |
| --- | --- | --- |
| `engine.h` | C | C ABI entry points and parameter structs |
| `error.h` | C | `sb_engine_status_t`, `sb_engine_diagnostic_severity_t`, `sb_engine_status_name()` |
| `diagnostic.h` | C | `sb_engine_string_view_t`, `sb_engine_diagnostic_view_t`, `sb_engine_diagnostic_set_view_t` |
| `version.h` | C | ABI version macros and `sb_engine_abi_version_packed()`, `sb_engine_abi_build_id()` |
| `export.h` | C | `SCRATCHBIRD_ENGINE_API`, `SCRATCHBIRD_ENGINE_CALL`, `SCRATCHBIRD_ENGINE_EXTERN_C` visibility macros |
| `engine.hpp` | C++ | `scratchbird::engine::Engine` RAII wrapper |
| `session.hpp` | C++ | `scratchbird::engine::Session` RAII wrapper |
| `transaction.hpp` | C++ | `scratchbird::engine::Transaction` RAII wrapper |
| `result.hpp` | C++ | `scratchbird::engine::Result` RAII wrapper |
| `types.hpp` | C++ | `Status`, `Uuid`, `Budget`, `StringView`, `kAbiVersionPacked` aliases |
| `descriptor.hpp` | C++ | `DescriptorFamily` enum, `Descriptor` struct |
| `execution_type_descriptor.hpp` | C++ | `ExecutionTypeFamily`, `ExecutionTypeWidthClass`, `ExecutionTypeModifierFlag`, `ExecutionTypeDescriptor` |
| `value.hpp` | C++ | `ExecutionValueState`, `ExecutionValue`, `PlainValuePayload*` encoding types |
| `sblr_envelope.hpp` | C++ | `SblrExecutionEnvelope`, `SblrOperationFamily`, `SblrBehaviorStatus`, encoding/decoding helpers |
| `sblr/lowering.hpp` | C++ | `scratchbird::engine::sblr::EnvelopeBuilder` builder API |
| `sblr/raising.hpp` | C++ | `scratchbird::engine::sblr::EnvelopeReader` reader API |

The `include/scratchbird/api/`, `include/scratchbird/diagnostics/`, `include/scratchbird/udr/`, and `include/scratchbird/wire/` subdirectories each contain only a README placeholder indicating scope (client-facing API, diagnostic rendering, C++ UDR ABI, and wire protocol respectively). Their specific headers are not part of the `embedded_engine_c_abi_v1` / `embedded_engine_cpp_wrappers_v1` frozen surfaces documented in this manual.

## What "Public" Means vs Internal

The `CORE_BETA_PUBLIC_API_ABI_MANIFEST.json` freeze document (status: `frozen_for_core_beta_qa`) defines exactly two classification tiers relevant to embedders:

- **`core`**: Frozen symbol set, ABI version tracked, removal requires a major-version gate. Covers `embedded_engine_c_abi_v1` (C ABI) and `embedded_engine_cpp_wrappers_v1` (C++ thin wrappers).
- **`core_driver_tool_adaptor`** / **`core_parser_package`** / **`non_core_cluster_boundary`**: Adjacent surfaces not part of the direct embedding path. Drivers do not own transaction finality; parser packages produce translation evidence; cluster provider failures are fail-closed.

Internal symbols — anything not in the frozen header list — are not API and may change without notice across any version.

## C ABI Symbol Set

The complete frozen C ABI symbol set (from `CORE_BETA_PUBLIC_API_ABI.md`) is:

| Symbol | Purpose |
| --- | --- |
| `sb_engine_abi_build_id` | Retrieve opaque build ID string |
| `sb_engine_abi_version_packed` | Retrieve packed ABI version as `uint32_t` |
| `sb_engine_close` | Close an engine handle |
| `sb_engine_describe_capabilities` | Query engine capability report |
| `sb_engine_dispatch_sblr` | Submit an SBLR envelope for execution |
| `sb_engine_metric_root` | Query the engine metrics tree |
| `sb_engine_open` | Open an engine handle against a database path |
| `sb_engine_result_class` | Retrieve the class (kind) of a result handle |
| `sb_engine_result_completion` | Retrieve command completion view from a result |
| `sb_engine_result_diagnostics` | Retrieve diagnostic set from a result |
| `sb_engine_result_next_batch` | Advance to and consume the next row batch |
| `sb_engine_result_payload` | Retrieve raw payload bytes from a result |
| `sb_engine_result_release` | Release a result handle |
| `sb_engine_result_summary` | Retrieve execution summary from a result |
| `sb_engine_session_begin` | Begin a new session on an engine handle |
| `sb_engine_session_end` | End a session |
| `sb_engine_status_name` | Return the string name of a status code |
| `sb_engine_transaction_begin` | Begin a transaction within a session |
| `sb_engine_transaction_commit` | Commit a transaction |
| `sb_engine_transaction_rollback` | Roll back a transaction |

All symbols are declared with `SCRATCHBIRD_ENGINE_API SCRATCHBIRD_ENGINE_CALL` linkage attributes defined in `export.h`. On POSIX platforms with shared-library builds, `SCRATCHBIRD_ENGINE_API` expands to `__attribute__((visibility("default")))`. On Windows, it expands to `__declspec(dllexport)` or `__declspec(dllimport)` depending on whether `SCRATCHBIRD_ENGINE_BUILDING_SHARED` or `SCRATCHBIRD_ENGINE_USING_SHARED` is defined. Static builds define neither.
