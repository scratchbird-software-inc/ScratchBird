---
title: "ScratchBird — Application Development and Integration"
---

# ScratchBird — Application Development and Integration

*ScratchBird documentation — draft*

## Who this book is for

Application developers, driver users, and integration engineers.

## About this book

This volume is for building on ScratchBird: the embedding API and frozen ABI, the client drivers and integration adaptors, and the AI / MCP integration layer.

## Parts in this volume

- **Embedding and API Reference**
- **Client and Driver Guide**
- **AI Integration Guide**

> This is a **draft**. See *About This Documentation* at the end of this book for
> status and license. Confirm any specific behavior against the current build.

\newpage



# Embedding and API Reference




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/README.md -->

<a id="ch-embedding-api-reference-readme-md"></a>

# ScratchBird Embedding and API Reference

This manual documents the public API surface that embedders use to link ScratchBird's core engine library (SBcore) directly into an application process. It is the reference for everyone who:

- links `ScratchBird::sb_engine` as a CMake target and calls the C ABI functions,
- uses the thin C++ RAII wrappers in `scratchbird::engine`,
- constructs and submits SBLR execution envelopes through the engine dispatch path, or
- needs to understand the ABI versioning contract and stability policy.

This manual does not cover server-mode operation, the wire protocol, driver packages, or SBsql syntax. For those topics see the cross-links below.

## Documented Invariant

**The engine executes SBLR/internal authority only. SQL text is not runtime authority inside the engine.** SQL text provided to a parser package is translation evidence; it becomes runtime authority only after a validated SBLR envelope is accepted and dispatched through the engine API and security gates. This invariant is stated in `CORE_BETA_PUBLIC_API_ABI.md` (`engine_sblr_internal_api_only`) and is enforced throughout all public surfaces.

## ABI/Versioning Summary

| Attribute | Value |
| --- | --- |
| ABI family | `sb_engine_public_abi` |
| Version | `1.0.0` |
| Packed macro | `SB_ENGINE_ABI_VERSION_PACKED` = `0x00010000` (65536) |
| Source header | `project/include/scratchbird/engine/version.h` |
| Runtime version function | `sb_engine_abi_version_packed()` |
| Build ID function | `sb_engine_abi_build_id()` |
| Freeze ID | `core_beta_public_api_abi_freeze_2026_05` |

## Directory Map

| Chapter | Purpose |
| --- | --- |
| [Overview and ABI](#ch-embedding-api-reference-overview-and-abi-md) | What the embedding API is, SBcore as the in-process library, ABI version, header inventory, and public-vs-internal distinctions. |
| [Lifecycle: Engine, Session, Transaction](#ch-embedding-api-reference-lifecycle-engine-session-transaction-md) | The object/lifecycle model: engine handle, session, transaction. Construction, teardown, ownership, and the C++ RAII wrappers. |
| [Types, Descriptors, and Values](#ch-embedding-api-reference-types-descriptors-and-values-md) | The canonical type model from `descriptor.hpp`, `execution_type_descriptor.hpp`, `types.hpp`, and `value.hpp`. |
| [Results and Cursors](#ch-embedding-api-reference-results-and-cursors-md) | How `result.hpp` returns and surfaces result data, row batches, command completion, and execution summaries. |
| [Diagnostics and Errors](#ch-embedding-api-reference-diagnostics-and-errors-md) | Error codes, diagnostic severity, the diagnostic set view, and how refusals surface at the API boundary. |
| [SBLR Envelope](#ch-embedding-api-reference-sblr-envelope-md) | The SBLR request envelope as the engine-facing execution representation: encoding, dispatch, and the Priority D registry. |
| [Compatibility and Stability Policy](#ch-embedding-api-reference-compatibility-and-stability-policy-md) | What is stable, what may change, how ABI version is checked at runtime, and the removal gate. |
| [Examples](#ch-embedding-api-reference-examples-md) | Walk-through of the real programs and scripts in `project/examples/`. |

## Reading Model

If you are integrating SBcore for the first time, read [Overview and ABI](#ch-embedding-api-reference-overview-and-abi-md) to orient yourself, then [Lifecycle: Engine, Session, Transaction](#ch-embedding-api-reference-lifecycle-engine-session-transaction-md) to understand the object model, then [SBLR Envelope](#ch-embedding-api-reference-sblr-envelope-md) to understand how work is submitted. The [Examples](#ch-embedding-api-reference-examples-md) chapter shows the real end-to-end C smoke program alongside the QA shell scripts.

If you are auditing ABI stability, go directly to [Compatibility and Stability Policy](#ch-embedding-api-reference-compatibility-and-stability-policy-md).

## Cross-Links

- Embedded engine operating mode: ../Getting_Started/operating_modes/embedded_engine.md (ScratchBird — Concepts and Getting Started, page XXX)
- SBsql language reference: ../Language_Reference/README.md (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Operations and Administration Guide: ../Operations_Administration/README.md (ScratchBird — Operations, Security, and Autonomy, page XXX)

## Draft Status

This is a draft manual. Every function name, type name, constant, and enum value has been verified against the public header files listed in `CORE_BETA_PUBLIC_API_ABI_MANIFEST.json`. Claims that could not be verified from source have been omitted and are noted in individual chapters. No performance promises, production-readiness claims, or compatibility promises beyond what the freeze document explicitly states are made here.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/overview_and_abi.md -->

<a id="ch-embedding-api-reference-overview-and-abi-md"></a>

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




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/lifecycle_engine_session_transaction.md -->

<a id="ch-embedding-api-reference-lifecycle-engine-session-transaction-md"></a>

# Lifecycle: Engine, Session, Transaction

## Purpose

This chapter documents the three-level object model that forms the foundation of every embedding interaction: an **engine handle** opens and holds a database, a **session** identifies a caller with a trust mode and user identity, and a **transaction** scopes atomic work within a session. Each level has its own creation parameters, teardown function, and ownership rules. The C++ RAII wrappers in the `scratchbird::engine` namespace manage these objects safely; the C ABI is always the underlying authority.

The engine executes SBLR/internal authority only. SQL text is not runtime authority at any of these levels.

## Object Hierarchy

```
sb_engine_handle_t  (engine)
  └── sb_engine_session_t  (session, one or more per engine)
        └── sb_engine_transaction_t  (transaction, zero or one per session at a time)
```

All three handle types are opaque pointer typedefs declared in `engine.h`:

```c
typedef struct sb_engine_handle_s*      sb_engine_handle_t;
typedef struct sb_engine_session_s*     sb_engine_session_t;
typedef struct sb_engine_transaction_s* sb_engine_transaction_t;
typedef struct sb_engine_result_s*      sb_engine_result_t;
```

## Open Mode

Every engine handle is opened in one of four modes, selected by `sb_engine_open_mode_t`:

| Enumerator | Numeric | Meaning |
| --- | --- | --- |
| `SB_ENGINE_OPEN_NORMAL` | 0 | Standard read-write open |
| `SB_ENGINE_OPEN_READ_ONLY` | 1 | No write operations admitted |
| `SB_ENGINE_OPEN_MAINTENANCE` | 2 | Maintenance mode; ordinary attaches blocked |
| `SB_ENGINE_OPEN_VALIDATION_ONLY` | 3 | Validate and inspect; no persistent writes |

Source: `engine.h`.

## Trust Mode

Sessions carry a trust mode that governs which operations the engine will admit:

| Enumerator | Numeric | Meaning |
| --- | --- | --- |
| `SB_ENGINE_TRUST_SERVER_ISOLATED` | 0 | IPC-server or isolated path; full policy gate |
| `SB_ENGINE_TRUST_EMBEDDED_TRUSTED` | 1 | Trusted embedded caller |
| `SB_ENGINE_TRUST_PARSER_UNTRUSTED` | 2 | Untrusted parser input path |

Source: `engine.h`.

## Opening an Engine Handle

```c
sb_engine_status_t sb_engine_open(
    const sb_engine_open_params_v1_t* params,
    sb_engine_handle_t* out_engine,
    sb_engine_result_t* out_result);
```

**`sb_engine_open_params_v1_t`** fields (from `engine.h`):

| Field | Type | Notes |
| --- | --- | --- |
| `struct_size` | `uint32_t` | Must be `sizeof(sb_engine_open_params_v1_t)` |
| `abi_version` | `uint32_t` | Must be `SB_ENGINE_ABI_VERSION_PACKED` |
| `database_path_utf8` | `const char*` | UTF-8 database file path (may be NULL for validation-only) |
| `database_path_size` | `uint64_t` | Byte length of `database_path_utf8` |
| `mode` | `sb_engine_open_mode_t` | One of the four open modes above |
| `reserved_mode` | `uint8_t` | Must be zero |
| `reserved_bytes[7]` | `uint8_t[7]` | Must be zero |
| `resource_defaults` | `sb_engine_budget_v1_t` | Default resource budget for this engine |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

The pattern of including `struct_size` and `abi_version` in every versioned parameter struct allows the engine to detect version mismatches and reject stale callers cleanly. This pattern applies to all `_v1_t` parameter types.

Always zero the struct before filling fields (as the C smoke example demonstrates with `memset`), then set `struct_size` and `abi_version`.

**Closing** the engine handle:

```c
sb_engine_status_t sb_engine_close(sb_engine_handle_t engine,
                                   sb_engine_result_t* out_result);
```

`out_result` may be NULL if the caller does not need diagnostic output from close.

## Resource Budget

**`sb_engine_budget_v1_t`** (from `engine.h`) lets the embedder impose resource limits per engine default or per request context:

| Field | Type | Meaning |
| --- | --- | --- |
| `deadline_unix_ms` | `uint64_t` | Absolute deadline in Unix milliseconds (0 = no deadline) |
| `monotonic_timeout_ms` | `uint64_t` | Monotonic timeout in milliseconds (0 = no timeout) |
| `cpu_units` | `uint64_t` | CPU budget (0 = no limit) |
| `memory_bytes` | `uint64_t` | Memory budget in bytes (0 = no limit) |
| `temporary_bytes` | `uint64_t` | Temporary space budget (0 = no limit) |
| `output_rows` | `uint64_t` | Maximum output rows (0 = no limit) |
| `output_bytes` | `uint64_t` | Maximum output bytes (0 = no limit) |
| `compile_budget_units` | `uint64_t` | Compilation budget (0 = no limit) |
| `recursion_depth` | `uint32_t` | Maximum recursion depth (0 = no limit) |
| `opcode_count` | `uint32_t` | Maximum opcode execution count (0 = no limit) |

## Beginning a Session

```c
sb_engine_status_t sb_engine_session_begin(
    sb_engine_handle_t engine,
    const sb_engine_session_params_v1_t* params,
    sb_engine_session_t* out_session,
    sb_engine_result_t* out_result);
```

**`sb_engine_session_params_v1_t`** fields (from `engine.h`):

| Field | Type | Notes |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_session_params_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `effective_user_uuid` | `sb_engine_uuid_t` | Identity of the calling principal |
| `session_uuid` | `sb_engine_uuid_t` | Unique identity for this session |
| `default_language_utf8` | `const char*` | BCP-47 language tag (e.g. `"en"`) |
| `default_language_size` | `uint64_t` | Byte length of the language tag |
| `trust_mode` | `sb_engine_trust_mode_t` | One of the three trust modes above |
| `flags` | `uint32_t` | Reserved; must be zero |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

**`sb_engine_uuid_t`** is a 16-byte struct (`uint8_t bytes[16]`) defined in `engine.h`. The byte layout is application-controlled; the engine treats it as an opaque identity.

**Ending** a session:

```c
sb_engine_status_t sb_engine_session_end(
    sb_engine_session_t session,
    const sb_engine_session_end_params_v1_t* params,
    sb_engine_result_t* out_result);
```

**`sb_engine_session_end_params_v1_t`** fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_session_end_params_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `rollback_active_transactions` | `uint8_t` | 1 = roll back any active transaction before ending |
| `cancel_open_results` | `uint8_t` | 1 = cancel and release any open result handles |
| `reserved_bytes[6]` | `uint8_t[6]` | Must be zero |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

The `params` argument to `sb_engine_session_end` may be NULL; the engine applies default teardown behavior in that case. The C++ `Session::reset()` always provides explicit params with `rollback_active_transactions = 1` and `cancel_open_results = 1`.

## Transactions

```c
sb_engine_status_t sb_engine_transaction_begin(
    sb_engine_session_t session,
    const sb_engine_transaction_params_v1_t* params,
    sb_engine_transaction_t* out_transaction,
    sb_engine_result_t* out_result);

sb_engine_status_t sb_engine_transaction_commit(
    sb_engine_transaction_t transaction,
    const sb_engine_transaction_finish_params_v1_t* params,
    sb_engine_result_t* out_result);

sb_engine_status_t sb_engine_transaction_rollback(
    sb_engine_transaction_t transaction,
    const sb_engine_transaction_finish_params_v1_t* params,
    sb_engine_result_t* out_result);
```

**`sb_engine_transaction_params_v1_t`** fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_transaction_params_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `isolation_level` | `uint32_t` | Isolation level (values not enumerated in public headers) |
| `access_mode` | `uint32_t` | Access mode (values not enumerated in public headers) |
| `autocommit_mode` | `uint32_t` | Autocommit mode (values not enumerated in public headers) |
| `flags` | `uint32_t` | Reserved |
| `timeout_ms` | `uint64_t` | Transaction timeout (0 = no timeout) |
| `idle_timeout_ms` | `uint64_t` | Idle timeout (0 = no idle timeout) |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

Note: The specific numeric values for `isolation_level`, `access_mode`, and `autocommit_mode` are not enumerated in the frozen public headers. The C smoke program (`project/examples/public_engine_consumer_smoke/main.c`) does not exercise these fields; they are zero-initialized. Consult the engine capability report or source-tree internal documentation for enumerated values.

**`sb_engine_transaction_finish_params_v1_t`** fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_transaction_finish_params_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `timeout_ms` | `uint64_t` | Commit/rollback timeout (0 = no timeout) |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

A transaction is passed as an argument to `sb_engine_dispatch_sblr`. Passing NULL for the transaction is valid for operations that do not require explicit transaction scope (the engine interprets this per the request context).

## C++ RAII Wrappers

Three header-only C++ classes in `scratchbird::engine` wrap the C handles and call teardown automatically on destruction:

### `scratchbird::engine::Engine` (`engine.hpp`)

- Wraps `sb_engine_handle_t`.
- Non-copyable, move-only.
- Destructor calls `sb_engine_close(handle_, nullptr)`.
- Static factory method: `Engine::open(params, out, result)` — calls `sb_engine_open` and populates `out` on success.
- `get()` returns the raw handle; `operator bool()` tests for non-null.

### `scratchbird::engine::Session` (`session.hpp`)

- Wraps `sb_engine_session_t`.
- Non-copyable, move-only.
- Destructor calls `sb_engine_session_end` with `rollback_active_transactions = 1` and `cancel_open_results = 1`.
- No factory method; callers construct directly from a raw `sb_engine_session_t` obtained from `sb_engine_session_begin`.

### `scratchbird::engine::Transaction` (`transaction.hpp`)

- Wraps `sb_engine_transaction_t`.
- Non-copyable, move-only.
- Destructor calls `rollback()`, which calls `sb_engine_transaction_rollback`.
- `commit()` calls `sb_engine_transaction_commit` and sets the handle to null on success.
- `rollback()` calls `sb_engine_transaction_rollback` and sets the handle to null on success.

### Supporting Types (`types.hpp`)

`types.hpp` defines aliases and one inline helper used across the C++ layer:

```cpp
using Status    = sb_engine_status_t;
using Uuid      = sb_engine_uuid_t;
using Budget    = sb_engine_budget_v1_t;
using StringView = std::string_view;

constexpr std::uint32_t kAbiVersionPacked = SB_ENGINE_ABI_VERSION_PACKED;

inline StringView to_string_view(sb_engine_string_view_t value) noexcept;
```

## Thread Safety

No thread-safety claims are documented in the public headers. No thread-safety invariants are stated. Callers should treat engine, session, and transaction handles as not thread-safe unless and until the engine documents otherwise.

## Ownership Rules

- `sb_engine_open` transfers ownership of the engine handle to the caller. The caller must call `sb_engine_close`.
- `sb_engine_session_begin` transfers ownership of the session handle to the caller. The caller must call `sb_engine_session_end`.
- `sb_engine_transaction_begin` transfers ownership of the transaction handle to the caller. The caller must call either `sb_engine_transaction_commit` or `sb_engine_transaction_rollback`.
- `sb_engine_dispatch_sblr` and other functions that produce a `sb_engine_result_t` transfer ownership of that result handle to the caller. The caller must call `sb_engine_result_release`.

The C++ RAII wrappers enforce all of the above automatically.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/types_descriptors_and_values.md -->

<a id="ch-embedding-api-reference-types-descriptors-and-values-md"></a>

# Types, Descriptors, and Values

## Purpose

This chapter documents the type model exposed by the embedding API's C++ headers. The engine represents every storable and computable datum through a combination of a **descriptor** (which type family a value belongs to and its full type parameterization) and an **execution value** (the actual byte payload together with its state). Understanding these structures is a prerequisite for interpreting result batches and for constructing typed parameters in SBLR envelopes.

The engine executes SBLR/internal authority only. SQL text is not runtime authority; type identities are resolved and carried by descriptors, not by SQL type names at dispatch time.

## Descriptor Family (`descriptor.hpp`)

`DescriptorFamily` is an enum class in `scratchbird::engine` that names the broad category of a type:

```cpp
enum class DescriptorFamily : std::uint32_t {
  unknown          = 0,
  null_value       = 1,
  boolean          = 2,
  integer          = 3,
  unsigned_integer = 4,
  decimal          = 5,
  real             = 6,
  text             = 7,
  binary           = 8,
  temporal         = 9,
  uuid             = 10,
  json_document    = 11,
  vector           = 12,
  graph            = 13,
  domain           = 14
};
```

The `Descriptor` struct pairs a family with its canonical byte encoding:

```cpp
struct Descriptor {
  DescriptorFamily family = DescriptorFamily::unknown;
  std::vector<std::uint8_t> canonical_bytes;
};
```

`canonical_bytes` carries the type's full parameterization in canonical form. Its internal encoding is not specified in the public headers; it is produced by the engine and consumed by the engine.

## Execution Type Descriptor (`execution_type_descriptor.hpp`)

`ExecutionTypeDescriptor` is a richer, fully-resolved type description used at execution time. It is defined in `scratchbird::engine` and carries all modifier fields that affect how a value is stored, compared, or displayed.

### `ExecutionTypeFamily`

```cpp
enum class ExecutionTypeFamily : std::uint16_t {
  null_type        = 0,
  boolean          = 1,
  signed_integer   = 2,
  unsigned_integer = 3,
  real             = 4,
  decimal          = 5,
  uuid             = 6,
  character        = 7,
  binary           = 8,
  bit_string       = 9,
  temporal         = 10,
  blob             = 11,
  network          = 12,
  document         = 13,
  search           = 14,
  structured       = 15,
  range            = 16,
  spatial          = 17,
  vector           = 18,
  graph            = 19,
  time_series      = 20,
  columnar         = 21,
  aggregate_state  = 22,
  sketch           = 23,
  locator          = 24,
  opaque           = 25,
  result_set       = 26,
  unknown          = 0xffffu
};
```

`ExecutionTypeFamily` is more detailed than `DescriptorFamily`: it distinguishes `signed_integer` from `unsigned_integer`, adds `bit_string`, `blob`, `network`, `structured`, `range`, `spatial`, `time_series`, `columnar`, `aggregate_state`, `sketch`, `locator`, `opaque`, and `result_set` families.

### `ExecutionTypeWidthClass`

```cpp
enum class ExecutionTypeWidthClass : std::uint16_t {
  fixed               = 0,
  variable            = 1,
  descriptor_defined  = 2,
  unknown             = 0xffffu
};
```

### `ExecutionTypeModifierFlag`

```cpp
enum class ExecutionTypeModifierFlag : std::uint64_t {
  precision              = 1ull << 0,
  scale                  = 1ull << 1,
  length                 = 1ull << 2,
  charset_uuid           = 1ull << 3,
  collation_uuid         = 1ull << 4,
  timezone_uuid          = 1ull << 5,
  domain_uuid            = 1ull << 6,
  domain_stack           = 1ull << 7,
  element_descriptor_uuid = 1ull << 8,
  vector_dimensions      = 1ull << 9,
  container_rank         = 1ull << 10,
  security_policy_uuid   = 1ull << 11
};
```

These flags indicate which modifier fields in `ExecutionTypeDescriptor` carry meaningful values for a given type.

### `ExecutionTypeDescriptor`

```cpp
struct ExecutionTypeDescriptor {
  Uuid            descriptor_uuid{};
  uint64_t        descriptor_epoch = 0;
  uint32_t        canonical_type_id = 0;
  ExecutionTypeFamily  family = ExecutionTypeFamily::unknown;
  ExecutionTypeWidthClass width_class = ExecutionTypeWidthClass::unknown;
  std::string     stable_name;
  uint32_t        bit_width = 0;
  uint32_t        precision = 0;
  uint32_t        scale = 0;
  uint32_t        length = 0;
  uint32_t        vector_dimensions = 0;
  uint32_t        container_rank = 0;
  uint64_t        modifier_flags = 0;
  Uuid            domain_uuid{};
  std::vector<Uuid> domain_stack;
  Uuid            charset_uuid{};
  Uuid            collation_uuid{};
  Uuid            timezone_uuid{};
  Uuid            element_descriptor_uuid{};
  Uuid            security_policy_uuid{};
  bool            nullable_allowed = true;
  bool            descriptor_authoritative = true;
  bool            parser_independent = true;
};
```

Key fields:

| Field | Meaning |
| --- | --- |
| `descriptor_uuid` | Stable UUID identifying this type descriptor in the catalog |
| `descriptor_epoch` | Version epoch; increments when the descriptor definition changes |
| `canonical_type_id` | Numeric catalog type ID |
| `family` | The `ExecutionTypeFamily` value |
| `width_class` | Whether values have fixed, variable, or descriptor-defined width |
| `stable_name` | Human-readable type name |
| `modifier_flags` | Bitmask of `ExecutionTypeModifierFlag` indicating active modifier fields |
| `parser_independent` | True when the type identity is not affected by the parser package in use |
| `descriptor_authoritative` | True when this descriptor is the authoritative source (not a hint) |

UUID-valued modifier fields (`charset_uuid`, `collation_uuid`, `timezone_uuid`, `domain_uuid`, `element_descriptor_uuid`, `security_policy_uuid`) carry the zero UUID when the corresponding `modifier_flags` bit is not set.

## Value State and Execution Values (`value.hpp`)

### `ExecutionValueState`

Every execution value carries a state that determines how the payload is interpreted:

```cpp
enum class ExecutionValueState : std::uint8_t {
  value            = 0,  // Normal value with payload
  sql_null         = 1,  // SQL NULL (no payload)
  missing          = 2,  // Column absent from row (no payload)
  default_requested = 3, // Caller requests engine default (no payload)
  unknown          = 4,  // State unknown (no payload)
  error            = 5,  // Value-level error (payload contains error info)
  lob_handle       = 6,  // Large object handle (payload is a LOB locator)
  protected_value  = 7   // Protected/redacted value (payload present but restricted)
};
```

Two inline helper predicates:

```cpp
constexpr bool ExecutionValueStateIsSqlNull(ExecutionValueState state) noexcept;
constexpr bool ExecutionValueStateHasPayload(ExecutionValueState state) noexcept;
```

`ExecutionValueStateHasPayload` returns true for `value`, `error`, `lob_handle`, and `protected_value`. It returns false for `sql_null`, `missing`, `default_requested`, and `unknown`.

### `ExecutionValue`

```cpp
struct ExecutionValue {
  Descriptor                   descriptor;
  bool                         is_null = true;
  std::vector<std::uint8_t>    encoded_value;
  ExecutionValueState          state = ExecutionValueState::sql_null;

  bool isSqlNull() const noexcept;
  bool isNull() const noexcept;
  bool hasPayload() const noexcept;
  void setState(ExecutionValueState new_state) noexcept;
};
```

`isSqlNull()` returns true if `state == sql_null` or if `state == value && is_null`.
`isNull()` is an alias for `isSqlNull()`.
`hasPayload()` returns true when the state allows payload and the value is not SQL null.
`setState()` updates state and synchronizes `is_null` accordingly.

### Plain Value Payload Encoding (`value.hpp`)

The `PlainValuePayload` wire encoding is used to serialize `ExecutionValue` instances. The encoding uses a 16-byte header with a fixed magic sequence:

| Constant | Value |
| --- | --- |
| `kPlainValuePayloadMagic0` | `'S'` (0x53) |
| `kPlainValuePayloadMagic1` | `'B'` (0x42) |
| `kPlainValuePayloadMagic2` | `'C'` (0x43) |
| `kPlainValuePayloadMagic3` | `'1'` (0x31) |
| `kPlainValuePayloadMajorVersion` | `1` |
| `kPlainValuePayloadMinorVersion` | `0` |
| `kPlainValuePayloadHeaderSize` | `16` |

`PlainValuePayloadStatus` enumerates parse outcomes:

| Enumerator | Meaning |
| --- | --- |
| `ok` | Successful parse |
| `truncated_header` | Buffer shorter than 16 bytes |
| `invalid_magic` | Magic bytes do not match `SBC1` |
| `unsupported_version` | Version bytes not recognized |
| `invalid_reserved` | Reserved field not zero |
| `invalid_state` | State code not a valid `ExecutionValueState` |
| `payload_length_mismatch` | Declared payload length does not match buffer |
| `payload_not_allowed` | State does not allow payload but payload bytes present |
| `payload_length_overflow` | Payload length overflows address space |

`PlainValuePayload` holds a decoded execution value payload:

```cpp
struct PlainValuePayload {
  ExecutionValueState          state = ExecutionValueState::sql_null;
  std::vector<std::uint8_t>    payload;
};
```

The encode and decode functions for `PlainValuePayload` are present in `value.hpp` but the full encode/decode function signatures span beyond the first 150 lines verified in this review. The magic, version constants, and status enum above are the publicly anchored constants.

## Type Aliases (`types.hpp`)

For convenience when writing C++ embedding code, `types.hpp` exports aliases into `scratchbird::engine`:

```cpp
using Status     = sb_engine_status_t;
using Uuid       = sb_engine_uuid_t;
using Budget     = sb_engine_budget_v1_t;
using StringView = std::string_view;

constexpr std::uint32_t kAbiVersionPacked = SB_ENGINE_ABI_VERSION_PACKED;
```

The `to_string_view` inline converts a C-ABI `sb_engine_string_view_t` to `std::string_view`, returning an empty view when the data pointer is null.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/results_and_cursors.md -->

<a id="ch-embedding-api-reference-results-and-cursors-md"></a>

# Results and Cursors

## Purpose

This chapter documents how the engine returns work product to an embedder. Every call that produces output — dispatch, capability query, metrics root — returns a `sb_engine_result_t` handle. The caller consumes it by first inspecting its class, then calling the appropriate view functions, and finally releasing the handle. For row-producing operations the caller iterates batches until the end-of-stream marker is set.

The engine executes SBLR/internal authority only. SQL text is not runtime authority; the result stream reflects executed engine work, not parsed SQL text.

## The Result Handle

```c
typedef struct sb_engine_result_s* sb_engine_result_t;
```

Declared in `engine.h`. The result handle is an opaque owned object. Ownership transfers to the caller on every function that produces one. The caller must call `sb_engine_result_release` exactly once.

## Result Class

Before calling any view function, the caller should retrieve the result's class:

```c
sb_engine_status_t sb_engine_result_class(
    sb_engine_result_t result,
    sb_engine_result_class_t* out_class);
```

`sb_engine_result_class_t` (from `engine.h`):

| Enumerator | Numeric | Meaning |
| --- | --- | --- |
| `SB_ENGINE_RESULT_NONE` | 0 | Empty or not yet populated result |
| `SB_ENGINE_RESULT_ROW_BATCH` | 1 | Row-producing result; iterate with `sb_engine_result_next_batch` |
| `SB_ENGINE_RESULT_COMMAND_COMPLETION` | 2 | Non-row command completed; see `sb_engine_result_completion` |
| `SB_ENGINE_RESULT_EXECUTION_SUMMARY` | 3 | Execution metrics summary; see `sb_engine_result_summary` |
| `SB_ENGINE_RESULT_DIAGNOSTIC_ONLY` | 4 | Only diagnostics; no row or completion data |
| `SB_ENGINE_RESULT_CAPABILITY_REPORT` | 5 | Capability probe response; use `sb_engine_result_payload` |
| `SB_ENGINE_RESULT_METRIC_ROOT` | 6 | Metrics tree root; use `sb_engine_result_payload` |

The class determines which view functions are meaningful.

## Releasing a Result

```c
sb_engine_status_t sb_engine_result_release(sb_engine_result_t result);
```

Must be called once per result handle. Returns `SB_ENGINE_STATUS_ALREADY_RELEASED` if called on an already-released handle.

## Command Completion View

For `SB_ENGINE_RESULT_COMMAND_COMPLETION`:

```c
sb_engine_status_t sb_engine_result_completion(
    sb_engine_result_t result,
    sb_engine_command_completion_view_v1_t* out_view);
```

**`sb_engine_command_completion_view_v1_t`** (from `engine.h`):

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_command_completion_view_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `operation_id` | `sb_engine_string_view_t` | Symbolic operation identifier (engine-assigned) |
| `affected_rows` | `uint64_t` | Number of rows affected by the command |
| `reserved0`, `reserved1` | `uint64_t` | Reserved |

## Execution Summary View

For `SB_ENGINE_RESULT_EXECUTION_SUMMARY`:

```c
sb_engine_status_t sb_engine_result_summary(
    sb_engine_result_t result,
    sb_engine_execution_summary_view_v1_t* out_view);
```

**`sb_engine_execution_summary_view_v1_t`** (from `engine.h`):

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_execution_summary_view_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `elapsed_us` | `uint64_t` | Elapsed microseconds |
| `rows_produced` | `uint64_t` | Total rows produced across all batches |
| `diagnostics_count` | `uint64_t` | Number of diagnostics attached to this execution |
| `reserved0`, `reserved1` | `uint64_t` | Reserved |

## Diagnostics on a Result

Any result class may carry attached diagnostics:

```c
sb_engine_status_t sb_engine_result_diagnostics(
    sb_engine_result_t result,
    sb_engine_diagnostic_set_view_t* out_view);
```

See [Diagnostics and Errors](#ch-embedding-api-reference-diagnostics-and-errors-md) for the `sb_engine_diagnostic_set_view_t` structure. The `out_view` is valid for the lifetime of the result handle.

## Payload View

For `SB_ENGINE_RESULT_CAPABILITY_REPORT`, `SB_ENGINE_RESULT_METRIC_ROOT`, and similar non-row payload results:

```c
sb_engine_status_t sb_engine_result_payload(
    sb_engine_result_t result,
    sb_engine_string_view_t* out_view);
```

Returns a `sb_engine_string_view_t` (a `const char* data` / `uint64_t size_bytes` pair). The payload bytes are valid for the lifetime of the result handle and must not be written to. The encoding of payload bytes (e.g., JSON, binary, or a structured format) depends on the result class and the request that produced it.

The C smoke program (`project/examples/public_engine_consumer_smoke/main.c`) demonstrates checking payload content after a capability probe:

```c
sb_engine_string_view_t payload;
memset(&payload, 0, sizeof(payload));
sb_engine_result_payload(result, &payload);
/* payload.data and payload.size_bytes are populated */
/* The smoke test searches the payload for "capability probe" */
```

## Row Batches

For `SB_ENGINE_RESULT_ROW_BATCH`, the caller iterates until `end_of_stream` is set:

```c
sb_engine_status_t sb_engine_result_next_batch(
    sb_engine_result_t result,
    const sb_engine_batch_request_v1_t* request,
    sb_engine_row_batch_view_v1_t* out_batch);
```

**`sb_engine_batch_request_v1_t`** (from `engine.h`):

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_batch_request_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `max_rows` | `uint64_t` | Maximum rows to return in this batch (0 = no limit) |
| `max_bytes` | `uint64_t` | Maximum bytes to return in this batch (0 = no limit) |
| `timeout_ms` | `uint64_t` | Per-batch wait timeout (0 = no timeout) |
| `reserved0`, `reserved1` | `uint64_t` | Reserved |

**`sb_engine_row_batch_view_v1_t`** (from `engine.h`):

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_row_batch_view_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `row_count` | `uint64_t` | Number of rows in this batch |
| `end_of_stream` | `uint8_t` | Non-zero when no more batches will follow |
| `reserved_bytes[7]` | `uint8_t[7]` | Reserved |
| `reserved0`, `reserved1` | `uint64_t` | Reserved |

The caller should loop calling `sb_engine_result_next_batch` until `out_batch.end_of_stream` is non-zero. An empty batch with `end_of_stream` set indicates the stream is exhausted.

Note: `sb_engine_row_batch_view_v1_t` provides row count and stream state, but individual row/column values are not directly accessible through public C ABI functions exposed in `engine.h`. Row value access through the public headers is handled via the C++ `ExecutionValue` / `PlainValuePayload` encoding described in [Types, Descriptors, and Values](#ch-embedding-api-reference-types-descriptors-and-values-md). The precise mechanism by which batched rows are accessed from `out_batch` is not fully enumerated in the frozen public C headers; the batch view provides count and stream metadata.

## Capability Request and Metrics

Two additional result-producing functions query the engine:

```c
sb_engine_status_t sb_engine_describe_capabilities(
    sb_engine_handle_t engine,
    const sb_engine_capability_request_v1_t* request,
    sb_engine_result_t* out_result);

sb_engine_status_t sb_engine_metric_root(
    sb_engine_handle_t engine,
    const sb_engine_metric_request_v1_t* request,
    sb_engine_result_t* out_result);
```

**`sb_engine_capability_request_v1_t`**:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_capability_request_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `flags` | `uint32_t` | Reserved |
| `reserved_flags` | `uint32_t` | Reserved |
| `reserved0`, `reserved1` | `uint64_t` | Reserved |

**`sb_engine_metric_request_v1_t`**:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_metric_request_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `root_path_utf8` | `const char*` | UTF-8 root path to query (may be NULL for engine root) |
| `root_path_size` | `uint64_t` | Byte length of `root_path_utf8` |
| `flags` | `uint32_t` | Reserved |
| `reserved_flags` | `uint32_t` | Reserved |
| `reserved0`, `reserved1` | `uint64_t` | Reserved |

Both produce results with class `SB_ENGINE_RESULT_CAPABILITY_REPORT` and `SB_ENGINE_RESULT_METRIC_ROOT` respectively, accessible via `sb_engine_result_payload`.

## The C++ `Result` Wrapper (`result.hpp`)

`scratchbird::engine::Result` wraps `sb_engine_result_t`:

- Non-copyable, move-only.
- Destructor calls `sb_engine_result_release`.
- `result_class()` calls `sb_engine_result_class` and returns the class.
- `payload()` calls `sb_engine_result_payload` and returns `std::string_view`. Returns an empty view if the data pointer is null.
- `get()` returns the raw handle; `operator bool()` tests for non-null.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/diagnostics_and_errors.md -->

<a id="ch-embedding-api-reference-diagnostics-and-errors-md"></a>

# Diagnostics and Errors

## Purpose

This chapter documents how the embedding API reports errors and engine-side diagnostics. ScratchBird uses two distinct but related mechanisms: a **status code** (`sb_engine_status_t`) returned by every C ABI call, and a **diagnostic set** (`sb_engine_diagnostic_set_view_t`) attached to result handles. A status code indicates whether the API call itself succeeded; the diagnostic set contains human-readable and machine-readable detail about what happened inside the engine.

The `include/scratchbird/diagnostics/README.md` header directory placeholder states its scope as "stable diagnostic structures and public diagnostic rendering boundaries only." No additional diagnostic headers beyond those in `include/scratchbird/engine/` are part of the frozen `embedded_engine_c_abi_v1` surface.

## Status Codes (`error.h`)

Every C ABI function returns `sb_engine_status_t`. It is defined in `error.h`:

```c
typedef enum sb_engine_status_t {
  SB_ENGINE_STATUS_OK                = 0,
  SB_ENGINE_STATUS_INVALID_ARGUMENT  = 1,
  SB_ENGINE_STATUS_INVALID_HANDLE    = 2,
  SB_ENGINE_STATUS_UNSUPPORTED       = 3,
  SB_ENGINE_STATUS_CAPABILITY_DISABLED = 4,
  SB_ENGINE_STATUS_SECURITY_DENIED   = 5,
  SB_ENGINE_STATUS_TRANSACTION_ACTIVE = 6,
  SB_ENGINE_STATUS_TRANSACTION_REQUIRED = 7,
  SB_ENGINE_STATUS_CONFLICT          = 8,
  SB_ENGINE_STATUS_NOT_FOUND         = 9,
  SB_ENGINE_STATUS_TIMEOUT           = 10,
  SB_ENGINE_STATUS_RESOURCE_EXHAUSTED = 11,
  SB_ENGINE_STATUS_INTERNAL_ERROR    = 12,
  SB_ENGINE_STATUS_ALREADY_RELEASED  = 13
} sb_engine_status_t;
```

| Code | Meaning |
| --- | --- |
| `SB_ENGINE_STATUS_OK` | Call succeeded |
| `SB_ENGINE_STATUS_INVALID_ARGUMENT` | A parameter failed validation (e.g., struct_size mismatch, null pointer where disallowed) |
| `SB_ENGINE_STATUS_INVALID_HANDLE` | A handle argument was NULL or already released |
| `SB_ENGINE_STATUS_UNSUPPORTED` | The requested operation or feature is not supported in this build |
| `SB_ENGINE_STATUS_CAPABILITY_DISABLED` | The requested capability is available but disabled by policy or configuration |
| `SB_ENGINE_STATUS_SECURITY_DENIED` | The session's trust mode, rights set, or policy gate denied the operation |
| `SB_ENGINE_STATUS_TRANSACTION_ACTIVE` | An operation was attempted that requires no active transaction, but one exists |
| `SB_ENGINE_STATUS_TRANSACTION_REQUIRED` | An operation was attempted that requires a transaction, but none is active |
| `SB_ENGINE_STATUS_CONFLICT` | A conflict (e.g., write conflict, concurrent modification) prevented completion |
| `SB_ENGINE_STATUS_NOT_FOUND` | A referenced object does not exist |
| `SB_ENGINE_STATUS_TIMEOUT` | The operation did not complete within the configured timeout |
| `SB_ENGINE_STATUS_RESOURCE_EXHAUSTED` | A resource budget (memory, CPU, output rows, etc.) was exceeded |
| `SB_ENGINE_STATUS_INTERNAL_ERROR` | The engine encountered an internal error condition |
| `SB_ENGINE_STATUS_ALREADY_RELEASED` | `sb_engine_result_release` was called on an already-released result handle |

### Retrieving the Status Name

```c
SCRATCHBIRD_ENGINE_API const char* SCRATCHBIRD_ENGINE_CALL
sb_engine_status_name(sb_engine_status_t status);
```

Returns a null-terminated C string with the symbolic name of the status code (e.g., `"SB_ENGINE_STATUS_OK"`). The returned pointer points to static storage and must not be freed.

## Diagnostic Severity (`error.h`)

Individual diagnostics carry a severity:

```c
typedef enum sb_engine_diagnostic_severity_t {
  SB_ENGINE_DIAGNOSTIC_INFO    = 0,
  SB_ENGINE_DIAGNOSTIC_WARNING = 1,
  SB_ENGINE_DIAGNOSTIC_ERROR   = 2
} sb_engine_diagnostic_severity_t;
```

## Diagnostic Structures (`diagnostic.h`)

### `sb_engine_string_view_t`

A non-owning view of a byte buffer returned from the engine:

```c
typedef struct sb_engine_string_view_t {
  const char* data;
  uint64_t    size_bytes;
} sb_engine_string_view_t;
```

`data` may be NULL if the engine has no content for the field. Always check for NULL before using `data`. The bytes pointed to are valid for the lifetime of the enclosing result or diagnostic set handle.

### `sb_engine_diagnostic_view_t`

An individual diagnostic entry:

```c
typedef struct sb_engine_diagnostic_view_t {
  uint32_t                       struct_size;
  uint32_t                       abi_version;
  uint32_t                       numeric_code;
  sb_engine_diagnostic_severity_t severity;
  sb_engine_string_view_t        symbolic_code;
  sb_engine_string_view_t        message_key;
  sb_engine_string_view_t        safe_detail;
  uint64_t                       reserved0;
  uint64_t                       reserved1;
} sb_engine_diagnostic_view_t;
```

| Field | Meaning |
| --- | --- |
| `numeric_code` | Numeric diagnostic code |
| `severity` | `SB_ENGINE_DIAGNOSTIC_INFO`, `_WARNING`, or `_ERROR` |
| `symbolic_code` | Symbolic code string (e.g., `"SBLR.EXECUTION.ADMISSION_ONLY"`) — stable across versions per the compatibility policy |
| `message_key` | Localization key for a human-readable message (e.g., `"sblr.execution.admission_only"`) |
| `safe_detail` | Engine-provided detail text that has passed the redaction gate; safe to log without sanitization |

`safe_detail` is text the engine considers safe to surface. Other internal detail strings are not exposed at this boundary. The stability policy (`CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md`) states that diagnostic shape, message keys, redaction class, and compatibility status must remain stable.

### `sb_engine_diagnostic_set_view_t`

A view of the full diagnostic set attached to a result:

```c
typedef struct sb_engine_diagnostic_set_view_t {
  uint32_t                          struct_size;
  uint32_t                          abi_version;
  const sb_engine_diagnostic_view_t* diagnostics;
  uint64_t                          diagnostic_count;
  uint64_t                          reserved0;
  uint64_t                          reserved1;
} sb_engine_diagnostic_set_view_t;
```

`diagnostics` points to an array of `diagnostic_count` entries. The array is valid for the lifetime of the result handle it was retrieved from. An empty diagnostic set has `diagnostic_count == 0`; `diagnostics` may be NULL in that case.

To retrieve the diagnostic set from a result:

```c
sb_engine_status_t sb_engine_result_diagnostics(
    sb_engine_result_t result,
    sb_engine_diagnostic_set_view_t* out_view);
```

## How Refusals Surface

When the engine refuses a request — due to security policy, capability unavailability, budget exhaustion, or SBLR admission failure — the primary signal is the `sb_engine_status_t` return value. The diagnostic set attached to the result provides additional detail.

Common refusal diagnostic codes observed in the source:

| Symbolic Code | Condition |
| --- | --- |
| `SBLR.EXECUTION.ADMISSION_ONLY` | Operation family dispatched but execution not yet implemented (Priority D admission-only status) |
| `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` | Cluster-positive operation attempted in a non-cluster build |
| `SBLR.CAPABILITY.FORBIDDEN` | Operation family requires a capability that is not enabled |
| `SBLR.ENVELOPE.INVALID` | Envelope byte stream malformed or too short |
| `SBLR.ENVELOPE.CHECKSUM_INVALID` | Envelope checksum failed verification |
| `SBLR.VERSION.UNSUPPORTED` | Envelope version_major is not 1 |
| `SBLR.OPCODE.UNKNOWN` | No Priority D registry row matched the family/opcode pair |
| `SBLR.OPCODE.REFERENCE_META_FORBIDDEN` | Operation family `reference_meta` is forbidden at the dispatch boundary |
| `SBLR.DESCRIPTOR.INVALID` | A descriptor in the envelope is malformed or has a zero kind field |

These symbolic codes appear as `symbolic_code` strings in `sb_engine_diagnostic_view_t.symbolic_code` and as diagnostic codes in `SblrDecodedEnvelope.diagnostic_code` (see [SBLR Envelope](#ch-embedding-api-reference-sblr-envelope-md)).

## Admission-Only Operations

In the current Core Beta surface, all operation families in the Priority D registry are marked `admission_only`. This means the engine accepts and validates envelopes for these operation families but does not execute them; the engine returns `SBLR.EXECUTION.ADMISSION_ONLY` as the diagnostic code. This is a documented beta behavior, not an error in the caller's usage. See [SBLR Envelope](#ch-embedding-api-reference-sblr-envelope-md) for the full Priority D registry.

## Checking Status in Practice

Every call to a C ABI function should check its return value before using output arguments:

```c
sb_engine_status_t st = sb_engine_open(&params, &engine, &result);
if (st != SB_ENGINE_STATUS_OK) {
    /* engine is 0, result may contain diagnostics */
    return;
}
```

For the C++ wrappers, `Status` is an alias for `sb_engine_status_t`:

```cpp
using Status = sb_engine_status_t;
/* Status SB_ENGINE_STATUS_OK = 0 */
```

`Transaction::commit()` and `Transaction::rollback()` return `Status` directly.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/sblr_envelope.md -->

<a id="ch-embedding-api-reference-sblr-envelope-md"></a>

# SBLR Envelope

## Purpose

This chapter documents the SBLR (ScratchBird Lowered Representation) execution envelope — the binary format that an embedder constructs and submits to the engine via `sb_engine_dispatch_sblr`. The SBLR envelope is the engine-facing execution representation: all work that the engine executes arrives in this form. SQL text is parser input only; the parser produces an SBLR envelope and the engine dispatches that, not the original text. This invariant applies equally to direct embedders who construct envelopes themselves.

All types in this chapter are from `scratchbird/engine/sblr_envelope.hpp` and `scratchbird/engine/sblr/lowering.hpp` unless noted.

## Dispatch Function

```c
sb_engine_status_t sb_engine_dispatch_sblr(
    sb_engine_session_t session,
    sb_engine_transaction_t transaction,
    const sb_engine_request_context_v1_t* context,
    const sb_engine_sblr_dispatch_params_v1_t* params,
    sb_engine_result_t* out_result);
```

`transaction` may be NULL for operations that do not require an explicit transaction. `context` and `params` must be non-NULL and correctly populated.

### Request Context (`engine.h`)

**`sb_engine_request_context_v1_t`** carries the per-request security and capability context:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_request_context_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `effective_user_uuid` | `sb_engine_uuid_t` | Effective user identity for this request |
| `session_uuid` | `sb_engine_uuid_t` | Session UUID (should match the session handle) |
| `parser_package_uuid` | `sb_engine_uuid_t` | UUID of the parser package that produced the envelope (zero if none) |
| `dialect_profile_uuid` | `sb_engine_uuid_t` | UUID of the dialect profile (zero if none) |
| `trust_mode` | `sb_engine_trust_mode_t` | Trust mode for this request |
| `flags` | `uint32_t` | Reserved; zero |
| `rights_set_ref` | `uint64_t` | Opaque reference to the rights/privilege set |
| `capability_set_ref` | `uint64_t` | Opaque reference to the capability set |
| `source_artifact_set_ref` | `uint64_t` | Opaque reference to source artifacts |
| `transaction_ref` | `uint64_t` | Opaque transaction reference (distinct from the handle) |
| `resource_budget` | `sb_engine_budget_v1_t` | Per-request resource budget |
| `idempotency_key[32]` | `uint8_t[32]` | Optional idempotency key (all-zero = no key) |
| `idempotency_key_size` | `uint64_t` | Meaningful bytes in `idempotency_key` |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

### Dispatch Parameters (`engine.h`)

**`sb_engine_sblr_dispatch_params_v1_t`** supplies the encoded envelope bytes:

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | `sizeof(sb_engine_sblr_dispatch_params_v1_t)` |
| `abi_version` | `uint32_t` | `SB_ENGINE_ABI_VERSION_PACKED` |
| `envelope_bytes` | `const uint8_t*` | Pointer to the encoded envelope (may be NULL for capability probe) |
| `envelope_size_bytes` | `uint64_t` | Byte length of `envelope_bytes` |
| `result_contract` | `uint32_t` | Reserved; zero |
| `flags` | `uint32_t` | Reserved; zero |
| `reserved0`, `reserved1` | `uint64_t` | Must be zero |

## Envelope Wire Format

The SBLR envelope has a fixed 32-byte header (`kSblrEnvelopeHeaderSize = 32`) followed by optional descriptor entries and a canonical payload. The maximum envelope size is 16 MiB (`kSblrMaxEnvelopeBytes = 16 * 1024 * 1024`).

### Header Layout

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic: `0x524c4253` (`"SBLR"` in little-endian) |
| 4 | 2 | `version_major` (must be 1) |
| 6 | 2 | `version_minor` |
| 8 | 2 | `payload_kind` (`SblrPayloadKind`) |
| 10 | 2 | `family` (`SblrOperationFamily`) |
| 12 | 2 | `opcode` |
| 14 | 2 | Descriptor count |
| 16 | 4 | `flags` |
| 20 | 4 | Canonical payload size (bytes) |
| 24 | 4 | Checksum (FNV-1a 32-bit over envelope with this field zeroed) |
| 28 | 4 | Reserved (must be zero) |

After the header: zero or more descriptor entries, each with an 8-byte sub-header (2-byte kind, 2-byte flags, 4-byte payload size) followed by payload bytes. After all descriptors: the canonical payload bytes.

Constants from `sblr_envelope.hpp`:

```cpp
inline constexpr uint32_t kSblrEnvelopeMagic      = 0x524c4253u;
inline constexpr uint32_t kSblrEnvelopeHeaderSize = 32u;
inline constexpr uint64_t kSblrMaxEnvelopeBytes   = 16ull * 1024ull * 1024ull;
```

## Operation Family and Opcode

`SblrOperationFamily` classifies the kind of work in the envelope:

| Enumerator | Numeric | Family Name |
| --- | --- | --- |
| `relational_query` | 10 | `sblr.query.relational.v3` |
| `dml_insert` | 20 | `sblr.dml.insert.v3` |
| `dml_update` | 21 | `sblr.dml.update.v3` |
| `dml_delete` | 22 | `sblr.dml.delete.v3` |
| `dml_merge` | 23 | `sblr.dml.merge.v3` |
| `catalog_mutation` | 30 | `sblr.catalog.mutation.v3` |
| `security_mutation` | 40 | `sblr.security.mutation.v3` |
| `transaction_control` | 50 | `sblr.transaction.control.v3` |
| `bulk_import` | 60 | `sblr.bulk.import.v3` |
| `bulk_export` | 61 | `sblr.bulk.export.v3` |
| `management_inspect` | 70 | `sblr.management.inspect.v3` |
| `management_control` | 71 | `sblr.management.control.v3` |
| `metrics_inspect` | 80 | `sblr.metrics.inspect.v3` |
| `replication_operation` | 90 | `sblr.replication.operation.v3` |
| `structured_kv` | 100 | `sblr.query.kv.v3` |
| `document` | 101 | `sblr.query.document.v3` |
| `graph` | 102 | `sblr.query.graph.v3` |
| `search` | 103 | `sblr.query.search.v3` |
| `vector` | 104 | `sblr.query.vector.v3` |
| `timeseries` | 105 | `sblr.query.timeseries.v3` |
| `versioned_history` | 110 | `sblr.versioned_history.v3` |
| `cluster_placement` | 120 | `sblr.cluster.placement.v3` |
| `acceleration_management` | 130 | `sblr.acceleration.management.v3` |
| `reference_meta` | 65000 | Forbidden at dispatch boundary |

## Payload Kind

```cpp
enum class SblrPayloadKind : std::uint16_t {
  opcode_stream      = 1,
  operation_envelope = 2,
};
```

The default for constructed envelopes is `operation_envelope = 2`.

## Priority D Registry and Behavior Status

The Priority D registry (`kSblrPriorityDRegistry`) is a compile-time table that maps `(family, opcode_min..opcode_max)` to a `SblrBehaviorStatus`. The engine decoder uses this table to classify every decoded envelope.

`SblrBehaviorStatus` values:

| Enumerator | Meaning |
| --- | --- |
| `implemented` | Fully implemented execution path |
| `admission_only` | Envelope accepted and admitted, but not executed (Core Beta status for all standard families) |
| `noncluster_fail_closed` | Requires cluster; fails closed with `SBLR.CAPABILITY.FORBIDDEN` in non-cluster builds |
| `edition_fail_closed` | Requires a specific edition; fails closed |
| `capability_fail_closed` | Requires a disabled capability; fails closed with `SBLR.CAPABILITY.FORBIDDEN` |
| `deferred_to_successor` | Deferred to a future operation |
| `unsupported` | Not supported |

In the current Core Beta freeze, all standard operation families (opcodes 1–499) are `admission_only`. `replication_operation` and `cluster_placement` are `noncluster_fail_closed`. `acceleration_management` is `capability_fail_closed`. `reference_meta` is always `unsupported` and is explicitly rejected by the decoder.

## Codec Status

`SblrCodecStatus` enumerates decode outcomes:

| Enumerator | Diagnostic Code |
| --- | --- |
| `ok` | (no error) |
| `envelope_invalid` | `SBLR.ENVELOPE.INVALID` |
| `envelope_truncated` | `SBLR.ENVELOPE.INVALID` |
| `checksum_invalid` | `SBLR.ENVELOPE.CHECKSUM_INVALID` |
| `version_unsupported` | `SBLR.VERSION.UNSUPPORTED` |
| `opcode_unknown` | `SBLR.OPCODE.UNKNOWN` |
| `reference_meta_forbidden` | `SBLR.OPCODE.REFERENCE_META_FORBIDDEN` |
| `descriptor_invalid` | `SBLR.DESCRIPTOR.INVALID` |

## Envelope Data Structures

```cpp
struct SblrDescriptor {
  uint16_t                     kind = 1;
  uint16_t                     flags = 0;
  std::vector<uint8_t>         payload;
};

struct SblrSourceArtifact {
  uint16_t                     kind = 1;
  std::string                  value;
};

struct SblrExecutionEnvelope {
  uint32_t                     version_major = 1;
  uint32_t                     version_minor = 0;
  SblrPayloadKind              payload_kind = SblrPayloadKind::operation_envelope;
  SblrOperationFamily          family = SblrOperationFamily::relational_query;
  uint16_t                     opcode = 1;
  uint32_t                     flags = 0;
  std::vector<SblrDescriptor>  descriptors;
  std::vector<SblrSourceArtifact> source_artifacts;
  std::vector<uint8_t>         canonical_bytes;
};

struct SblrDecodedEnvelope {
  SblrCodecStatus              status = SblrCodecStatus::ok;
  SblrExecutionEnvelope        envelope;
  std::string_view             diagnostic_code;
  std::string_view             message_key;
};
```

## Encoding and Decoding

Two free functions in `sblr_envelope.hpp`:

```cpp
// Encode an SblrExecutionEnvelope into wire bytes
std::vector<uint8_t> EncodeSblrEnvelope(const SblrExecutionEnvelope& envelope);

// Decode wire bytes into an SblrDecodedEnvelope
SblrDecodedEnvelope DecodeSblrEnvelopeBytes(const uint8_t* data, uint64_t size);
```

The checksum uses FNV-1a 32-bit with the checksum field zeroed during computation. The result is never zero (if FNV produces zero, the stored checksum is 1).

## EnvelopeBuilder (`sblr/lowering.hpp`)

`scratchbird::engine::sblr::EnvelopeBuilder` provides a fluent builder interface:

```cpp
std::vector<uint8_t> bytes =
    EnvelopeBuilder{}
        .operation(SblrOperationFamily::relational_query, 1)
        .payload_kind(SblrPayloadKind::operation_envelope)
        .version(1, 0)
        .flags(0)
        .descriptor(kind, data, size, flags)
        .append_bytes(canonical_data, canonical_size)
        .encode();
```

Methods:

| Method | Returns | Effect |
| --- | --- | --- |
| `operation(family, opcode)` | `EnvelopeBuilder&` | Set operation family and opcode |
| `payload_kind(kind)` | `EnvelopeBuilder&` | Set payload kind |
| `version(major, minor)` | `EnvelopeBuilder&` | Set envelope version |
| `flags(flags)` | `EnvelopeBuilder&` | Set envelope flags |
| `descriptor(kind, data, size, flags)` | `EnvelopeBuilder&` | Append a descriptor entry |
| `source_artifact(kind, value)` | `EnvelopeBuilder&` | Append a source artifact |
| `append_bytes(data, size)` | `EnvelopeBuilder&` | Append to canonical_bytes |
| `finish()` | `SblrExecutionEnvelope` | Return the assembled envelope struct |
| `encode()` | `std::vector<uint8_t>` | Encode to wire bytes via `EncodeSblrEnvelope` |

## EnvelopeReader (`sblr/raising.hpp`)

`scratchbird::engine::sblr::EnvelopeReader` provides read access to a decoded envelope:

```cpp
EnvelopeReader reader(decoded_envelope.envelope);
SblrOperationFamily f = reader.family();
uint16_t op           = reader.opcode();
SblrPayloadKind pk    = reader.payload_kind();
uint64_t desc_count   = reader.descriptor_count();
SblrBehaviorStatus bs = reader.behavior_status();

// Static factory from raw bytes:
SblrDecodedEnvelope result = EnvelopeReader::decode(data, size);
```

`behavior_status()` looks up the (family, opcode) pair in the Priority D registry and returns the registered behavior status.

## Dispatch Pattern

The C smoke program (`project/examples/public_engine_consumer_smoke/main.c`) demonstrates a minimal capability-probe dispatch with a zeroed dispatch params struct (no envelope bytes). For real operations, the embedder would:

1. Construct an `SblrExecutionEnvelope` with the appropriate family and opcode.
2. Call `EncodeSblrEnvelope` (or `EnvelopeBuilder::encode()`) to get wire bytes.
3. Set `dispatch.envelope_bytes` and `dispatch.envelope_size_bytes`.
4. Call `sb_engine_dispatch_sblr` with appropriate session, transaction, and context.
5. Check the returned `sb_engine_result_t` class and consume accordingly.

In the Core Beta, all dispatched operation families return `admission_only` behavior; the engine validates and admits the envelope but returns a diagnostic rather than executing the request.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/compatibility_and_stability_policy.md -->

<a id="ch-embedding-api-reference-compatibility-and-stability-policy-md"></a>

# Compatibility and Stability Policy

## Purpose

This chapter summarizes the `CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md` and freeze invariants relevant to an embedder. It answers: which surfaces are stable, which may change, how to verify at runtime that the library version matches the headers, and what the removal gate requires.

The source authority for all claims in this chapter is `project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md` and `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json`. The policy document's own authority is stated as `public_release_evidence_only`.

## Semantic Versioning Rules

The public ABI surface is versioned with `SB_ENGINE_ABI_VERSION_MAJOR`, `SB_ENGINE_ABI_VERSION_MINOR`, and `SB_ENGINE_ABI_VERSION_PATCH`, packed into `SB_ENGINE_ABI_VERSION_PACKED`. The rules from the policy:

| Version increment | Permitted changes |
| --- | --- |
| **Major** | May remove or break public ABI symbols, but only after a recorded removal gate exists |
| **Minor** | May add compatible public headers, symbols, providers, extension points, config keys, diagnostics, and policy-pack schema rows |
| **Patch** | Must not add, remove, or reorder public ABI symbols |

A deprecation requires three things before removal is permitted: a stable diagnostic or manifest row marking the deprecation, a replacement path, and at least one release cycle between deprecation and removal.

## Frozen ABI Version

| Attribute | Value |
| --- | --- |
| Family | `sb_engine_public_abi` |
| Major | 1 |
| Minor | 0 |
| Patch | 0 |
| Packed (decimal) | 65536 |
| Packed (hex) | `0x00010000` |
| Macro | `SB_ENGINE_ABI_VERSION_PACKED` |
| Freeze ID | `core_beta_public_api_abi_freeze_2026_05` |
| Freeze status | `frozen_for_core_beta_qa` |

## Compatibility Surfaces

The policy defines eight compatibility surfaces with their governing CTest gate and public evidence anchor:

| Surface | Rule | Evidence anchor |
| --- | --- | --- |
| Public headers | Header inventory must match `public_headers` in the manifest | `sb_engine_public_headers_api_docs_freeze_gate`; `PUBLIC_API_ABI_SURFACE` marker in each header |
| C API symbols | Symbol inventory must match `c_api_symbols` in the manifest | `sb_engine_public_abi_symbol_gate`; `sb_engine_abi_version_packed` |
| Provider ABI | Cluster provider and extension boundaries must stay provider-gated and fail closed without external authority | `public_cluster_provider_handshake_gate`; `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` |
| File format | Version metadata and upgrade gates must refuse unsupported or downgraded format transitions | `public_release_version_metadata_gate`; `public_upgrade_migration_gate` |
| Diagnostics | Diagnostic shape, message keys, redaction class, and compatibility status must stay stable | `public_diagnostic_stability_gate`; `PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR` |
| Config | Public secure defaults and config schema checks must remain fail-closed | `public_default_config_check`; `PUBLIC_DEFAULT_CONFIG_CHECK` |
| Policy pack schema | Default policy-pack schema versions and catalog import/mutation gates must remain compatible | `public_policy_pack_manifest_gate`; `public_policy_pack_catalog_import_gate` |
| Extension points | Parser package, UDR runtime, cluster provider, metrics, agents, and manager boundaries must keep contract versions | `cluster_provider.v1`; `sb_parser_package_v3`; `sb_udr_v1` |

## What Is Stable for Embedders

For an embedder using only the frozen public C and C++ surfaces:

**Stable (may not be removed without a major-version removal gate):**
- All 20 C ABI symbols listed in `CORE_BETA_PUBLIC_API_ABI.md`
- All 16 public header files in `project/include/scratchbird/engine/`
- All enum enumerators in `error.h`, `diagnostic.h`, `engine.h`, `execution_type_descriptor.hpp`, `descriptor.hpp`, `value.hpp`, and `sblr_envelope.hpp`
- All struct field layouts in `_v1_t` parameter structs (fields are not reordered; reserved fields stay reserved)
- The ABI version macro values (changing `SB_ENGINE_ABI_VERSION_PACKED` is a break)
- Diagnostic symbolic codes cited in this manual (e.g., `SBLR.EXECUTION.ADMISSION_ONLY`, `SBLR.CLUSTER.SUPPORT_NOT_ENABLED`)

**May be extended (minor version, additive only):**
- New public headers may be added to the install set
- New C ABI symbols may be added
- New enum enumerators may be added to extensible enums
- New `_v2_t` or later versioned parameter structs may be added

**May change (minor or patch):**
- The content of the `sb_engine_abi_build_id` string (it is opaque and version-specific)
- The byte encoding of `PlainValuePayload` canonical bytes and `ExecutionTypeDescriptor` canonical bytes, since the internal format is not specified in the public headers
- Engine-side behavior under `admission_only` operation families (these are not execution behavior claims)

## Runtime ABI Verification

Every embedder should verify the loaded library version matches the headers used at compile time before calling any other function:

```c
#include "scratchbird/engine/version.h"

uint32_t runtime = sb_engine_abi_version_packed();
if (runtime != SB_ENGINE_ABI_VERSION_PACKED) {
    /* version mismatch: do not proceed */
}
```

This check is performed by the public consumer smoke example (`project/examples/public_engine_consumer_smoke/main.c`, line 40).

The build ID string provides additional disambiguation when the version is correct but the precise build matters:

```c
const char* build_id = NULL;
uint64_t build_id_size = 0;
sb_engine_abi_build_id(&build_id, &build_id_size);
```

## Architectural Invariants

The manifest records these invariants as machine-checked (non-negotiable for any build claiming the frozen surface):

| Invariant | Value |
| --- | --- |
| Engine execution authority | `engine_sblr_internal_api_only` |
| SQL text as runtime authority | `false` |
| UUID identity as authoritative | `true` |
| MGA finality as authoritative | `true` |
| Cluster-positive behavior in core | `false` |

These are not documentation conventions; they are architecture-level properties enforced by the engine and verified by the CTest gate suite. An embedder can rely on these invariants holding for any library that passes the freeze gate.

## Removal Gate

From the policy:

> Removal of a public header, C ABI symbol, provider contract, diagnostic code, config key, policy-pack schema row, or extension boundary is **forbidden** for the current Core Beta ABI version.

A future removal requires all of:
1. A major-version gate (incrementing `SB_ENGINE_ABI_VERSION_MAJOR`)
2. Explicit deprecation evidence (a manifest row or stable diagnostic marking the item deprecated)
3. A documented replacement path
4. Public tests proving old clients fail closed with stable diagnostics

An embedder does not need to guard against removal of any currently frozen symbol for the lifetime of the `1.0.x` ABI version family.

## Extension Boundary Contract Versions

For embedders who also implement extension boundaries (not typical for pure embedders):

| Boundary | Type | Contract version | Core classification |
| --- | --- | --- | --- |
| `parser_package.sbsql_v3` | Parser package | `sb_parser_package_v3` | core |
| `udr_package.trusted_cpp_v1` | UDR package | `sb_udr_v1` | core |
| `cluster_provider.v1` | Cluster provider | `sb_cluster_provider_v1` | non_core_cluster |
| `cluster_metrics.v1` | Cluster metrics | `sb_cluster_metrics_v1` | non_core_cluster |
| `cluster_agents.v1` | Cluster agents | `sb_cluster_agents_v1` | non_core_cluster |
| `cluster_manager.v1` | Cluster manager | `sb_cluster_manager_v1` | non_core_cluster |

Non-cluster boundaries fail closed with their respective diagnostic codes when a cluster provider is absent.




===== FILE SEPARATION =====

<!-- chapter source: Embedding_API_Reference/examples.md -->

<a id="ch-embedding-api-reference-examples-md"></a>

# Examples

## Purpose

This chapter walks through the real programs and scripts in `project/examples/`. There are two example packs: `public_engine_consumer_smoke` (a standalone C program that links against the public embedding API) and `core_beta_qa` (three shell scripts that drive built CTest fixtures). A third directory, `public_smoke_suite`, contains a JSON manifest describing staged CTest operations but no directly runnable scripts.

All examples share the invariant stated in the `core_beta_qa` manifest: engine execution remains SBLR/internal API based; SQL text is parser/client input only and is not runtime authority.

## Pack 1: `public_engine_consumer_smoke`

**Files:**
- `project/examples/public_engine_consumer_smoke/main.c`
- `project/examples/public_engine_consumer_smoke/CMakeLists.txt`

### CMake Entry Point

```cmake
find_package(ScratchBirdEngine CONFIG REQUIRED)

add_executable(scratchbird_public_engine_consumer_smoke main.c)
target_link_libraries(scratchbird_public_engine_consumer_smoke
    PRIVATE ScratchBird::sb_engine)
```

This is the canonical pattern for embedding: `find_package(ScratchBirdEngine CONFIG REQUIRED)` and link `ScratchBird::sb_engine`. The program is pure C and includes only `scratchbird/engine/engine.h`.

### `main.c` Walk-Through

The program demonstrates the full open → session → dispatch → result → close sequence entirely through the C ABI.

**Step 1: ABI version check.**

```c
if (sb_engine_abi_version_packed() != SB_ENGINE_ABI_VERSION_PACKED) {
    return 1;
}
```

The first thing any embedder should do is verify the runtime library version matches the compile-time header version. A mismatch returns 1 and the program does not proceed.

**Step 2: Build ID retrieval.**

```c
const char* build_id = 0;
uint64_t build_id_size = 0;
if (sb_engine_abi_build_id(&build_id, &build_id_size) != SB_ENGINE_STATUS_OK ||
    build_id == 0 || build_id_size == 0) {
    return 2;
}
```

Confirms the build ID is populated and non-empty. The content is opaque; the program only checks it is present.

**Step 3: Open engine in validation-only mode.**

```c
sb_engine_open_params_v1_t open_params;
memset(&open_params, 0, sizeof(open_params));
open_params.struct_size = sizeof(open_params);
open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;

sb_engine_handle_t engine = 0;
sb_engine_result_t result = 0;
if (sb_engine_open(&open_params, &engine, &result) != SB_ENGINE_STATUS_OK ||
    engine == 0) {
    return 3;
}
```

Note the `memset` to zero before populating — this ensures reserved fields are zero. `SB_ENGINE_OPEN_VALIDATION_ONLY` means no database path is required and no persistent writes occur. This is the correct mode for a smoke test.

**Step 4: Begin a session.**

```c
sb_engine_session_params_v1_t session_params;
memset(&session_params, 0, sizeof(session_params));
session_params.struct_size = sizeof(session_params);
session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
session_params.effective_user_uuid = uuid_with_tail(1);
session_params.session_uuid = uuid_with_tail(2);
session_params.default_language_utf8 = "en";
session_params.default_language_size = 2;
session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;

sb_engine_session_t session = 0;
if (sb_engine_session_begin(engine, &session_params, &session, &result) !=
        SB_ENGINE_STATUS_OK || session == 0) {
    (void)sb_engine_close(engine, 0);
    return 4;
}
```

The helper `uuid_with_tail(uint8_t tail)` constructs an `sb_engine_uuid_t` with `bytes[0] = 0x01`, `bytes[6] = 0x70`, and `bytes[15] = tail` (all others zero). This illustrates that UUIDs are application-constructed byte arrays; the engine does not generate them at session-begin time.

**Step 5: Construct a request context and dispatch (capability probe).**

```c
sb_engine_request_context_v1_t context;
memset(&context, 0, sizeof(context));
context.struct_size = sizeof(context);
context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
context.effective_user_uuid = session_params.effective_user_uuid;
context.session_uuid = session_params.session_uuid;
context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
context.rights_set_ref = 1;
context.capability_set_ref = 1;

sb_engine_sblr_dispatch_params_v1_t dispatch;
memset(&dispatch, 0, sizeof(dispatch));
dispatch.struct_size = sizeof(dispatch);
dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
/* envelope_bytes is NULL, envelope_size_bytes is 0 */

if (sb_engine_dispatch_sblr(session, 0, &context, &dispatch, &result) !=
        SB_ENGINE_STATUS_OK || result == 0) {
    (void)sb_engine_session_end(session, 0, 0);
    (void)sb_engine_close(engine, 0);
    return 5;
}
```

With `envelope_bytes = NULL` and `envelope_size_bytes = 0`, the dispatch acts as a capability probe. The transaction argument is `0` (NULL) since no transaction is needed. The `rights_set_ref = 1` and `capability_set_ref = 1` are minimal opaque references for the validation-only mode.

**Step 6: Check result class.**

```c
sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK ||
    result_class != SB_ENGINE_RESULT_CAPABILITY_REPORT) {
    return 6;
}
```

A successful capability probe returns `SB_ENGINE_RESULT_CAPABILITY_REPORT`.

**Step 7: Read and verify the payload.**

```c
sb_engine_string_view_t payload;
memset(&payload, 0, sizeof(payload));
if (sb_engine_result_payload(result, &payload) != SB_ENGINE_STATUS_OK ||
    payload.data == 0 || payload.size_bytes == 0 ||
    !contains_bytes(payload.data, payload.size_bytes, "capability probe", 16)) {
    return 7;
}
```

The smoke test confirms the payload contains the substring `"capability probe"`. The payload encoding for capability reports is not specified in the public headers; the program checks for a known substring rather than parsing a structured format.

**Step 8: Release result, end session, close engine.**

```c
if (sb_engine_result_release(result) != SB_ENGINE_STATUS_OK) {
    return 8;
}

sb_engine_session_end_params_v1_t end_params;
memset(&end_params, 0, sizeof(end_params));
end_params.struct_size = sizeof(end_params);
end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
end_params.rollback_active_transactions = 1;
end_params.cancel_open_results = 1;
if (sb_engine_session_end(session, &end_params, 0) != SB_ENGINE_STATUS_OK) {
    return 9;
}
if (sb_engine_close(engine, 0) != SB_ENGINE_STATUS_OK) {
    return 10;
}
return 0;
```

Teardown always proceeds in reverse acquisition order: result, session, engine. The `rollback_active_transactions = 1` and `cancel_open_results = 1` session-end parameters are the recommended safe teardown pattern.

## Pack 2: `core_beta_qa`

**Files:**
- `project/examples/core_beta_qa/admin_lifecycle_smoke.sh`
- `project/examples/core_beta_qa/embedded_public_abi_smoke.sh`
- `project/examples/core_beta_qa/driver_route_smoke.sh`
- `project/examples/core_beta_qa/manifest.json`
- `project/examples/core_beta_qa/README.md`

These scripts require a built tree. They accept two positional arguments: `REPO_ROOT` (default: the repository root inferred from the script's own location) and `BUILD_ROOT` (default: `$REPO_ROOT/build`).

Run from the repository root:

```bash
project/examples/core_beta_qa/admin_lifecycle_smoke.sh    "$PWD" "$PWD/build"
project/examples/core_beta_qa/embedded_public_abi_smoke.sh "$PWD" "$PWD/build"
project/examples/core_beta_qa/driver_route_smoke.sh        "$PWD" "$PWD/build"
```

All scripts use `set -euo pipefail`, create a temporary work directory under `$TMPDIR` or `/tmp`, and clean up on exit.

### `admin_lifecycle_smoke.sh`

Runs the built `database_lifecycle_admin_cli_conformance` fixture:

```bash
FIXTURE="${BUILD_ROOT}/tests/database_lifecycle/database_lifecycle_admin_cli_conformance"
"${FIXTURE}"
echo "admin_lifecycle_smoke=passed"
```

This fixture covers the database create/open/attach/detach lifecycle through the admin CLI conformance gate. The script comment is explicit: it does not ask the engine to execute SQL text; parser/client text remains outside the engine authority boundary.

### `embedded_public_abi_smoke.sh`

Runs two built fixtures back to back:

```bash
ABI_FIXTURE="${BUILD_ROOT}/tests/engine_public_abi/sb_engine_public_abi_cpp_fixture"
SBLR_FIXTURE="${BUILD_ROOT}/tests/engine_public_abi/sb_engine_public_sblr_admission_fixture"
"${ABI_FIXTURE}"
"${SBLR_FIXTURE}"
echo "embedded_public_abi_smoke=passed"
```

- `sb_engine_public_abi_cpp_fixture` exercises the public C++ ABI directly (the C++ RAII wrappers and C ABI functions).
- `sb_engine_public_sblr_admission_fixture` exercises SBLR admission — constructing and dispatching SBLR envelopes through the public dispatch path, verifying that the Priority D admission gate behaves correctly.

The `core_beta_qa/manifest.json` records the engine execution boundary: "Engine execution remains SBLR/internal API based; SQL text is parser/client input only and is not runtime authority."

### `driver_route_smoke.sh`

Runs representative driver gates through CTest:

```bash
ctest --test-dir "${BUILD_ROOT}" \
  -R "driver_package_manifest_gate|driver_python_gate|driver_cpp_gate" \
  --output-on-failure
echo "driver_route_smoke=passed"
```

This does not exercise the embedding API directly; it validates the driver package manifest gate and representative Python and C++ driver gates. Optional driver toolchains have their own deterministic skip/waiver behavior inside those CTest targets.

## Pack 3: `public_smoke_suite` (Manifest Only)

**File:**
- `project/examples/public_smoke_suite/manifest.json`

This pack contains only a JSON manifest (no directly runnable scripts). The manifest describes a sequence of staged CTest operations (create, open, schema, insert, select, transaction, rollback, backup, verify, diagnostics) that reference built test fixtures by CTest target name. It serves as the operations inventory for staged public smoke testing.

Key policy field from the manifest:

```json
{
  "sql_text_runtime_authority": false,
  "engine_execution_authority": "sblr_internal_api_uuid_mga"
}
```

The `public_smoke_suite` manifest refers to fixtures that are not co-located in the examples directory; they are in `project/tools/release/` and `project/tests/release/`. This pack is intended for use by the build/release toolchain rather than as standalone runnable scripts.

## Summary of Example Coverage

| Example | Language | What It Tests |
| --- | --- | --- |
| `public_engine_consumer_smoke/main.c` | C | Full open/session/dispatch/result/close cycle against the C ABI |
| `core_beta_qa/embedded_public_abi_smoke.sh` | Shell | C++ ABI and SBLR admission fixtures |
| `core_beta_qa/admin_lifecycle_smoke.sh` | Shell | Database lifecycle admin CLI conformance |
| `core_beta_qa/driver_route_smoke.sh` | Shell | Driver package manifest and representative driver gates |
| `public_smoke_suite/manifest.json` | JSON | Staged CTest operation inventory (not directly runnable) |




# Client and Driver Guide




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/README.md -->

<a id="ch-client-driver-guide-readme-md"></a>

# ScratchBird Client and Driver Guide

## Purpose

This guide is the reference for every component that connects an application to a ScratchBird
Convergent Data Engine (CDE). It covers the shared connection model, wire protocol, authentication,
TLS, type mapping, diagnostics, pooling, metadata, conformance, and CLI tools — the backbone that
all drivers and adaptors build on. Per-driver and per-adaptor detail is in the individual chapters
listed in the tables below.

This is a **draft**. All components documented here carry a `beta_2` driver status and
`release_candidate` release bucket. No claim in this guide constitutes a production certification
or a promise of binary stability beyond that designation. Consult the per-component
conformance gate listed in the manifest before deploying any component to a production workload.

## API Boundary Rule

Drivers, adaptors, and tools must communicate through supported public server, wire, parser, or
driver APIs for their category. They must not link directly to private engine internals.

Source: `project/drivers/README.md` — "Drivers, adaptors, and tools must not link directly to
private engine internals. They must communicate through the supported public server, wire, parser,
or driver APIs for their category."

---

## Shared Chapters

| Chapter | Contents |
| --- | --- |
| [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) | Connection model, ingress modes, DSN key set, session opening, auth bootstrap flow |
| [authentication.md](#ch-client-driver-guide-authentication-md) | Client-side credential supply, auth_method negotiation, engine_local_password and scram_ready |
| [tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) | The scratchbird_tls_1_3_floor TLS profile and what clients must present |
| [wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) | ScratchBird Wire Protocol (SBWP) sbwp_v1_1: role, negotiation, framing, relationship to SBPS |
| [type_mapping.md](#ch-client-driver-guide-type-mapping-md) | The sbsql_core type-mapping profile: canonical type to client-representation table |
| [diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) | native_sqlstate profile: how message vectors and refusal vectors surface to clients |
| [pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) | thread_safety_class and pooling_capability values and what they mean for client developers |
| [metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) | sys_information_recursive: introspecting schemas and objects via sys.information.* projections |
| [conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) | S1-S5 staged baseline, BASELINE_REQUIREMENT_MAPPING, conformance gates, release status |
| [cli_tools.md](#ch-client-driver-guide-cli-tools-md) | CLI utilities: sb_isql, sb_admin, sb_backup, sb_security, sb_verify, compatibility front-ends |

---

## Drivers

All 21 drivers share the following manifest values unless noted in the per-driver page:
ingress modes `direct_listener` and `manager_proxy`, wire protocol `sbwp_v1_1`,
auth methods `engine_local_password` and `scram_ready`, TLS profile `scratchbird_tls_1_3_floor`,
metadata profile `sys_information_recursive`, diagnostic profile `native_sqlstate`,
driver status `beta_2`, release bucket `release_candidate`.

Source: `project/drivers/DriverPackageManifest.csv`.

| Family | Name | API Surface | Type Mapping | Thread Safety | Pooling | Conformance Gate | Driver Page |
| --- | --- | --- | --- | --- | --- | --- | --- |
| adbc | ADBC | adbc_c_api | arrow_recordbatch | thread_safe | connection_pool | driver_adbc_gate | [drivers/adbc.md](#ch-client-driver-guide-drivers-adbc-md) |
| flight_sql | FlightSQL | flight_sql_grpc | arrow_recordbatch | thread_safe | stream_pool | driver_flightsql_gate | [drivers/flightsql.md](#ch-client-driver-guide-drivers-flightsql-md) |
| julia | Julia | dbinterface_tables | sbsql_core | thread_safe | connection_pool | driver_julia_gate | [drivers/julia.md](#ch-client-driver-guide-drivers-julia-md) |
| perl | Perl | perl_dbi | sbsql_core | connection_thread_confined | connection_pool | driver_perl_gate | [drivers/perl.md](#ch-client-driver-guide-drivers-perl-md) |
| r2dbc | R2DBC | r2dbc_spi | sbsql_core | thread_safe | reactive_pool | driver_r2dbc_gate | [drivers/r2dbc.md](#ch-client-driver-guide-drivers-r2dbc-md) |
| c_cpp | C/C++ | native_cli_c_api | sbsql_core | thread_safe | session_pool; statement_cache | driver_cpp_gate | [drivers/cpp.md](#ch-client-driver-guide-drivers-cpp-md) |
| dart | Dart | language_binding | sbsql_core | thread_safe | session_pool | driver_dart_gate | [drivers/dart.md](#ch-client-driver-guide-drivers-dart-md) |
| dotnet | .NET | ado_net | sbsql_core | thread_safe | connection_pool | driver_dotnet_gate | [drivers/dotnet.md](#ch-client-driver-guide-drivers-dotnet-md) |
| elixir | Elixir | language_binding | sbsql_core | thread_safe | session_pool | driver_elixir_gate | [drivers/elixir.md](#ch-client-driver-guide-drivers-elixir-md) |
| go | Go | database_sql | sbsql_core | thread_safe | connection_pool | driver_go_gate | [drivers/go.md](#ch-client-driver-guide-drivers-go-md) |
| jdbc | JDBC | jdbc_4_x | sbsql_core | thread_safe | connection_pool | driver_jdbc_gate | [drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md) |
| mojo | Mojo | language_binding | sbsql_core | thread_safe | session_pool | driver_mojo_gate | [drivers/mojo.md](#ch-client-driver-guide-drivers-mojo-md) |
| node | Node.js | language_binding | sbsql_core | thread_safe | connection_pool | driver_node_gate | [drivers/node.md](#ch-client-driver-guide-drivers-node-md) |
| odbc | ODBC | odbc_3_x | sbsql_core | thread_safe | connection_pool | driver_odbc_gate | [drivers/odbc.md](#ch-client-driver-guide-drivers-odbc-md) |
| pascal | Pascal | language_binding | sbsql_core | thread_safe | session_pool | driver_pascal_gate | [drivers/pascal.md](#ch-client-driver-guide-drivers-pascal-md) |
| php | PHP | language_binding | sbsql_core | connection_thread_confined | session_pool | driver_php_gate | [drivers/php.md](#ch-client-driver-guide-drivers-php-md) |
| python | Python | dbapi_2 | sbsql_core | thread_safe | connection_pool | driver_python_gate | [drivers/python.md](#ch-client-driver-guide-drivers-python-md) |
| r | R | dbi | sbsql_core | connection_thread_confined | session_pool | driver_r_gate | [drivers/r.md](#ch-client-driver-guide-drivers-r-md) |
| ruby | Ruby | language_binding | sbsql_core | thread_safe | connection_pool | driver_ruby_gate | [drivers/ruby.md](#ch-client-driver-guide-drivers-ruby-md) |
| rust | Rust | language_binding | sbsql_core | thread_safe | connection_pool | driver_rust_gate | [drivers/rust.md](#ch-client-driver-guide-drivers-rust-md) |
| swift | Swift | language_binding | sbsql_core | thread_safe | session_pool | driver_swift_gate | [drivers/swift.md](#ch-client-driver-guide-drivers-swift-md) |

**Notes:**
- The ADBC driver uses `arrow_recordbatch` type mapping, not `sbsql_core`. See [drivers/adbc.md](#ch-client-driver-guide-drivers-adbc-md).
- The FlightSQL driver uses `arrow_recordbatch` type mapping and a `grpc_status_sqlstate` diagnostic profile rather than `native_sqlstate`. Its DSN key set adds `flight_endpoint`. See [drivers/flightsql.md](#ch-client-driver-guide-drivers-flightsql-md).
- The ODBC driver adds a `dsn` key to the standard DSN key set. See [drivers/odbc.md](#ch-client-driver-guide-drivers-odbc-md).

---

## Adaptors

All 12 adaptors carry `beta_2` driver status, `release_candidate` release bucket, `sbwp_v1_1`
wire protocol, and `native_sqlstate` diagnostic profile unless noted.

| Name | API Surface | Ingress Mode(s) | Type Mapping | Pooling | Conformance Gate | Adaptor Page |
| --- | --- | --- | --- | --- | --- | --- |
| scratchbird-airbyte | application_adapter | direct_listener; manager_proxy | python_dbapi_mapping | delegates_to_python | adaptor_airbyte_gate | [adaptors/airbyte.md](#ch-client-driver-guide-adaptors-airbyte-md) |
| scratchbird-dbt-adapter | application_adapter | direct_listener; manager_proxy | python_dbapi_mapping | delegates_to_python | adaptor_dbt_gate | [adaptors/dbt.md](#ch-client-driver-guide-adaptors-dbt-md) |
| scratchbird-looker | application_adapter | driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_looker_gate | [adaptors/looker.md](#ch-client-driver-guide-adaptors-looker-md) |
| scratchbird-powerbi | application_adapter | direct_listener; manager_proxy | powerquery_mapping | explicit_session | adaptor_powerbi_gate | [adaptors/powerbi.md](#ch-client-driver-guide-adaptors-powerbi-md) |
| scratchbird-tableau | application_adapter | direct_listener; manager_proxy | tableau_mapping | explicit_session | adaptor_tableau_gate | [adaptors/tableau.md](#ch-client-driver-guide-adaptors-tableau-md) |
| scratchbird-dbeaver-driver | application_adapter | manager_proxy; driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_dbeaver_gate | [adaptors/dbeaver.md](#ch-client-driver-guide-adaptors-dbeaver-md) |
| scratchbird-hibernate-dialect | application_adapter | driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_hibernate_gate | [adaptors/hibernate.md](#ch-client-driver-guide-adaptors-hibernate-md) |
| scratchbird-metabase-driver | application_adapter | driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_metabase_gate | [adaptors/metabase.md](#ch-client-driver-guide-adaptors-metabase-md) |
| scratchbird-prisma-adapter | application_adapter | driver_embedded_node | sbsql_core | delegates_to_node | adaptor_prisma_gate | [adaptors/prisma.md](#ch-client-driver-guide-adaptors-prisma-md) |
| scratchbird-sqlalchemy-dialect | application_adapter | driver_embedded_python | python_dbapi_mapping | delegates_to_python | adaptor_sqlalchemy_gate | [adaptors/sqlalchemy.md](#ch-client-driver-guide-adaptors-sqlalchemy-md) |
| scratchbird-superset-driver | application_adapter | driver_embedded_python | python_dbapi_mapping | delegates_to_python | adaptor_superset_gate | [adaptors/superset.md](#ch-client-driver-guide-adaptors-superset-md) |
| scratchbird-typeorm-adapter | application_adapter | driver_embedded_node | sbsql_core | delegates_to_node | adaptor_typeorm_gate | [adaptors/typeorm.md](#ch-client-driver-guide-adaptors-typeorm-md) |

**Notes:**
- Looker, DBeaver, Hibernate, and Metabase use `driver_embedded_jdbc` ingress and `jdbc_mapping` type mapping.
- Looker, Hibernate, and Metabase use `jdbc_url` as their primary DSN key instead of the standard `database;host;port` triple.
- DBeaver uses `manager_proxy;driver_embedded_jdbc` (both modes listed).
- Prisma and TypeORM use `driver_embedded_node` ingress and `sbsql_core` type mapping.

---

## CLI Tools

| Tool | Purpose | Page |
| --- | --- | --- |
| sb_isql | Interactive SBsql shell (SBsql dialect, Firebird ISQL compatible with PostgreSQL psql extensions) | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_admin | Scheduler administration and metrics access | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_backup | Backup, restore, and backup-file verification | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_security | User, role, permission, and audit administration | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_verify | Database integrity and consistency verification | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_fb_isql | Firebird SQL syntax compatibility front-end | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_my_isql | MySQL wire protocol compatibility front-end | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |
| sb_pg_isql | PostgreSQL wire protocol compatibility front-end | [cli_tools.md](#ch-client-driver-guide-cli-tools-md) |

---

## Cross-References

- Operations and Administration Guide (ScratchBird — Operations, Security, and Autonomy, page XXX) — operator procedures for backup, restore, diagnostics, and service lifecycle
- Security Guide (ScratchBird — Operations, Security, and Autonomy, page XXX) — engine-side authentication providers, authorization, and cryptographic policy
- Language Reference — Data Types (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — canonical SBsql type names
- Language Reference — Refusal Vectors (SBsql Language Reference — Syntax, page XXX) — how the engine classifies refused requests
- Language Support (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — localized/multilingual SBsql. A driver's language-surface capabilities (completion, canonical preview, localized diagnostics, the editor tool protocol) are defined in the language surface manifest and explained in the Language Support manual; this guide does not restate them. See Client And Editor Language Surface (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX).




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/connection_and_dsn.md -->

<a id="ch-client-driver-guide-connection-and-dsn-md"></a>

# Connection and DSN

## Purpose

This page describes how a ScratchBird client opens a session: the two ingress modes available
to all drivers, the shared set of DSN (Data Source Name) keys that all drivers recognize, the
sequence a driver follows to open a connection, and the auth bootstrap flow that runs before
statement execution is possible.

All values on this page are verified from `project/drivers/DriverPackageManifest.csv` and
from driver implementation sources including `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md`,
`project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`, and
`project/drivers/tool/cli/cli_auth_bootstrap.cpp`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Ingress Modes

Every ScratchBird driver supports exactly two ingress modes. The mode governs how the client
reaches the server and which front-door accepts the connection.

| Mode | Manifest Value | Description |
| --- | --- | --- |
| Direct listener | `direct_listener` | Client connects directly to a ScratchBird listener endpoint over TCP. The listener negotiates SBWP, authenticates the connection, and routes it to the engine. This is the standard network connection mode. |
| Manager proxy | `manager_proxy` | Client connects through a ScratchBird manager, which acts as a front-door proxy. The client must supply a `manager_auth_token` for the attach; the manager then establishes the engine-side session. This mode is used in managed or multi-tenant deployments. |

Source: `DriverPackageManifest.csv` column `ingress_mode_set` — value `direct_listener;manager_proxy`
for all standard drivers.

The CLI tools document an additional pair of transport modes (`embedded` and `local-ipc`) that map
to IPC transports for local use. Those modes do not use the SBWP TCP listener path. See
[cli_tools.md](#ch-client-driver-guide-cli-tools-md) for details.

---

## Shared DSN Key Set

The following DSN keys are recognized by all standard drivers (those with the `sbsql_core`,
`arrow_recordbatch`, `python_dbapi_mapping`, or `jdbc_mapping` type-mapping profile).

Source: `DriverPackageManifest.csv` column `dsn_key_set`.

### Standard Keys (all drivers except where noted)

| Key | Type | Description |
| --- | --- | --- |
| `database` | string | The target database name or path on the server. Required. |
| `host` | string | Hostname or IP address of the ScratchBird listener or manager. Default: `localhost`. |
| `port` | integer | TCP port. Default: `3092`. |
| `user` | string | Username for authentication. |
| `auth_method` | string | Requested authentication method. See [authentication.md](#ch-client-driver-guide-authentication-md) for admitted values. |

### Alias Keys (Python driver verified; similar aliases apply in other drivers)

The Python driver accepts the following DSN aliases and normalizes them:

| Alias | Canonical Key |
| --- | --- |
| `dbname` | `database` |
| `username` | `user` |
| `connecttimeout` | `connect_timeout` |
| `sockettimeout` | `socket_timeout` |
| `applicationname` | `application_name` |
| `binarytransfer` | `binary_transfer` |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — connection config alias
handling.

### ODBC-specific key

The ODBC driver adds a `dsn` key (an ODBC data source name registered in the driver manager)
in addition to the standard keys.

Source: `DriverPackageManifest.csv` row `driver:odbc`, `dsn_key_set` = `database;host;port;dsn;user;auth_method`.

### JDBC-based adaptor keys

Adaptors that embed the JDBC driver (Looker, Hibernate, Metabase, DBeaver) use a `jdbc_url`
key rather than the individual `database;host;port` triple. The `user` and `auth_method` keys
remain.

Source: `DriverPackageManifest.csv` rows for `scratchbird-looker`, `scratchbird-hibernate-dialect`,
`scratchbird-metabase-driver`.

### FlightSQL keys

The FlightSQL driver adds a `flight_endpoint` key alongside `database`, `user`, and `auth_method`.

Source: `DriverPackageManifest.csv` row `driver:flightsql`.

---

## Auth Bootstrap Fields

In addition to the standard DSN keys, drivers that implement the staged auth bootstrap
expose the following startup configuration fields. These are set in the connection config or
DSN and are passed through the startup sequence before the first round-trip.

| Field | Purpose |
| --- | --- |
| `auth_token` | Generic token-auth payload surface |
| `auth_method_id` | Explicit auth method identifier |
| `auth_payload_json` | Auth payload in JSON form |
| `auth_payload_b64` | Auth payload in base64 form |
| `auth_provider_profile` | Auth provider profile selector |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "auth startup config fields".

---

## Opening a Session

The sequence a driver follows to open a session is:

1. **Parse and normalize the DSN or connection config.** Aliases are resolved to canonical keys.
   Non-native `protocol`/`parser`/`dialect` hints (e.g., `jdbc`, `postgresql`, `odbc`) are
   normalized to `native` mode.
2. **Validate ingress mode.** If `front_door_mode=manager_proxy`, a `manager_auth_token` must
   be present before any socket connect attempt. Failure is fast (no socket opened).
3. **Establish the transport.** For `direct_listener`, open a TCP connection to `host:port`
   (default `localhost:3092`). If TLS is required (default unless `sslmode=disable`), perform
   TLS handshake under the `scratchbird_tls_1_3_floor` profile. See [tls_profiles.md](#ch-client-driver-guide-tls-profiles-md).
4. **Negotiate SBWP.** Send a `STARTUP` frame with protocol version `sbwp_v1_1`. See
   [wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md).
5. **Authenticate.** The server responds with an `AuthRequest`. The driver selects an auth
   method from its admitted set and responds. See [authentication.md](#ch-client-driver-guide-authentication-md).
6. **Receive `AuthOk` and `Ready`.** The session is now open and ready for statement execution.

---

## Session Schema

After a session is opened, the active schema context can be queried or changed:

- `get_session_schema()` — returns the normalized active session-schema setting.
- `set_session_schema(schema)` — updates session-schema state and executes `SET SCHEMA` /
  `SET SEARCH_PATH` on the server. Resetting to `None` normalizes to `public`.

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "runtime session-schema
parity helpers".

---

## Connection Liveness

Drivers expose a liveness probe:

- `is_valid(timeout_ms)` — returns a boolean backed by a `ping()` round-trip. A closed
  connection returns `False`. A negative timeout raises a `ProgrammingError`.

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "JDBC-style liveness helper".

---

## Cross-References

- [authentication.md](#ch-client-driver-guide-authentication-md) — auth method negotiation and credential supply
- [tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profile requirements
- [wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP startup and negotiation frames
- Security Guide: authentication_and_providers.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — engine-side provider policy




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/authentication.md -->

<a id="ch-client-driver-guide-authentication-md"></a>

# Authentication

## Purpose

This page describes how a ScratchBird driver supplies credentials to the server. It covers
auth method negotiation, the two admitted methods available in all current drivers
(`engine_local_password` and `scram_ready`), and the staged auth-surface probe that
allows a client to discover the server's admitted methods before opening a full session.

This page covers the **client side** of authentication — what the driver sends. The engine's
provider registry, plugin trust evaluation, and policy-pack behavior are covered in
Security Guide: authentication_and_providers.md (ScratchBird — Operations, Security, and Autonomy, page XXX)
and Security Guide: auth_plugin_families.md (ScratchBird — Operations, Security, and Autonomy, page XXX).

All values on this page are verified from `project/drivers/DriverPackageManifest.csv` and
`project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Auth Methods

The manifest column `auth_method_set` lists the auth methods a driver negotiates. All standard
drivers declare the same two-method set:

| Auth Method | Manifest Value | Description |
| --- | --- | --- |
| Engine local password | `engine_local_password` | Password authentication managed by the ScratchBird engine's local credential store. The driver sends a password credential; the engine verifies it against the local store. |
| SCRAM-ready | `scram_ready` | The connection is prepared to participate in SCRAM (Salted Challenge Response Authentication Mechanism) authentication. Drivers implement `SCRAM_SHA_256` and `SCRAM_SHA_512` challenge-response exchanges. |

Source: `DriverPackageManifest.csv` column `auth_method_set` — value `engine_local_password;scram_ready` for all 21 drivers and all 12 adaptors.

---

## Auth Method Negotiation

The sequence for auth method negotiation is:

1. The driver sends a `STARTUP` message that includes a preferred `auth_method` (or leaves it
   for the server to select).
2. The server responds with an `AuthRequest` message identifying the method it has selected
   from the driver's admitted set and its own policy.
3. The driver executes the selected method:

| Method | Driver Execution |
| --- | --- |
| `PASSWORD` | The driver sends a password credential directly. Maps to `engine_local_password`. |
| `SCRAM_SHA_256` | The driver executes a SCRAM-SHA-256 challenge-response exchange. |
| `SCRAM_SHA_512` | The driver executes a SCRAM-SHA-512 challenge-response exchange. Both SHA variants are part of `scram_ready`. |
| `TOKEN` | The driver sends a generic token payload. Used for managed/proxy bootstrap flows. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "direct auth execution in
`_startup_and_auth()` for: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, generic `TOKEN`".

### Fail-Closed Methods

Drivers fail closed — rather than guessing — when the server requests a method that is admitted
in principle but not executable directly by the client:

| Method | Behavior |
| --- | --- |
| `MD5` | Driver rejects with an unsupported error. MD5 is admitted by the server but not locally executable in the driver. |
| `PEER` | Driver rejects with a broker-required error. PEER authentication requires a broker path, not direct client execution. |
| `REATTACH` | Driver rejects with a broker-required error. Generic REATTACH requires a broker path. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "fail-closed unsupported/
broker-required handling for admitted-but-not-local auth methods".

The Go driver confirms the same pattern: "admitted but unsupported or broker-required methods
(`MD5`, `PEER`, `REATTACH`) now fail closed with `0A000` instead of guessing through generic
payload replay."

Source: `project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`.

---

## Staged Auth Surface Probe

Drivers expose a pre-connect probe that discovers the server's admitted auth methods without
opening a full session:

- `probe_auth_surface(...)` — performs pre-connect auth negotiation and returns the set of
  admitted methods, plugin IDs, and whether any method requires a broker.
- `get_resolved_auth_context()` — returns the resolved auth context after a real connect.

This probe is available in both direct-listener and manager-proxy modes:

| Mode | Probe Behavior |
| --- | --- |
| `direct_listener` | Connects to the listener, exchanges STARTUP, receives auth challenge, and exits before completing auth. |
| `manager_proxy` | Probes the manager's front-door for admitted methods and managed token bootstrap availability. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "staged auth/bootstrap
surfaces … `probe_auth_surface(...)`, `get_resolved_auth_context()`".

The CLI tools (`sb_isql`, `sb_admin`, `sb_security`) expose the same probe via
`--probe-auth-surface` and `--show-auth-context` command-line flags.
See [cli_tools.md](#ch-client-driver-guide-cli-tools-md).

---

## Manager Proxy Token

When using `manager_proxy` ingress mode, the driver must supply a `manager_auth_token` before
any socket connection is attempted. The driver fails fast (before opening a socket) if this
token is absent.

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "fail-fast validation in
`_connect()` so `front_door_mode=manager_proxy` without `manager_auth_token` errors before
any socket connect attempt".

---

## DSN Auth Fields

The following DSN / connection config fields control auth method selection and credential supply:

| Key | Purpose |
| --- | --- |
| `auth_method` | Request a specific auth method by name |
| `auth_token` | Generic token-auth payload |
| `auth_method_id` | Explicit auth method identifier |
| `auth_payload_json` | Auth payload in JSON form |
| `auth_payload_b64` | Auth payload in base64 form |
| `auth_provider_profile` | Auth provider profile selector |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "auth startup config fields
on `ConnectionConfig`".

---

## MGA Reauth

SBWP includes a `Reauth` message type (`0x22`). Sessions may be re-authenticated after
initial connection within the same transport. This is a server-initiated flow; the client
responds to a new `AuthRequest`. Drivers that implement the `kFeatureReauth` feature bit
(feature bit 22) support this path.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `kReauth = 0x22`,
`kFeatureReauth = FeatureBit(22)`.

---

## Cross-References

- [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN keys, ingress modes, session opening
- Security Guide: authentication_and_providers.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — engine-side provider registry and trust model
- Security Guide: auth_plugin_families.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — all 18 plugin families and their capabilities
- Security Guide: security_policies_and_crypto.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — policy-pack model and cryptographic hardening




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/tls_profiles.md -->

<a id="ch-client-driver-guide-tls-profiles-md"></a>

# TLS Profiles

## Purpose

This page describes the TLS profile that all ScratchBird drivers and adaptors use when
establishing encrypted connections. It covers what the profile name means, what the client
must present, and when plaintext transport is available.

All values on this page are verified from `project/drivers/DriverPackageManifest.csv` and
`project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md`,
`project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## The scratchbird_tls_1_3_floor Profile

Every driver and adaptor in the manifest declares a single TLS profile:

| Profile Name | Manifest Value |
| --- | --- |
| ScratchBird TLS 1.3 floor | `scratchbird_tls_1_3_floor` |

Source: `DriverPackageManifest.csv` column `tls_profile_set` — value `scratchbird_tls_1_3_floor`
for all 21 drivers, all 12 adaptors, and the CLI tool.

The name encodes the floor requirement: **TLS 1.3 is the minimum acceptable version**. A
connection that cannot negotiate TLS 1.3 or higher must not proceed unless the connection is
explicitly configured as plaintext (see below).

---

## What Clients Must Present

When the `scratchbird_tls_1_3_floor` profile is active, the client:

1. Initiates a TLS handshake after the TCP connection is established and before sending any
   SBWP frames.
2. Must negotiate TLS 1.3 or higher. Connections that can only negotiate TLS 1.2 or earlier
   are refused by the server.
3. Must present a valid certificate chain that the server trusts, if the server is configured
   to require mutual TLS. The exact server-side certificate policy is an operator concern; see
   Security Guide: security_policies_and_crypto.md (ScratchBird — Operations, Security, and Autonomy, page XXX).
4. After TLS is established, sends the SBWP `STARTUP` frame over the encrypted transport.

---

## Plaintext Transport (sslmode=disable)

Drivers accept `sslmode=disable` in the DSN to open a plaintext connection without TLS
wrapping. This is intended for development, local loopback, and operator-controlled
environments where TLS termination happens at the network layer.

| DSN Key | Value | Effect |
| --- | --- | --- |
| `sslmode` | `disable` | Opens plaintext transport without TLS handshake. |
| `sslmode` | `require` | Requires TLS; refuses if not available. |
| `sslmode` | `verify-ca` | Requires TLS and validates server certificate against a trusted CA. |
| `sslmode` | `verify-full` | Requires TLS, validates CA, and validates server hostname. |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "`sslmode=disable` now
opens plaintext transport without TLS wrapping."

Source: `project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md` — "`sslmode` accepts JDBC-compatible
aliases and normalizes to `disable|require|verify-ca|verify-full`".

Do not use `sslmode=disable` on untrusted networks. The engine's transport security policy may
also refuse plaintext connections depending on the operator's configuration.

---

## OpenSSL Dependency

The SBWP wire implementation uses OpenSSL for TLS. The worker source includes
`<openssl/ssl.h>` and `<openssl/err.h>`.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — OpenSSL includes at
top of file.

The exact OpenSSL version compatibility range is a build-system concern and is not documented
here; see `project/drivers/tool/cli/docs/BUILD_MATRIX.md` (not verified in this audit).

---

## Cross-References

- [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN keys, ingress modes, session opening sequence
- [wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP framing after TLS is established
- Security Guide: security_policies_and_crypto.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — server-side cryptographic policy
- Security Guide: platform_configuration.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — platform-specific TLS configuration




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/wire_protocol_sbwp.md -->

<a id="ch-client-driver-guide-wire-protocol-sbwp-md"></a>

# Wire Protocol: SBWP

## Purpose

This page describes the ScratchBird Wire Protocol (SBWP), version `sbwp_v1_1`. It covers the
protocol's role in the connection stack, what version `sbwp_v1_1` means, the framing model as
visible in source, the feature-bit negotiation mechanism, and the relationship between SBWP and
the internal SBPS (parser-to-server) IPC layer.

All values on this page are verified against
`project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`. Frame byte layouts that cannot
be confirmed from available source are noted as unverified and omitted.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Role

SBWP is the client-to-server wire protocol. Every standard ScratchBird driver and adaptor
communicates with the engine over SBWP. It runs over TCP (TLS-wrapped by default under the
`scratchbird_tls_1_3_floor` profile) and mediates:

- session startup and authentication
- query execution (simple and extended query protocols)
- transaction control
- result streaming
- cancellation and termination
- optional capabilities negotiated at startup (compression, streaming, SBLR execution, etc.)

SBWP is distinct from SBPS (the parser-to-server IPC protocol), which is an internal
communication path between the sbsql_worker parser process and the core engine. Client
drivers do not speak SBPS. The `src/wire/` tree contains components used by both SBWP and
SBPS (result batch transfer, streaming cursor manager, etc.).

---

## Version

| Manifest Value | Meaning |
| --- | --- |
| `sbwp_v1_1` | SBWP major version 1, minor version 1 |

Source: `DriverPackageManifest.csv` column `wire_protocol_set` — `sbwp_v1_1` for all 21
drivers, 12 adaptors, and the CLI tool.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`:

```cpp
constexpr std::uint8_t kSbwpMajor = 1;
constexpr std::uint8_t kSbwpMinor = 1;
constexpr std::uint16_t kSbwpVersionMin  = 0x0100;  // 1.0 — minimum accepted
constexpr std::uint16_t kSbwpVersionCurrent = 0x0101;  // 1.1 — current
```

The server accepts connections from clients presenting version `0x0100` (1.0) or higher. The
current version is `0x0101` (1.1).

---

## Frame Header

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`:

```cpp
constexpr std::size_t kSbwpHeaderSize = 40;
constexpr std::uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;  // 64 MiB
```

Each SBWP frame has a 40-byte header. Maximum payload per frame is 64 MiB.

Frame flags (from the header):

| Flag Constant | Value | Meaning |
| --- | --- | --- |
| `kFrameFlagCompressed` | bit 0 | Payload is compressed |
| `kFrameFlagPartial` | bit 1 | Frame is a partial (continuation) frame |

Unknown flag bits outside `kFrameFlagKnownMask` should be treated as a protocol error by
conformant clients.

The exact byte layout of the 40-byte header (field positions, endianness) is not documented
here as the source shows only the size constant and not the full struct layout accessible in
this audit.

---

## Message Types

The following message type codes are defined in source. Client-to-server messages are in the
`0x01`–`0x2F` range; server-to-client messages are in the `0x40`–`0x81` range.

### Client-to-Server Messages

| Constant | Code | Purpose |
| --- | --- | --- |
| `kStartup` | `0x01` | Initiate a session |
| `kAuthResponse` | `0x02` | Authentication credential response |
| `kQuery` | `0x03` | Simple query execution |
| `kParse` | `0x04` | Extended query: parse a statement |
| `kBind` | `0x05` | Extended query: bind parameters |
| `kDescribe` | `0x06` | Extended query: describe a portal or statement |
| `kExecute` | `0x07` | Extended query: execute a portal |
| `kClose` | `0x08` | Extended query: close a portal or statement |
| `kSync` | `0x09` | Extended query: synchronize pipeline |
| `kCancel` | `0x0b` | Cancel an in-progress request |
| `kTerminate` | `0x0c` | Terminate the session |
| `kCopyData` | `0x0d` | COPY data payload |
| `kCopyDone` | `0x0e` | COPY transfer complete |
| `kCopyFail` | `0x0f` | COPY transfer failed |
| `kSblrExecute` | `0x10` | Execute an SBLR bytecode payload directly |
| `kSubscribe` | `0x11` | Subscribe to notifications |
| `kUnsubscribe` | `0x12` | Unsubscribe from notifications |
| `kStreamControl` | `0x14` | Streaming flow control |
| `kTxnBegin` | `0x15` | Begin a transaction |
| `kTxnCommit` | `0x16` | Commit a transaction |
| `kTxnRollback` | `0x17` | Roll back a transaction |
| `kTxnSavepoint` | `0x18` | Create a savepoint |
| `kTxnRelease` | `0x19` | Release a savepoint |
| `kTxnRollbackTo` | `0x1a` | Roll back to a savepoint |
| `kPing` | `0x1b` | Liveness ping |
| `kSetOption` | `0x1c` | Set a session option |
| `kResetSession` | `0x21` | Reset session state |
| `kReauth` | `0x22` | Re-authenticate on existing connection |
| `kTraceContext` | `0x23` | Attach a trace/telemetry context |

### Server-to-Client Messages

| Constant | Code | Purpose |
| --- | --- | --- |
| `kAuthRequest` | `0x40` | Server auth challenge |
| `kAuthOk` | `0x41` | Authentication succeeded |
| `kReady` | `0x43` | Server ready for next command |
| `kRowDescription` | `0x44` | Column metadata for a result set |
| `kDataRow` | `0x45` | A data row |
| `kCommandComplete` | `0x46` | Command finished |
| `kError` | `0x48` | Error diagnostic |
| `kParseComplete` | `0x4a` | Parse step complete |
| `kBindComplete` | `0x4b` | Bind step complete |
| `kCloseComplete` | `0x4c` | Close complete |
| `kParameterStatus` | `0x4f` | Session parameter status |
| `kParameterDescription` | `0x50` | Parameter type description |
| `kCopyInResponse` | `0x51` | Server ready to receive COPY data |
| `kNotification` | `0x54` | Async notification |
| `kPong` | `0x5d` | Liveness pong (response to ping) |
| `kQueryProgress` | `0x60` | Query progress update |
| `kServerInfo` | `0x61` | Server information |
| `kStateNotification` | `0x62` | Session state change notification |
| `kCancelAck` | `0x65` | Cancel acknowledged |
| `kCancelled` | `0x66` | Operation was cancelled |
| `kMultiResultBegin` | `0x67` | Begin of multi-result response |
| `kMultiResultEnd` | `0x68` | End of multi-result response |
| `kGeneratedKeys` | `0x69` | Generated keys from INSERT/UPDATE |
| `kOutParameters` | `0x6a` | Callable out-parameters |
| `kBatchResult` | `0x6b` | Batch execution result |
| `kPipelineStatus` | `0x6c` | Pipeline execution status |
| `kArrayBindStatus` | `0x6d` | Array bind status |
| `kBulkRejectData` | `0x6e` | Bulk operation reject data |
| `kLobLocator` | `0x6f` | LOB locator reference |
| `kLobChunk` | `0x70` | LOB data chunk |
| `kLobClose` | `0x71` | LOB handle close |
| `kCursorStatus` | `0x72` | Cursor state update |
| `kFailoverHint` | `0x73` | Failover endpoint hint |
| `kHeartbeat` | `0x80` | Server heartbeat |
| `kExtension` | `0x81` | Protocol extension frame |

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `enum Msg` definition.

---

## Ready Reasons

The `kReady` message carries a reason code indicating why the server became ready:

| Constant | Value | Trigger |
| --- | --- | --- |
| `kStartup` | `1` | Session startup complete |
| `kCommandComplete` | `2` | Previous command completed |
| `kErrorRecovered` | `3` | Error was recovered; pipeline continues |
| `kResetComplete` | `4` | Session reset complete |
| `kReauthComplete` | `5` | Re-authentication complete |
| `kCancelOutcome` | `6` | Cancel request outcome delivered |
| `kStateChange` | `7` | Session state changed |

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `enum class ReadyReason`.

---

## Feature Negotiation

During startup, the client and server negotiate optional capabilities using a feature-bit
field. Each bit represents a capability the client wishes to use. The server's response
indicates which capabilities it will honor.

| Feature Constant | Bit | Capability |
| --- | --- | --- |
| `kFeatureCompression` | 0 | Frame-level payload compression |
| `kFeatureStreaming` | 1 | Streaming result windows |
| `kFeatureSblr` | 2 | Direct SBLR bytecode execution (`kSblrExecute`) |
| `kFeatureNotifications` | 4 | Async notifications (`kNotification`) |
| `kFeatureBatch` | 6 | Batch execution (`kBatchResult`) |
| `kFeaturePipeline` | 7 | Pipelined execution (`kPipelineStatus`) |
| `kFeatureBinaryCopy` | 8 | Binary COPY format |
| `kFeatureSavepoints` | 9 | Savepoint support |
| `kFeatureMultiResult` | 13 | Multi-result responses |
| `kFeatureGeneratedKeys` | 14 | Generated key return |
| `kFeatureOutParameters` | 15 | Callable out-parameters |
| `kFeatureArrayBind` | 16 | Array parameter binding |
| `kFeatureBulkRejects` | 17 | Bulk operation reject reporting |
| `kFeatureLobLocator` | 18 | LOB locator references |
| `kFeatureCursors` | 19 | Cursor control |
| `kFeatureCopyBackpressure` | 20 | COPY backpressure flow control |
| `kFeatureSessionReset` | 21 | Session reset (`kResetSession`) |
| `kFeatureReauth` | 22 | Re-authentication on existing connection |
| `kFeatureFailoverHints` | 23 | Failover endpoint hints |
| `kFeatureTraceContext` | 24 | Trace context attachment |

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `constexpr uint64_t kFeature*` definitions.

---

## SBWP and SBPS

SBWP is the **external** wire protocol. SBPS (ScratchBird Parser-Server Protocol) is the
**internal** IPC protocol between the sbsql_worker parser process and the core engine server.
Client drivers speak SBWP only; they never interact with SBPS directly.

The `src/wire/` directory contains shared components that serve both paths:
streaming result windows (`streaming_result_window`), result batch transfer
(`result_batch_transfer`), streaming cursor manager (`streaming_cursor_manager`), and direct
binary result frames (`direct_binary_result_frame`). These are engine internals.

The `src/wire/parser_server_ipc/` subdirectory contains the SBPS IPC layer
(`parser_server_ipc.cpp`, `parser_ipc_common.cpp`). This is also an engine internal.

---

## Cross-References

- [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — session opening sequence that uses STARTUP
- [authentication.md](#ch-client-driver-guide-authentication-md) — AuthRequest/AuthResponse exchange over SBWP
- [tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS layer under which SBWP frames run
- [diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — how `kError` messages map to SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/type_mapping.md -->

<a id="ch-client-driver-guide-type-mapping-md"></a>

# Type Mapping

## Purpose

This page describes the `sbsql_core` type-mapping profile — the profile used by the majority
of ScratchBird drivers. It provides a shared table of canonical SBsql types and their common
client-side representations, verified against the Python driver type implementation.

The `sbsql_core` profile governs how values move between the engine's canonical SBsql type
system and the host language. The profile does not invent types; it maps the types defined
in the SBsql type system to the nearest idiomatic representation in each language.

Sources used: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md`,
`project/drivers/DriverPackageManifest.csv`,
`project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` (OID constants).

For the canonical SBsql type definitions, see
Language Reference: Data Types (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX).

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Type Mapping Profiles by Driver Family

| Profile | Used by |
| --- | --- |
| `sbsql_core` | cpp, dart, dotnet, elixir, go, jdbc, julia, mojo, node, odbc, pascal, perl, php, python, r2dbc, ruby, rust, swift; also scratchbird-prisma-adapter and scratchbird-typeorm-adapter |
| `arrow_recordbatch` | adbc, flightsql |
| `python_dbapi_mapping` | scratchbird-airbyte, scratchbird-dbt-adapter, scratchbird-sqlalchemy-dialect, scratchbird-superset-driver |
| `jdbc_mapping` | scratchbird-dbeaver-driver, scratchbird-hibernate-dialect, scratchbird-looker, scratchbird-metabase-driver |
| `powerquery_mapping` | scratchbird-powerbi |
| `tableau_mapping` | scratchbird-tableau |

Source: `DriverPackageManifest.csv` column `type_mapping_profile`.

The per-driver pages (under `drivers/` and `adaptors/`) document profile-specific detail.
This page covers only the `sbsql_core` profile.

---

## sbsql_core: Canonical Type to Client Representation

The following table maps SBsql canonical type families to their common client representations
in the `sbsql_core` profile, as verified from the Python driver implementation. Languages
with stronger or weaker native types will vary; see per-driver pages for precision.

| SBsql Type Family | Canonical Name(s) | Common Client Representation | Notes |
| --- | --- | --- | --- |
| Boolean | `boolean`, `bool` | Native boolean | Text tokens `true`/`t` map to true; others map to false |
| Signed integer 32-bit | `int32`, `int`, `integer` | Native 32-bit integer | OID 23 |
| Signed integer 64-bit | `int64`, `bigint` | Native 64-bit integer | OID 20; unknown-text integer inference capped at signed int64 range |
| Signed integer 8/16-bit | `int8`, `int16`, `smallint` | Native integer of appropriate width | OID 21 (int2); unknown binary 1-byte decoded as signed int8 |
| Decimal / numeric | `decimal(p,s)`, `numeric(p,s)` | Language decimal or high-precision type | OID 1700; Python: `decimal.Decimal` |
| Approximate real | `real`, `float`, `double precision` | Native float | OID 701 (float8) |
| Text | `varchar(n)`, `text`, `char(n)`, `clob` | Native string | OID 25 (text) |
| Binary | `bytea`, `blob`, `binary(n)`, `varbinary(n)` | Native bytes / byte array | Binary payloads decoded; hex (`\x` prefix or `0x`) and octal escape formats decoded |
| UUID | `uuid` | Language UUID type or string | 16-byte UUID; Python: `uuid.UUID` |
| Date | `date` | Native date | Python: `datetime.date` |
| Time (without timezone) | `time(p)` | Native time (no offset) | Rejects timezone-offset payloads |
| Time with timezone | `timetz` | Offset-aware time | Requires explicit timezone offset; 12-byte binary payload (microseconds + zone seconds west) |
| Timestamp (without timezone) | `timestamp(p)` | Naive datetime | Rejects timezone-offset payloads; materializes naive datetime |
| Timestamp with timezone | `timestamptz` | UTC-aware datetime | Materializes UTC-aware datetime |
| Interval | `interval` | Language interval or duration type | Representation varies by language |
| XML | `xml` | String or XML document type | Python: `SqlXml` wrapper routes to OID_XML |
| Array | `array<T>` | Native collection / array of T | Typed array OID inference: `bool[]`, `int4[]`, `float8[]`, `text[]`, `date[]`, `timetz[]`, `timestamp[]`, `timestamptz[]`, `numeric[]`, `uuid[]`, `bytea[]`, etc. Mixed int+float widens to float8[] |
| JSON / document | `json_document`, `document` | String or native map/document | Representation varies |
| Vector | `vector` | Float array | Float-only sequences encode as explicit vector OID |
| Range | `daterange`, `tsrange`, `tstzrange` | Language range type or pair | String temporal bounds coerced to UTC-backed binary |
| LOB wrappers | — | `Blob`, `Clob`, `RowId`, `Ref`, `SqlXml` | Python: explicit wrapper types; `Blob`/`RowId` route to `OID_BYTEA`; `Clob`/`Ref` route to `OID_TEXT` |

Source: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md` — type codec parity entries
covering TIMETZ, BYTEA, typed arrays, wrapper families, temporal ranges, and unknown-type inference.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — OID constants:
```cpp
constexpr std::uint32_t kOidBool    = 16;
constexpr std::uint32_t kOidInt8    = 20;
constexpr std::uint32_t kOidInt4    = 23;
constexpr std::uint32_t kOidText    = 25;
constexpr std::uint32_t kOidFloat8  = 701;
constexpr std::uint32_t kOidNumeric = 1700;
```

---

## Temporal Family Rules

The `sbsql_core` profile enforces JDBC-aligned timezone family rules for temporal types:

| Type | Rule |
| --- | --- |
| `time` | Rejects payloads that carry a timezone offset. Must be tz-naive. |
| `timestamp` | Rejects payloads that carry a timezone offset. Materializes as naive datetime. |
| `timetz` | Requires an explicit timezone offset. |
| `timestamptz` | Materializes as UTC-aware datetime. |

Trailing `Z` in text payloads is normalized to UTC offset form.

Source: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md` — "Temporal typed-text parsing
now enforces JDBC family semantics".

---

## Unknown Type Fallback

When the driver receives a value with an unrecognized OID:

- **Binary unknown (1-byte):** decoded as a signed int8.
- **Text unknown (integer-shaped):** auto-coercion is limited to signed int64 range; values
  outside that range remain text.
- **Other objects (parameter encoding):** Python `Enum` values encode using enum member names;
  unsupported objects fall back to `str(value)` with `OID_TEXT`.

Source: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md` — "unknown-binary scalar
parity" and "unknown-text integer bounds parity".

---

## COPY Format

The wire protocol defines a canonical row-fields text format for COPY operations:

```cpp
constexpr std::uint8_t kCopyFormatCanonicalRowFieldsText = 0x00;
constexpr std::uint32_t kCopyDefaultWindowBytes = 64u * 1024u;
```

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`.

---

## Cross-References

- Language Reference: Data Types (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — canonical SBsql type names and definitions
- Language Reference: Data Types: Type System Overview (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — descriptor binding model
- Language Reference: Data Types: Temporal Types (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — temporal type detail
- Language Reference: Data Types: Numeric Types (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — numeric type detail
- [wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — OID values in SBWP frames




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/diagnostics_and_sqlstate.md -->

<a id="ch-client-driver-guide-diagnostics-and-sqlstate-md"></a>

# Diagnostics and SQLSTATE

## Purpose

This page describes the `native_sqlstate` diagnostic-mapping profile — the profile used by
all standard ScratchBird drivers except FlightSQL. It explains how the engine's structured
message vectors and refusal vectors surface as SQLSTATE codes and errors at the client, and
what retry behavior is appropriate for each class.

Sources used: `project/drivers/DriverPackageManifest.csv`,
`project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md`,
`project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`,
and documentation at
`docs/documentation/draft/Language_Reference/syntax_reference/refusal_vectors.md`,
`docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Diagnostic Profiles

| Profile | Manifest Value | Used by |
| --- | --- | --- |
| Native SQLSTATE | `native_sqlstate` | All drivers and adaptors except FlightSQL |
| gRPC status + SQLSTATE | `grpc_status_sqlstate` | flightsql driver only |

Source: `DriverPackageManifest.csv` column `diagnostic_mapping_profile`.

---

## Engine Message Vector Model

The ScratchBird engine produces **message vectors** — structured lists of diagnostic records
rather than single error strings. Each record in the vector carries:

- A `diagnostic_code` string (namespaced, dot-separated).
- A severity level (`ERROR`, `WARNING`, `INFO`).
- A human-readable detail string.
- Optional structured fields.

Source: `docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md` —
"A message vector is a structured list of diagnostic records attached to an operation result."

These flow through named diagnostic channels:

| Channel | Content |
| --- | --- |
| `diagnostic.canonical_message_vector` | General engine and parser diagnostics |
| `diagnostic.lifecycle.message_vector` | Lifecycle events, emulated statement boundaries, startup/shutdown diagnostics |

---

## SQLSTATE Surfacing in native_sqlstate Drivers

Under the `native_sqlstate` profile, the engine's `kError` SBWP message carries a SQLSTATE
field, a detail string, and an optional hint string. Drivers map this to their host language's
error hierarchy.

The Python driver's error mapping (`src/scratchbird/errors.py`) defines the following lanes:

| SQLSTATE Range / Code | Driver Error Class | Retry Boundary |
| --- | --- | --- |
| `40001`, `40P01` | Serialization / deadlock error | Fresh statement boundary only — never auto-replay a whole transaction |
| `08xxx` | Connection class error | Reconnect or reopen — do not retry the statement without restoring the connection |
| All other | Application error | No automatic replay |

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — "`retry_scope_for_sqlstate(...)`
makes the retry boundary explicit: `40001`/`40P01` => fresh statement only, `08xxx` => reconnect
or reopen only, everything else => no automatic replay."

The Go driver confirms the same `0A000` SQLSTATE for fail-closed auth method rejection:
"admitted but unsupported or broker-required methods (`MD5`, `PEER`, `REATTACH`) now fail
closed with `0A000`".

Source: `project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`.

---

## Refusal Vector Classes

SBsql defines three top-level refusal classes. These surface as SQLSTATE and error text in
the `native_sqlstate` profile:

| Refusal Class | Meaning | Retry Expectation |
| --- | --- | --- |
| `unsupported` | The surface, option, route, shape, profile, build flag, or provider operation is not available in this build or for this target. | Only after changing the statement, build profile, provider, or feature set. |
| `denied` | Blocked by authorization, sandboxing, policy, safety, recovery state, resource admission, descriptor rules, stream rules, or data-protection rules. | Only after the blocking authority condition changes. |
| `unlicensed` | The surface and route are recognized, but the running product profile or admitted provider reports that the capability is not licensed. | Only with a product profile or provider that licenses the capability. |

Source: `docs/documentation/draft/Language_Reference/syntax_reference/refusal_vectors.md` — "High-Level Classes".

The engine never silently drops a refused request. Refusal is a controlled, expected
operational outcome.

---

## UDR Bridge Refusal Codes

When SBsql statements cross the UDR bridge, the following codes may appear in the
message vector:

| Code | Trigger |
| --- | --- |
| `UDR.BRIDGE.CONTEXT_MISSING` | Required context packet absent |
| `UDR.BRIDGE.SECRET_MATERIAL_DENIED` | Secret material access not permitted from this surface |
| `UDR.BRIDGE.SANDBOX_DENIED` | Operation denied by sandbox policy |

Source: `docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md`.

---

## Storage Refusal States

Storage and page operations can produce the following refusal states:

| State | Meaning |
| --- | --- |
| `refused` | Request was received and explicitly denied |
| `recovery_required` | The filespace or page agent requires recovery before accepting new work |
| `invalid_filespace_identity` | The filespace identity presented is not valid for this operation |
| `invalid_page_family` | The page family presented does not match the agent's domain |

Source: `docs/documentation/draft/Operations_Administration/diagnostics_message_vectors_and_support_bundles.md` —
page_filespace_handoff refusal states.

---

## Error Hierarchy in Drivers

The `native_sqlstate` profile expects drivers to expose a DB-API-compatible error hierarchy:

- Protocol errors carry `SQLSTATE`, `DETAIL`, and `HINT` fields from the `kError` SBWP message.
- Parser-failure fallback behavior is defined for cases where SQLSTATE is absent or
  the server closed the connection before delivering an error.

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — `ERR` row: "DB-API
error hierarchy, SQLSTATE-to-error-class mapping, retry-boundary classification (`statement` vs
`reconnect` vs `none`), protocol error message shaping (`SQLSTATE`/`DETAIL`/`HINT`), and
parser-failure fallback behavior."

---

## Cross-References

- [wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — `kError` message (code `0x48`) in the SBWP message type table
- Language Reference: Refusal Vectors (SBsql Language Reference — Syntax, page XXX) — full refusal vector specification
- Operations Administration: Diagnostics, Message Vectors, and Support Bundles (ScratchBird — Operations, Security, and Autonomy, page XXX) — operator-side diagnostics and support bundle collection




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/pooling_and_concurrency.md -->

<a id="ch-client-driver-guide-pooling-and-concurrency-md"></a>

# Pooling and Concurrency

## Purpose

This page documents the `thread_safety_class` and `pooling_capability` values that appear
across the driver manifest, and explains what each value means for application developers
choosing a connection pooling strategy.

All values are verified from `project/drivers/DriverPackageManifest.csv`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Thread Safety Classes

The `thread_safety_class` column in the manifest describes whether a single driver connection
object is safe to use from multiple threads concurrently.

| Class | Manifest Value | Meaning for Application Code |
| --- | --- | --- |
| Thread-safe | `thread_safe` | The connection object and derived cursor/statement objects are safe for concurrent use from multiple threads. The driver manages internal synchronization. |
| Connection-thread-confined | `connection_thread_confined` | A connection object must be used from a single thread. Do not share a connection across threads. Create one connection per thread, or use a pool that enforces thread affinity when checking out connections. |

### Per-Driver Thread Safety

| Driver / Adaptor | Thread Safety Class |
| --- | --- |
| adbc | `thread_safe` |
| flightsql | `thread_safe` |
| julia | `thread_safe` |
| perl | `connection_thread_confined` |
| r2dbc | `thread_safe` |
| cpp | `thread_safe` |
| dart | `thread_safe` |
| dotnet | `thread_safe` |
| elixir | `thread_safe` |
| go | `thread_safe` |
| jdbc | `thread_safe` |
| mojo | `thread_safe` |
| node | `thread_safe` |
| odbc | `thread_safe` |
| pascal | `thread_safe` |
| php | `connection_thread_confined` |
| python | `thread_safe` |
| r | `connection_thread_confined` |
| ruby | `thread_safe` |
| rust | `thread_safe` |
| swift | `thread_safe` |
| scratchbird-airbyte | `connection_thread_confined` |
| scratchbird-dbt-adapter | `connection_thread_confined` |
| scratchbird-looker | `connection_thread_confined` |
| scratchbird-powerbi | `connection_thread_confined` |
| scratchbird-tableau | `connection_thread_confined` |
| scratchbird-dbeaver-driver | `connection_thread_confined` |
| scratchbird-hibernate-dialect | `connection_thread_confined` |
| scratchbird-metabase-driver | `connection_thread_confined` |
| scratchbird-prisma-adapter | `connection_thread_confined` |
| scratchbird-sqlalchemy-dialect | `connection_thread_confined` |
| scratchbird-superset-driver | `connection_thread_confined` |
| scratchbird-typeorm-adapter | `connection_thread_confined` |
| CLI tool | `connection_thread_confined` |

Source: `DriverPackageManifest.csv` column `thread_safety_class`.

**Pattern:** All application-layer adaptors are `connection_thread_confined`. This reflects
that adaptors delegate pooling to their embedded driver (Python, JDBC, or Node.js), and the
adaptor layer itself is not safe to share across threads.

---

## Pooling Capabilities

The `pooling_capability` column describes the type of connection pooling the driver supports.

| Capability | Manifest Value | Description |
| --- | --- | --- |
| Connection pool | `connection_pool` | The driver supports a pool of established connections that are checked out by callers and returned when done. The driver manages the pool lifecycle internally. |
| Session pool | `session_pool` | The driver supports a pool of established sessions (which may be lighter-weight than full connections). Callers check out a session. |
| Reactive pool | `reactive_pool` | The driver supports an asynchronous, reactive-streams-compatible connection pool. Used by the R2DBC driver in async/reactive application contexts. |
| Stream pool | `stream_pool` | The driver supports a pool for Arrow stream connections. Used by the FlightSQL driver. |
| Session pool + statement cache | `session_pool;statement_cache` | The driver supports session pooling combined with a per-session statement cache. Used by the C/C++ driver. |
| Explicit session | `explicit_session` | The driver opens and closes sessions explicitly; there is no automatic pool. Callers manage the session lifecycle directly. |
| Delegates to Python | `delegates_to_python` | The adaptor delegates pooling entirely to the embedded Python driver. The adaptor itself does not maintain a pool. |
| Delegates to JDBC | `delegates_to_jdbc` | The adaptor delegates pooling entirely to the embedded JDBC driver. |
| Delegates to Node | `delegates_to_node` | The adaptor delegates pooling entirely to the embedded Node.js driver. |

### Per-Driver Pooling Capability

| Driver / Adaptor | Pooling Capability |
| --- | --- |
| adbc | `connection_pool` |
| flightsql | `stream_pool` |
| julia | `connection_pool` |
| perl | `connection_pool` |
| r2dbc | `reactive_pool` |
| cpp | `session_pool;statement_cache` |
| dart | `session_pool` |
| dotnet | `connection_pool` |
| elixir | `session_pool` |
| go | `connection_pool` |
| jdbc | `connection_pool` |
| mojo | `session_pool` |
| node | `connection_pool` |
| odbc | `connection_pool` |
| pascal | `session_pool` |
| php | `session_pool` |
| python | `connection_pool` |
| r | `session_pool` |
| ruby | `connection_pool` |
| rust | `connection_pool` |
| swift | `session_pool` |
| scratchbird-airbyte | `delegates_to_python` |
| scratchbird-dbt-adapter | `delegates_to_python` |
| scratchbird-looker | `delegates_to_jdbc` |
| scratchbird-powerbi | `explicit_session` |
| scratchbird-tableau | `explicit_session` |
| scratchbird-dbeaver-driver | `delegates_to_jdbc` |
| scratchbird-hibernate-dialect | `delegates_to_jdbc` |
| scratchbird-metabase-driver | `delegates_to_jdbc` |
| scratchbird-prisma-adapter | `delegates_to_node` |
| scratchbird-sqlalchemy-dialect | `delegates_to_python` |
| scratchbird-superset-driver | `delegates_to_python` |
| scratchbird-typeorm-adapter | `delegates_to_node` |
| CLI tool | `explicit_session` |

Source: `DriverPackageManifest.csv` column `pooling_capability`.

---

## Guidance for Application Developers

**For thread_safe drivers with connection_pool or session_pool:**
You may share a pool object across threads. Individual connections or sessions from the pool
are scoped to the borrowing call and should not themselves be shared across threads while
checked out.

**For connection_thread_confined drivers:**
Create one connection per thread. If you need concurrent access, use a pool that enforces
thread affinity — i.e., a checked-out connection must be used and returned on the same thread
that checked it out.

**For delegates_to_* adaptors:**
The adaptor's pooling behavior is entirely determined by the embedded driver. Configure
pooling settings through that driver's native API (e.g., Python driver pool config, JDBC
connection pool config, Node.js driver pool config). The adaptor layer itself does not offer
pool configuration.

**For explicit_session:**
The application is responsible for opening, using, and closing sessions. This is appropriate
for tooling, administrative scripts, and BI tools that manage session lifecycle at the
application level.

---

## Pool Resource Lifecycle (Python driver, verified)

The Python driver pool implementation includes:
- Checkout / reuse / stale-connection replacement
- Statement cache per session
- Retry backoff
- Circuit-breaker state transitions
- Keepalive validation
- Leak-detection guard behavior
- Telemetry metrics and slow-query tracking

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — `RES` row, pool
and resilience primitives.

---

## Cross-References

- [conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) — S1-S5 conformance stages including resource lifecycle (RES stage)
- [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — how sessions are opened




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/metadata_sys_information.md -->

<a id="ch-client-driver-guide-metadata-sys-information-md"></a>

# Metadata: sys_information_recursive

## Purpose

This page describes the `sys_information_recursive` metadata profile — the profile used by
all ScratchBird drivers and adaptors. It explains how drivers introspect database schemas,
tables, columns, indexes, and other catalog objects via the `sys.information.*` recursive
catalog projections.

Sources used: `project/drivers/DriverPackageManifest.csv`,
`project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Profile Identification

| Profile Name | Manifest Value | Used by |
| --- | --- | --- |
| sys.information recursive | `sys_information_recursive` | All 21 drivers, all 12 adaptors, and the CLI tool |

Source: `DriverPackageManifest.csv` column `metadata_profile`.

---

## What the Profile Provides

The `sys_information_recursive` profile exposes catalog metadata through the
`sys.information.*` namespace of recursive catalog projections. Unlike flat information
schemas, these projections support recursive schema tree navigation — a schema can be a
parent of other schemas, and the projections let drivers traverse that tree.

Drivers expose metadata through named **metadata collections**. The following collections
are supported:

| Collection | Contents |
| --- | --- |
| `schemas` | Database schemas (supports recursive parent-expansion) |
| `tables` | Tables and views |
| `columns` | Columns within tables/views |
| `indexes` | Indexes |
| `index_columns` | Columns within indexes |
| `constraints` | Table constraints |
| `catalogs` | Database catalog entries |
| `primary_keys` | Primary key constraints |
| `foreign_keys` | Foreign key constraints |
| `procedures` | Stored procedures |
| `functions` | User-defined functions |
| `routines` | All routines (procedures + functions) |
| `table_privileges` | Table-level privilege grants |
| `column_privileges` | Column-level privilege grants |
| `type_info` | Type information |

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "first-class executable
metadata APIs on `Connection`" and "convenience metadata wrappers."

---

## Accessing Metadata

Drivers expose the following entry points for metadata access:

| Method | Purpose |
| --- | --- |
| `query_metadata(collection_name, restrictions)` | Execute a metadata query and return a cursor. Accepts a collection name (e.g., `'tables'`) and optional restriction filters. |
| `get_schema(collection_name, restrictions)` | Materialize metadata rows by draining cursor results. |
| `ddl_editor_schema_payload(schema_pattern, expand_schema_parents)` | Return a deterministic schema-navigation payload for DDL-editor consumers, including `schemaPaths` and a recursive `schemaTree`. |

Convenience wrappers are available for each collection (e.g., `.schemas()`, `.tables()`,
`.columns()`, `.primary_keys()`, etc.). Unsupported collections raise
`errors.NotSupportedError`.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md`.

---

## Restriction Filtering

Metadata queries accept restriction filters to narrow results. The filter system supports:

| Feature | Behavior |
| --- | --- |
| Wildcard matching | `%` (any sequence of characters) and `_` (any single character), with escape character support |
| Null matching | The string `"null"` matches NULL values |
| Alias-aware keys | Restriction keys are normalized through alias mappings |
| Unknown key handling | Unrecognized restriction keys are ignored rather than raising errors |

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "first-class metadata
restriction filtering."

---

## Recursive Schema Navigation

The `sys_information_recursive` profile supports recursive schema parent expansion. A schema
can be a child of another schema, and the driver can traverse the tree:

- `schema_paths_for_navigation(...)` — normalize, de-duplicate, and filter schema names;
  optionally enable parent expansion mode.
- `expand_schema_parent_paths(...)` — emit recursive dotted parent segments for
  metadata-only tree navigation.
- `build_schema_tree(...)` — construct a recursive `SchemaTreeNode` structure with
  per-parent uniqueness and terminal-node tracking.

The `metadata_expand_schema_parents` configuration option (available as a DSN key or
connection config alias) controls whether parent schemas are automatically expanded in
metadata queries. Recognized aliases include `metadataExpandSchemaParents`,
`metadata_expand_schema_parents`, `expand_schema_parents`, and `dbeaver_expand_schema_parents`.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "recursive schema tree
shaping."

---

## Schema Navigation Payload

The `ddl_editor_schema_payload()` method returns a deterministic payload for tools that
display a schema tree (DDL editors, BI tools, IDE plugins). The payload contains:

- `schemaPaths` — a list of normalized schema path strings.
- `schemaTree` — a recursive tree of `SchemaTreeNode` objects.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "deterministic
DDL-editor payload shaping."

---

## Query Resolution and Aliases

Metadata collection names are normalized before execution. The driver resolves aliases such
that a call to `.routines()` routes to the same metadata path as an explicit
`query_metadata('routines')`. Normalization also handles JDBC-style naming variations.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "`normalize_collection_name(...)`",
"`resolve_collection_query(...)`."

---

## Cross-References

- [conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) — META stage (S3) conformance requirement
- Language Reference: Catalog Reference (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) — canonical sys.information catalog structure




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/conformance_baseline.md -->

<a id="ch-client-driver-guide-conformance-baseline-md"></a>

# Conformance Baseline

## Purpose

This page describes the staged S1-S5 conformance baseline that all ScratchBird drivers are
validated against, the BASELINE_REQUIREMENT_MAPPING.md artifact each driver maintains, the
conformance gates referenced in the manifest, and what `beta_2` / `release_candidate` status
means for production use.

Sources used: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md`,
`project/drivers/DriverPackageManifest.csv`, `project/drivers/tool/cli/package_contract.json`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Release Status

The manifest records two status values for each component:

| Column | Value | Meaning |
| --- | --- | --- |
| `driver_status` | `beta_2` | The driver has passed second-beta validation. API surface is stabilizing but not yet frozen. |
| `release_bucket` | `release_candidate` | The component is in release-candidate state — it has cleared beta gates and is under final release testing. |

These designations apply to all 21 drivers, all 12 adaptors, and the CLI tool.

Source: `DriverPackageManifest.csv` columns `driver_status`, `release_bucket`.

**No component in this guide should be assumed production-certified** until the release gate
listed in its `conformance_profile_ref` has been formally closed. Live server verification and
release evidence are outstanding closure steps for most drivers.

---

## The S1–S5 Staged Baseline

Conformance is organized into five stages. Each stage builds on the previous.

| Stage | Short Name | Requirement Group |
| --- | --- | --- |
| S1 | CONN | Connection: DSN parsing, ingress mode, TLS, auth negotiation, session opening, session schema, liveness |
| S2 | TXN / EXEC | Transaction and execution: begin, commit, rollback, savepoints, prepared transactions, isolation levels, simple and extended query, batch, multi-result, callable, generated keys |
| S3 | META | Metadata: collection routing, restriction filtering, recursive schema navigation, DDL-editor payload |
| S4 | ERR / RES | Error and resource: error hierarchy, SQLSTATE mapping, retry boundaries, pool lifecycle, circuit breaker, keepalive, leak detection |
| S5 | TYPE | Type mapping: scalar, temporal, JSON, range, composite, vector, array, LOB wrappers, unknown-type fallback |

The naming convention `S1` through `S5` maps to the conformance profile reference in the
manifest (e.g., `driver_python_gate` references the Python driver's full stage completion).

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — "Status legend" and
table of PYTHONBL groups mapped to JDBC baseline groups.

---

## BASELINE_REQUIREMENT_MAPPING.md

Each driver maintains a `BASELINE_REQUIREMENT_MAPPING.md` file that records:

- The mapping of driver-specific groups (e.g., `PYTHONBL-CONN`) to the shared JDBC baseline
  groups (e.g., `JDBCBL-CONN`).
- The current implementation status of each group: `Implemented`, `Partial`, or `Missing`.
- The source and test anchors that provide evidence for each implemented group.
- Notes on known scope limits or outstanding steps.

The Python driver's BASELINE_REQUIREMENT_MAPPING records the `2026-04-03` JDBC/.NET-class
baseline closure date. A broader shared auth/bootstrap ratchet was introduced on `2026-04-17`.

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md`.

---

## Python Driver Baseline Status (as of source audit)

| Stage | JDBC Baseline Group | Status | Outstanding |
| --- | --- | --- | --- |
| S1 CONN | JDBCBL-CONN | Implemented | Live server verification and release evidence |
| S2 TXN | JDBCBL-TXN | Implemented | Live server verification and release evidence |
| S2 EXEC | JDBCBL-EXEC | Implemented | Live server verification and release evidence |
| S3 META | JDBCBL-META | Implemented | Live server verification and release evidence |
| S5 TYPE | JDBCBL-TYPE | Implemented | Live server verification and release evidence |
| S4 ERR | JDBCBL-ERR | Implemented | Live server verification and release evidence |
| S4 RES | JDBCBL-RES | Implemented | Live server verification and release evidence |

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — table rows.

**Note:** The Python driver's lane note states: "live server verification and release evidence
remain the outstanding closure step." Status `Implemented` means the behavior is present and
anchored by lane source and tests; it does not certify server-validated production readiness.

---

## MGA Recovery Contract

All drivers that implement the S2 (TXN/EXEC) stage follow ScratchBird's MGA (Multi-Generational
Architecture) recovery model:

- Reconnect or reopen repairs transport and session state only.
- Reconnect never resurrects abandoned in-flight transactions or replays lost statements.
- Transaction recovery means: reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states (after `PORTAL_SUSPENDED`).
- ScratchBird sessions are always in a transaction; `COMMIT` / `ROLLBACK` immediately reopen
  the next transaction boundary.
- Autocommit mode transitions are local driver policy on native lanes; the driver does not
  push a synthetic `SET_OPTION autocommit` wire message.
- `READ UNCOMMITTED` is a legacy compatibility alias; `REPEATABLE READ` maps to canonical
  `SNAPSHOT`; `SERIALIZABLE` maps to canonical `SNAPSHOT TABLE STABILITY`.

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — "MGA Recovery Contract".

---

## Conformance Gates

The manifest `conformance_profile_ref` column names the gate that must be closed before a
component is considered release-ready. The known gates are:

| Component | Gate |
| --- | --- |
| adbc | driver_adbc_gate |
| flightsql | driver_flightsql_gate |
| julia | driver_julia_gate |
| perl | driver_perl_gate |
| r2dbc | driver_r2dbc_gate |
| cpp | driver_cpp_gate |
| dart | driver_dart_gate |
| dotnet | driver_dotnet_gate |
| elixir | driver_elixir_gate |
| go | driver_go_gate |
| jdbc | driver_jdbc_gate |
| mojo | driver_mojo_gate |
| node | driver_node_gate |
| odbc | driver_odbc_gate |
| pascal | driver_pascal_gate |
| php | driver_php_gate |
| python | driver_python_gate |
| r | driver_r_gate |
| ruby | driver_ruby_gate |
| rust | driver_rust_gate |
| swift | driver_swift_gate |
| scratchbird-airbyte | adaptor_airbyte_gate |
| scratchbird-dbt-adapter | adaptor_dbt_gate |
| scratchbird-looker | adaptor_looker_gate |
| scratchbird-powerbi | adaptor_powerbi_gate |
| scratchbird-tableau | adaptor_tableau_gate |
| scratchbird-dbeaver-driver | adaptor_dbeaver_gate |
| scratchbird-hibernate-dialect | adaptor_hibernate_gate |
| scratchbird-metabase-driver | adaptor_metabase_gate |
| scratchbird-prisma-adapter | adaptor_prisma_gate |
| scratchbird-sqlalchemy-dialect | adaptor_sqlalchemy_gate |
| scratchbird-superset-driver | adaptor_superset_gate |
| scratchbird-typeorm-adapter | adaptor_typeorm_gate |
| CLI tool | tool_cli_gate |

Source: `DriverPackageManifest.csv` column `conformance_profile_ref`.

---

## CLI Conformance Tool

The CLI package includes `sbdriver_conformance` — a standalone conformance manifest runner.

Key points:
- A conformance manifest (`conformance/sbwp_conformance_manifest.sample.json`) describes
  test cases with typed assertions including `expect_columns`, `expect_column_type_oids`,
  `expect_first_row_json`, `expect_first_row_types`, and `expect_rows_json`.
- The runner (`conformance/run_sbdriver_conformance_sample.sh`) accepts `--binary-params`,
  `--text-params`, `--manifest`, `--output`, and `--no-build` flags.
- The runner requires `SB_CONFORMANCE_DSN` to be set to a live endpoint.

Source: `project/drivers/tool/cli/README.md` — "Conformance Sample".

---

## Building and Running Driver Gates

Gates are run via CTest from the project build system:

```bash
cmake -S project -B build/driver_gates -DSB_BUILD_DRIVER_GATES=ON
ctest --test-dir build/driver_gates -L driver --output-on-failure
```

Source: `project/drivers/README.md` — "CTest Gates".

---

## Cross-References

- [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — S1 CONN detail
- [authentication.md](#ch-client-driver-guide-authentication-md) — S1 CONN auth negotiation
- [type_mapping.md](#ch-client-driver-guide-type-mapping-md) — S5 TYPE detail
- [metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) — S3 META detail
- [diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — S4 ERR detail
- [pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) — S4 RES detail




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/cli_tools.md -->

<a id="ch-client-driver-guide-cli-tools-md"></a>

# CLI Tools

## Purpose

This page documents the ScratchBird CLI utilities: the primary tools (`sb_isql`, `sb_admin`,
`sb_backup`, `sb_security`, `sb_verify`) and the compatibility front-ends
(`sb_fb_isql`, `sb_my_isql`, `sb_pg_isql`). It also documents `sbdriver_conformance`,
the conformance manifest runner.

All claims are grounded in the source files and package contract:
`project/drivers/tool/cli/sb_isql.cpp`,
`project/drivers/tool/cli/sb_admin.cpp`,
`project/drivers/tool/cli/sb_backup.cpp`,
`project/drivers/tool/cli/sb_security.cpp`,
`project/drivers/tool/cli/sb_verify.cpp`,
`project/drivers/tool/cli/sb_fb_isql.cpp`,
`project/drivers/tool/cli/sb_my_isql.cpp`,
`project/drivers/tool/cli/sb_pg_isql.cpp`,
`project/drivers/tool/cli/README.md`,
`project/drivers/tool/cli/package_contract.json`.

This is a **draft**. CLI tools carry `beta_2` driver status and `release_candidate` release
bucket. Linux is supported with CI coverage; Windows is experimental (CI build attempt
enabled); macOS is untested and not currently in CI.

---

## CLI Component Identity

| Property | Value |
| --- | --- |
| component_id | `tool:cli` |
| driver_family | `native_cli` |
| driver_status | `beta_2` |
| release_bucket | `release_candidate` |
| wire_protocol | `sbwp_v1_1` |
| auth_authority | `engine` |
| transaction_authority | `mga_engine` |
| license | `MPL-2.0` |

Source: `project/drivers/tool/cli/package_contract.json`.

---

## Connection Modes

The network-backed CLI tools (`sb_isql`, `sb_admin`, `sb_security`) support four connection
modes via `--mode`:

| Flag Value | Description |
| --- | --- |
| `--mode=embedded` | Maps to local IPC transport (in-process or local IPC in the beta C++ network client) |
| `--mode=local-ipc` | IPC path to the running server SBPS Unix endpoint (`ipc_method` + `ipc_path` required) |
| `--mode=inet` | Direct TCP listener mode (`direct_listener` ingress) |
| `--mode=managed` | Manager proxy front-door mode (`manager_proxy` ingress) |

Full control is available via `--connection=<connection_string>` with optional
`--conn-opt key=value` overrides.

Source: `project/drivers/tool/cli/README.md` — "Connection Modes".

---

## Auth and Bootstrap Flags

The network-backed tools expose the shared staged auth/bootstrap contract:

| Flag | Purpose |
| --- | --- |
| `--probe-auth-surface` | Perform pre-connect auth negotiation and exit (no full session opened) |
| `--show-auth-context` | Print resolved auth context after a real connect |
| `--auth-token=<tok>` | Supply a generic token-auth payload |
| `--auth-method-id` | Explicit auth method identifier |
| `--auth-method-payload` | Auth method payload |
| `--auth-payload-json` | Auth payload in JSON form |
| `--auth-payload-b64` | Auth payload in base64 form |
| `--auth-provider-profile` | Auth provider profile selector |
| `--auth-required-methods` | Require specific auth methods |
| `--auth-forbidden-methods` | Forbid specific auth methods |
| `--auth-require-channel-binding` | Require channel binding |
| `--workload-identity-token` | Workload identity token |
| `--proxy-principal-assertion` | Proxy principal assertion |

Unsupported admitted methods remain fail-closed through the shared C++ driver surface.

Source: `project/drivers/tool/cli/README.md` — "Auth / Bootstrap".

---

## sb_isql — Interactive SBsql Shell

`sb_isql` is the primary interactive SQL shell for ScratchBird, using the SBsql dialect.
It is Firebird ISQL compatible with PostgreSQL psql extensions.

**Usage:**
```
sb_isql <database_path> [options]
```

**Key options:**

| Flag | Description |
| --- | --- |
| `-U, --user=<username>` | Username |
| `-P, --password=<pass>` | Password (prompted if absent) |
| `-p, --port=<n>` | TCP port (default: 3092) |
| `-H, --host=<host>` | Host (default: localhost) |
| `-c, --command=<sql>` | Execute a single SQL command and exit |
| `-f, --file=<file>` | Execute commands from a file and exit |
| `-o, --output=<file>` | Write output to a file |
| `-t, --tuples-only` | Print tuples only (suppress headers/footers) |
| `-A, --no-align` | Unaligned output mode |
| `-F, --field-separator=<s>` | Field separator (default: `|`) |
| `-q, --quiet` | Quiet mode |
| `-e, --echo` | Echo commands before execution |
| `-b, --bail` | Stop on first error |
| `-v, --verbose` | Verbose mode |
| `--schema-tree` | Print schema tree and exit |

**SET commands (Firebird ISQL compatible):**
`SET BAIL`, `SET TERM`, `SET COUNT`, `SET HEADING`, `SET ECHO`, `SET LIST`, `SET NULL`,
`SET WIDTH`, `SET STATS`, `SET PLAN`, `SET PLANONLY`, `SET EXPLAIN`.

**Meta-commands (psql-style, start with `\`):**
`\?` (help), `\q` (quit), `\d` / `\dt` (list or describe tables), `\di` (indexes),
`\du` (users), `\l` (databases), `\c <database>` (connect to database).

Source: `project/drivers/tool/cli/sb_isql.cpp` — file header doc comment.

---

## sb_admin — Administration Tool

`sb_admin` provides scheduler administration and engine metrics access.

**Usage:**
```
sb_admin <database> <subcommand> [options]
```

**Subcommands:**

| Subcommand | Description |
| --- | --- |
| `job list [--like <pattern>]` | List scheduled jobs |
| `job runs <job_name>` | Show execution history for a job |
| `metrics` | Show engine metrics |
| `jit <compile\|rebuild\|inspect> <object_uuid>` | JIT compile, rebuild, or inspect an object |
| `jit retire <artifact_uuid>` | Retire a JIT artifact |

**Key options:** `-U/--user`, `-P/--password`, `-p/--port`, `--database=<name>`, `-q/--quiet`.

Source: `project/drivers/tool/cli/sb_admin.cpp` — file header doc comment.

---

## sb_backup — Backup and Restore Tool

`sb_backup` creates and restores ScratchBird database backups. Backups use the `.sbbak` format
with optional LZ4 compression and CRC32c integrity checksums.

**Usage:**
```
sb_backup <command> [options]
```

**Commands:**

| Command | Description |
| --- | --- |
| `backup <database> <backup_file>` | Create a backup |
| `restore <backup_file> <database>` | Restore from a backup |
| `verify <backup_file>` | Verify backup file integrity |
| `info <backup_file>` | Show backup metadata |

**Key options:** `--compress`, `--no-compress`, `-v/--verbose`, `-q/--quiet`, `-p/--progress`.

For operator procedures covering backup scheduling, restore validation, and data movement,
see Operations Administration: Backup, Restore, and Data Movement (ScratchBird — Operations, Security, and Autonomy, page XXX).

Source: `project/drivers/tool/cli/sb_backup.cpp` — file header doc comment.

---

## sb_security — Security Administration Tool

`sb_security` manages users, roles, permissions, and audit configuration.

**Usage:**
```
sb_security <command> <database> [options]
```

**Command groups:**

| Group | Commands |
| --- | --- |
| User management | `user list`, `user create`, `user delete`, `user password`, `user enable`, `user disable`, `user info`, `user unlock` |
| Role management | `role list`, `role create`, `role delete`, `role grant`, `role revoke`, `role members`, `role grants` |
| Permission | `grant <privilege> on <object> to <user/role>`, `revoke <privilege> on <object> from <user/role>`, `show grants for <user/role>`, `show grants on <object>` |
| Audit | `audit status`, `audit enable [category]`, `audit disable [category]`, `audit log [filter]` |
| Security checks | `check`, `check passwords`, `check permissions`, `check audit` |

**Key options:** `-U/--user`, `-P/--password`, `-p/--port`, `-v/--verbose`, `-q/--quiet`,
`--json` (JSON output format).

For engine-side user and role management policy, see
Security Guide: standard_roles_and_groups.md (ScratchBird — Operations, Security, and Autonomy, page XXX) and
Security Guide: grants_and_privileges.md (ScratchBird — Operations, Security, and Autonomy, page XXX).

Source: `project/drivers/tool/cli/sb_security.cpp` — file header doc comment.

---

## sb_verify — Database Verification Tool

`sb_verify` checks database integrity and page consistency. It is used for offline verification
of a database file, not for live connection health.

**Usage:**
```
sb_verify <database_path> [options]
```

**Key options:**

| Flag | Description |
| --- | --- |
| `--full` | Full verification (all checks) |
| `--quick` | Quick check (header and page checksums only) |
| `--pages` | Verify all pages |
| `--repair` | Attempt repair (dangerous — use with caution) |
| `-v/--verbose` | Verbose output |
| `-q/--quiet` | Only show errors |
| `-o/--output=<file>` | Write report to file |

**Exit codes:**

| Code | Meaning |
| --- | --- |
| `0` | No issues found |
| `1` | Minor issues (warnings) |
| `2` | Serious issues (errors) |
| `3` | Critical issues (corruption) |
| `4` | Usage or argument error |

Source: `project/drivers/tool/cli/sb_verify.cpp` — file header doc comment.

---

## sbdriver_conformance — Conformance Manifest Runner

`sbdriver_conformance` runs typed conformance assertions against a live ScratchBird endpoint
using a JSON manifest.

**Manifest assertions supported:**
`expect_columns`, `expect_column_type_oids`, `expect_first_row_json`,
`expect_first_row_types`, `expect_rows_json`.

**Usage:**
```bash
export SB_CONFORMANCE_DSN="scratchbird://user:pass@localhost:3092/mydb?protocol=native"
lanes/active/tooling/cli/conformance/run_sbdriver_conformance_sample.sh
```

**Runner flags:** `--binary-params`, `--text-params`, `--manifest <path>`,
`--output <path>`, `--no-build`.

Source: `project/drivers/tool/cli/README.md` — "Conformance Sample".

---

## Compatibility Front-Ends

### sb_fb_isql — Firebird SQL Compatibility Shell

`sb_fb_isql` provides a Firebird SQL syntax compatibility front-end. It uses the
`FirebirdQueryCompiler` to parse Firebird SQL and compile it to SBLR bytecode for execution.

Key options: `-c/--command`, `-f/--file`, `-q/--quiet`, `-s/--dialect=<1|2|3>` (default: 3),
`--stats`, `--version`.

Source: `project/drivers/tool/cli/sb_fb_isql.cpp` — file header doc comment and includes.

### sb_my_isql — MySQL Compatibility Shell

`sb_my_isql` provides a MySQL wire protocol compatibility front-end for testing. It uses
the `mysql_adapter` from the FDW (Foreign Data Wrapper) layer.

Source: `project/drivers/tool/cli/sb_my_isql.cpp` — file header doc comment and includes.

### sb_pg_isql — PostgreSQL Compatibility Shell

`sb_pg_isql` provides a PostgreSQL wire protocol compatibility front-end for testing. It uses
the `postgresql_adapter` from the FDW layer.

Source: `project/drivers/tool/cli/sb_pg_isql.cpp` — file header doc comment and includes.

**Build note:** The compatibility front-ends (`sb_pg_isql`, `sb_my_isql`, `sb_fb_isql`) require
FDW adapters from the engine repository and are built with `-DSB_BUILD_CLI_FDW=ON`.

Source: `project/drivers/tool/cli/README.md` — "Optional: `-DSB_BUILD_CLI_FDW=ON`".

---

## Build

```bash
cmake -S . -B build_cli -DSB_BUILD_CLI=ON -DSB_BUILD_CPP=ON -DSB_BUILD_ODBC=OFF
cmake --build build_cli --config Release
ctest --test-dir build_cli --output-on-failure
```

Expected build outputs:
`bin/sb_isql`, `bin/sb_admin`, `bin/sb_backup`, `bin/sb_security`, `bin/sb_verify`,
`bin/sbdriver_conformance`.

Source: `project/drivers/tool/cli/package_contract.json` — `artifact_verification`.

---

## Authority Boundaries

The CLI tools follow these authority boundaries (from `package_contract.json`):

- The CLI may shape command-line options, prompts, and conformance manifests.
- The C++ driver (`driver:cpp`) handles transport and protocol binding.
- The server revalidates authentication, authorization, SBLR, UUID, cache, schema, and
  transaction claims.
- MGA transaction finality remains engine-owned.

The CLI never auto-replays a whole transaction. Retry rule: `40xxx` requires a fresh statement
boundary; `08xxx` requires reconnect or reopen.

Source: `project/drivers/tool/cli/package_contract.json` — `delegation_posture`.

---

## Cross-References

- [connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN keys and ingress modes used by the CLI
- [authentication.md](#ch-client-driver-guide-authentication-md) — auth methods the CLI tools negotiate
- [conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) — sbdriver_conformance and the S1-S5 gates
- Operations Administration: Backup, Restore, and Data Movement (ScratchBird — Operations, Security, and Autonomy, page XXX) — operator backup procedures
- Operations Administration: Service Lifecycle (ScratchBird — Operations, Security, and Autonomy, page XXX) — service state context for CLI operations
- Security Guide: authentication_and_providers.md (ScratchBird — Operations, Security, and Autonomy, page XXX) — engine-side auth provider detail




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/python.md -->

<a id="ch-client-driver-guide-drivers-python-md"></a>

# ScratchBird Python Driver — DB-API 2.0 (PEP 249)

The ScratchBird Python driver gives Python applications access to a ScratchBird
Convergent Data Engine (CDE) through the standard DB-API 2.0 interface defined
by PEP 249. It speaks the ScratchBird Native Wire Protocol (SBWP v1.1) directly
and does not depend on any intermediate ODBC or JDBC layer.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0012-7000-8000-000000000012` |
| `api_surface_set` | `dbapi_2` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_python_gate` |

---

## Installation

The package is distributed as `scratchbird` (PyPI name). Build a wheel from
source using `setuptools`:

```bash
python -m pip install build
python -m build
python -m pip install dist/scratchbird-0.1.0-py3-none-any.whl
```

Or install in editable mode for development:

```bash
python -m pip install -e .
```

Requires Python 3.8 or later. License: MPL-2.0.
Source: `project/drivers/driver/python/` — `pyproject.toml`, package name
`scratchbird`, version `0.1.0`.

---

## Connecting

The driver accepts two DSN forms. The entry point is `scratchbird.connect(...)`,
which returns a `Connection` object.

### URI form

```
scratchbird://user:password@host:3092/database?sslmode=require
```

### Key-value form (whitespace or semicolon separated)

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass sslmode=require
```

Accepted aliases: `dbname` -> `database`, `username` -> `user`,
`connecttimeout` -> `connect_timeout`.

### Minimal connection example

```python
import scratchbird

conn = scratchbird.connect(
    "scratchbird://alice:secret@db.example.com:3092/prod?sslmode=require"
)
```

Or using keyword arguments:

```python
conn = scratchbird.connect(
    host="db.example.com",
    port=3092,
    database="prod",
    user="alice",
    password="secret",
    sslmode="require",
)
```

### Token auth

```python
conn = scratchbird.connect(
    host="db.example.com",
    port=3092,
    database="prod",
    user="alice",
    auth_method_id="scratchbird.auth.token",
    auth_token="<bearer-token>",
)
```

### Manager-proxy ingress

```python
conn = scratchbird.connect(
    host="proxy.example.com",
    port=3090,
    database="prod",
    user="alice",
    password="secret",
    front_door_mode="manager_proxy",
    manager_auth_token="<proxy-token>",
)
```

### Auth preflight probe

```python
result = scratchbird.probe_auth_surface(
    "scratchbird://alice@db.example.com:3092/prod"
)
```

The `Connection.get_resolved_auth_context()` method returns the auth surface
resolved after connect. Directly executable auth classes: `PASSWORD`,
`SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`. `MD5`, `PEER`, and `REATTACH` are
fail-closed.

The default port is `3092`. The default `sslmode` is `require`.

---

## Executing statements and transactions

### Cursors

```python
cur = conn.cursor()
cur.execute("SELECT id, name FROM users WHERE region = %s", ("west",))
rows = cur.fetchall()
```

Parameter style is `pyformat` (`%s` positional or `%(name)s` named). Named
parameters with `::` cast syntax are handled correctly — the cast marker is not
mistaken for a named placeholder.

### executemany

```python
cur.executemany(
    "INSERT INTO events (ts, kind) VALUES (%s, %s)",
    [(t1, "click"), (t2, "view")],
)
```

Repeated identical `INSERT ... VALUES` batch shapes reuse a session-local
prepared statement handle to reduce parse overhead for high-volume loads.
`seq_of_params` must not be `None`.

### Generated keys and status

```python
cur.execute("INSERT INTO orders (amount) VALUES (%s) RETURNING id", (99.5,))
print(cur.lastrowid)       # last generated key from COMMAND_COMPLETE
print(cur.statusmessage)   # command tag string
```

### Multi-result traversal

```python
cur.execute("SELECT 1; SELECT 2")
print(cur.fetchall())
cur.nextset()
print(cur.fetchall())
```

### Transaction control

ScratchBird sessions are always in a transaction. `COMMIT` and `ROLLBACK`
immediately reopen the next boundary.

```python
conn.begin()                    # start an explicit transaction
conn.execute("UPDATE ...")      # via Connection.execute shortcut
conn.commit()

conn.begin()
conn.rollback()

sp = conn.savepoint("sp1")
conn.rollback_to_savepoint("sp1")
conn.release_savepoint("sp1")
```

Isolation levels: the canonical MGA mapping is:
- `READ UNCOMMITTED` — legacy compatibility alias only
- `READ COMMITTED` — canonical `READ COMMITTED`
- `REPEATABLE READ` — canonical `SNAPSHOT`
- `SERIALIZABLE` — canonical `SNAPSHOT TABLE STABILITY`

Use `canonical_isolation_label(level)` and
`canonical_read_committed_mode_label(mode)` to inspect the current mapping.
`READ_COMMITTED_MODE_READ_CONSISTENCY` selects canonical `READ COMMITTED READ
CONSISTENCY`.

### Autocommit

```python
conn.autocommit = True   # commits any active transaction first, then switches
conn.autocommit = False  # eagerly starts a transaction if none is active
```

Autocommit transitions are local driver policy; the wrapper does not push a
synthetic wire `SET_OPTION autocommit` for the native lane.

### Batch and multi-result convenience APIs

```python
summary = conn.execute_batch(
    "INSERT INTO t (x) VALUES (%s)",
    [(1,), (2,), (3,)],
)
# returns: totalRowCount + per-item (index, rowCount, fields, command, lastId)

results = conn.query_multi("SELECT 1; SELECT 2", None)
# returns: list of {rows, rowCount, fields, command, lastId}
```

### Retry scope

```python
from scratchbird import retry_scope_for_sqlstate
scope = retry_scope_for_sqlstate("40001")
# "statement" — retry at a fresh statement boundary
```

`40001`/`40P01` → `RETRY_SCOPE_STATEMENT`;
`08xxx` → `RETRY_SCOPE_RECONNECT`;
all others → `RETRY_SCOPE_NONE`.

---

## Type mapping

The `sbsql_core` type-mapping profile applies. The table below lists canonical
SBsql types and their Python representations.

| SBsql canonical type | Python type | Notes |
|---|---|---|
| `BOOLEAN` | `bool` | text tokens `true`/`t` → `True`, others → `False` |
| `SMALLINT` (`INT2`) | `int` | |
| `INTEGER` (`INT4`) | `int` | |
| `BIGINT` (`INT8`) | `int` | unknown-text integer capped at signed int64 |
| `REAL` (`FLOAT4`) | `float` | |
| `DOUBLE PRECISION` (`FLOAT8`) | `float` | |
| `NUMERIC` / `DECIMAL` | `decimal.Decimal` | |
| `TEXT` / `VARCHAR` / `CHAR` | `str` | |
| `BYTEA` | `bytes` | hex (`\x`/`0x`) and octal-escape decode |
| `DATE` | `datetime.date` | |
| `TIME` | `datetime.time` (naive) | rejects timezone-offset payloads |
| `TIMETZ` | `datetime.time` (offset-aware) | 12-byte binary: micros + zone seconds west |
| `TIMESTAMP` | `datetime.datetime` (naive) | |
| `TIMESTAMPTZ` | `datetime.datetime` (UTC-aware) | |
| `UUID` | `uuid.UUID` | |
| `JSON` / `JSONB` | `scratchbird.Json` / `scratchbird.Jsonb` | |
| `XML` | `scratchbird.SqlXml` | |
| array types | `list` | typed OID inference for bool/int/float/text/date/time/timetz/timestamp/timestamptz/numeric/uuid/bytea |
| range types | `scratchbird.Range` | string temporal bounds coerced to UTC binary |
| geometry | `scratchbird.Geometry` | |
| `BLOB` | `scratchbird.Blob` → `OID_BYTEA` | |
| `CLOB` | `scratchbird.Clob` → `OID_TEXT` | |
| `ROWID` | `scratchbird.RowId` → `OID_BYTEA` | |
| `REF` | `scratchbird.Ref` → `OID_TEXT` | |
| `SQLXML` | `scratchbird.SqlXml` → `OID_XML` | |

Python `Enum` values encode as text using the member name.
Custom objects fall back to `str(value)`.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the shared `sbsql_core`
profile.

---

## Metadata and introspection

The driver exposes `sys.information.*` through collection-based metadata APIs
on `Connection`:

```python
tables = conn.get_schema("tables", restrictions={"TABLE_SCHEMA": "public"})
schemas = conn.get_schema("schemas")
pks = conn.get_schema("primary_keys", restrictions={"TABLE_NAME": "orders"})
```

Supported collections: `tables`, `columns`, `schemas`, `indexes`,
`index_columns`, `constraints`, `catalogs`, `primary_keys`, `foreign_keys`,
`procedures`, `functions`, `routines`, `table_privileges`,
`column_privileges`, `type_info`.

Restrictions support `%`/`_` wildcard matching (JDBC-style, with escape
handling) and `"null"` literal matching.

Recursive schema-tree navigation:

```python
payload = conn.ddl_editor_schema_payload(
    schema_pattern="pub%",
    expand_schema_parents=True,
)
# returns: {schemaPaths: [...], schemaTree: {...}}
```

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

The driver implements the full DB-API 2.0 exception hierarchy:

| Class | Meaning |
|---|---|
| `scratchbird.Warning` | Non-fatal warning |
| `scratchbird.InterfaceError` | Driver API misuse |
| `scratchbird.DatabaseError` | Base for all server errors |
| `scratchbird.DataError` | Bad data value |
| `scratchbird.OperationalError` | Connection / server errors |
| `scratchbird.IntegrityError` | Constraint violation |
| `scratchbird.InternalError` | Server internal error |
| `scratchbird.ProgrammingError` | SQL syntax / API error |
| `scratchbird.NotSupportedError` | Unsupported operation |

SQLSTATE codes from the server are preserved in `exception.args` alongside
DETAIL and HINT fields. SQL normalization `ValueError` is mapped to
`ProgrammingError`.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The driver is `thread_safe` (one connection per thread is the safe default for
DB-API). The `scratchbird.ConnectionPool` class provides explicit pooling with
configurable min/max size, checkout/reuse, stale-connection replacement,
statement caching (`StatementCache`), retry backoff (`retry_with_backoff`),
circuit-breaker support (`CircuitBreaker`), and keepalive validation. Each
pooled connection resets cached borrow state before reuse.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

Conformance gate: `driver_python_gate`. All JDBCBL groups (CONN, TXN, EXEC,
META, TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/jdbc.md -->

<a id="ch-client-driver-guide-drivers-jdbc-md"></a>

# ScratchBird JDBC Driver — JDBC 4.x

The ScratchBird JDBC driver is a pure-Java Type 4 driver that connects Java
applications to a ScratchBird Convergent Data Engine (CDE) over the ScratchBird
Native Wire Protocol (SBWP v1.1). It requires no native shared library and
implements the standard `java.sql` / `javax.sql` interfaces defined by JDBC 4.x.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0006-7000-8000-000000000006` |
| `api_surface_set` | `jdbc_4_x` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_jdbc_gate` |

---

## Installation and packaging

The driver is published as a Maven artifact:

```
groupId:    com.scratchbird
artifactId: scratchbird-jdbc
version:    0.1.0
```

Build from source with Gradle (requires JDK 17):

```bash
./gradlew build        # Linux / macOS
gradlew.bat build      # Windows
```

The resulting JAR includes `Automatic-Module-Name: com.scratchbird.jdbc`.
Minimum Java version: 17.

Integration test environment variables:
- `SCRATCHBIRD_JDBC_URL`
- `SCRATCHBIRD_JDBC_USER`
- `SCRATCHBIRD_JDBC_PASSWORD`

---

## Connecting

### JDBC URL form

```
jdbc:scratchbird://host:3092/database
```

Query parameters accepted on the URL: `sslmode`, `user`, `password`,
`auth_token`, `front_door_mode`, `manager_auth_token`, and startup plugin
selection fields.

### Loading the driver

```java
import com.scratchbird.jdbc.SBDriver;

// Auto-registration via ServiceLoader (Java 9+):
Connection conn = DriverManager.getConnection(
    "jdbc:scratchbird://db.example.com:3092/prod",
    "alice", "secret"
);

// Or construct directly:
SBDriver driver = new SBDriver();
Connection conn = driver.connect(
    "jdbc:scratchbird://db.example.com:3092/prod", props
);
```

### Connection properties (`SBConnectionProperties`)

Key properties (passed via `java.util.Properties` or a
`SBConnectionProperties` object):

| Key | Description |
|---|---|
| `user` | Database user |
| `password` | Password for `PASSWORD`/`SCRAM` auth |
| `auth_token` | Bearer token for `TOKEN` auth |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct` (default) or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |

### Auth preflight probe

```java
SBAuthProbeResult probe = SBDriver.probeAuthSurface(url, props);
// or on an existing connection:
SBAuthProbeResult probe = conn.probeAuthSurface(props);
SBResolvedAuthContext ctx = conn.getResolvedAuthContext();
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed (`0A000`).

The default port is `3092`; the default `sslmode` is `require`.

---

## Executing statements and transactions

### Statement execution

```java
try (Statement stmt = conn.createStatement()) {
    ResultSet rs = stmt.executeQuery("SELECT id, name FROM users WHERE id = 1");
    while (rs.next()) {
        System.out.println(rs.getLong("id") + " " + rs.getString("name"));
    }
}
```

### Prepared statements with parameter binding

```java
try (PreparedStatement ps = conn.prepareStatement(
        "INSERT INTO events (ts, kind) VALUES (?, ?)")) {
    ps.setTimestamp(1, ts);
    ps.setString(2, "click");
    ps.executeUpdate();
    System.out.println(ps.getLargeUpdateCount());
}
```

### Callable statements (stored procedures)

```java
try (CallableStatement cs = conn.prepareCall("{ call my_proc(?, ?) }")) {
    cs.setInt(1, 42);
    cs.setString(2, "arg");
    cs.execute();
}
```

### Multiple result sets

```java
stmt.execute("SELECT 1; SELECT 2");
do {
    ResultSet rs = stmt.getResultSet();
    // consume rs
} while (stmt.getMoreResults());
```

### Generated keys

```java
stmt.executeUpdate(
    "INSERT INTO orders (amount) VALUES (99.5)",
    Statement.RETURN_GENERATED_KEYS
);
ResultSet keys = stmt.getGeneratedKeys();
```

### Transaction control

ScratchBird sessions are always in a transaction. `COMMIT` / `ROLLBACK`
immediately reopen the next boundary.

```java
conn.setAutoCommit(false);
conn.commit();
conn.rollback();

Savepoint sp = conn.setSavepoint("sp1");
conn.rollback(sp);
conn.releaseSavepoint(sp);
```

### Isolation levels and canonical MGA mapping

The `setTransactionIsolation(int)` mapping:

| JDBC constant | Canonical MGA mode |
|---|---|
| `TRANSACTION_READ_UNCOMMITTED` | Legacy alias only |
| `TRANSACTION_READ_COMMITTED` | `READ COMMITTED` |
| `TRANSACTION_REPEATABLE_READ` / `SNAPSHOT` | `SNAPSHOT` |
| `TRANSACTION_SERIALIZABLE` | `SNAPSHOT TABLE STABILITY` |

The `READ COMMITTED` sub-mode selector:

```java
conn.setReadCommittedMode(SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY);
// or using the two-arg form:
conn.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED,
    SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY);
```

`canonicalReadCommittedModeLabel(mode)` returns the canonical MGA label
string for audit and test purposes.

### Prepared / limbo transaction control

```java
conn.prepareTransaction("xid1");
conn.commitPrepared("xid1");
conn.rollbackPrepared("xid1");
```

`supportsDormantReattach()` returns `false` on this driver; `detachToDormant()`
and `reattachDormant(...)` fail closed with `0A000`.

### Retry scope

`SBProtocolHandler.retryScopeForSqlState(sqlstate)` returns
`SBRetryScope.STATEMENT` for `40001`/`40P01`,
`SBRetryScope.RECONNECT` for `08xxx`, and
`SBRetryScope.NONE` otherwise.

---

## Type mapping

The `sbsql_core` profile applies. Key JDBC type mappings:

| SBsql canonical type | Java type (`getObject`) | JDBC type constant |
|---|---|---|
| `BOOLEAN` | `Boolean` | `Types.BOOLEAN` |
| `SMALLINT` | `Short` | `Types.SMALLINT` |
| `INTEGER` | `Integer` | `Types.INTEGER` |
| `BIGINT` | `Long` | `Types.BIGINT` |
| `REAL` | `Float` | `Types.REAL` |
| `DOUBLE PRECISION` | `Double` | `Types.DOUBLE` |
| `NUMERIC` / `DECIMAL` | `java.math.BigDecimal` | `Types.NUMERIC` |
| `TEXT` / `VARCHAR` | `String` | `Types.VARCHAR` |
| `BYTEA` | `byte[]` | `Types.BINARY` |
| `DATE` | `java.sql.Date` | `Types.DATE` |
| `TIME` | `java.sql.Time` | `Types.TIME` |
| `TIMETZ` | `java.sql.Time` (offset-aware) | `Types.TIME_WITH_TIMEZONE` |
| `TIMESTAMP` | `java.sql.Timestamp` | `Types.TIMESTAMP` |
| `TIMESTAMPTZ` | `java.sql.Timestamp` (UTC) | `Types.TIMESTAMP_WITH_TIMEZONE` |
| `UUID` | `java.util.UUID` | `Types.OTHER` |
| `JSON` / `JSONB` | `SBJsonb` | `Types.OTHER` |
| `XML` | `SBSQLXML` | `Types.SQLXML` |
| array types | `SBArray` | `Types.ARRAY` |
| range types | `SBRange` | `Types.OTHER` |
| geometry | `SBGeometry` | `Types.OTHER` |
| `BLOB` | `SBBlob` | `Types.BLOB` |
| `CLOB` | `SBClob` | `Types.CLOB` |
| `REF` | `SBRef` | `Types.REF` |
| `ROWID` | `SBRowId` | `Types.ROWID` |

Type encoding and decoding is handled by `SBTypeCodec`. Extended type retrieval
uses `ResultSet.getObject(col, Class<T>)`.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Metadata and introspection

`DatabaseMetaData` is exposed through `SBDatabaseMetaData`:

```java
DatabaseMetaData meta = conn.getMetaData();
ResultSet tables = meta.getTables(null, "public", "%", new String[]{"TABLE"});
ResultSet cols   = meta.getColumns(null, "public", "orders", "%");
ResultSet pks    = meta.getPrimaryKeys(null, "public", "orders");
```

`ResultSetMetaData` is returned by `SBResultSetMetaData`.
All metadata queries target `sys.information.*` views on the engine.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Server errors arrive as `java.sql.SQLException` subclasses. SQLSTATE codes are
preserved in `SQLException.getSQLState()`. The retry-scope helper
`SBProtocolHandler.retryScopeForSqlState(sqlstate)` classifies errors for
retry decisions. Connection failures use `08xxx` SQLSTATE codes;
constraint violations use `23xxx`.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The driver is `thread_safe`. `SBConnectionPool` provides connection pooling with
lease/borrow lifecycle, stale-connection eviction, and keepalive support. For
applications using a third-party pool (HikariCP, c3p0, etc.), the driver
implements `javax.sql.DataSource` through `SBConnectionPool`. Pool-reset logic
rolls back abandoned transaction state on checkout.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

Conformance gate: `driver_jdbc_gate`. All JDBCBL groups (CONN, TXN, EXEC,
META, TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/odbc.md -->

<a id="ch-client-driver-guide-drivers-odbc-md"></a>

# ScratchBird ODBC Driver — ODBC 3.x (3.8)

The ScratchBird ODBC driver is a shared-library ODBC 3.8 driver (Windows: DLL,
Linux: `.so`) that connects ODBC-capable applications to a ScratchBird
Convergent Data Engine (CDE) over the ScratchBird Native Wire Protocol (SBWP
v1.1). Applications link against the standard ODBC Driver Manager; no
ScratchBird-specific library is loaded by application code.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0009-7000-8000-000000000009` |
| `api_surface_set` | `odbc_3_x` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `dsn`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_odbc_gate` |

---

## Building the driver

The driver is built with CMake (C++17, C99). Platform targets: Linux and
Windows (CI-covered); macOS is untested.

```bash
cmake -S . -B build
cmake --build build --config Release
```

The CMake project name is `scratchbird_odbc`, version `0.1.0`. The shared
library output is `scratchbird_odbc` (`.so` on Linux, `.dll` on Windows). See
`docs/BUILD_MATRIX.md` for required ODBC Manager and OpenSSL dependencies.

---

## Connecting

### Connection string (direct/native)

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;PWD=pass;SSLMode=prefer
```

### Connection string (token auth)

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;AuthMethodId=scratchbird.auth.token;AuthToken=token
```

`AuthToken`, `BearerToken`, and `Token` are accepted aliases and normalized to
the bridge config.

### Connection string (manager-proxy ingress)

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3090;Database=mydb;UID=user;PWD=pass;FrontDoorMode=manager_proxy;ManagerAuthToken=token
```

### DSN entry (odbc.ini / registry)

The driver registers under the name `ScratchBird`. `DSN=<name>` is an accepted
DSN key in the connection string.

### Key reference

| Key | Description |
|---|---|
| `Server` / `Host` | Engine hostname or IP |
| `Port` | Engine port (default `3092`) |
| `Database` | Target database name |
| `UID` / `User` | Login user |
| `PWD` / `Password` | Login password |
| `SSLMode` | `disable`, `prefer`, `require`, `verify-ca`, `verify-full` |
| `AuthMethodId` | Auth plugin identifier |
| `AuthToken` / `BearerToken` / `Token` | Bearer token value |
| `FrontDoorMode` | `direct` (default) or `manager_proxy` |
| `ManagerAuthToken` | Proxy bearer token |

### Auth preflight probe

```c
// C-level via OdbcClientBridge:
OdbcClientBridge::probeAuthSurface(connectionString, outResult);
OdbcClientBridge::getResolvedAuthContext();
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed.

---

## Executing statements and transactions

### Basic execution (SQLExecDirect / SQLExecute)

```c
SQLHSTMT hstmt;
SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
SQLExecDirect(hstmt,
    (SQLCHAR*)"SELECT id FROM users WHERE region = ?", SQL_NTS);
```

`SQL_SUCCESS_WITH_INFO` is treated as successful for both `SQLExecute` and
`SQLExecDirect` (not as a hard failure).

### Parameter binding (SQLBindParameter)

```c
SQLINTEGER region_len = SQL_NTS;
SQLCHAR region[32] = "west";
SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
    31, 0, region, sizeof(region), &region_len);
```

### Transaction control

```c
SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT,
    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);

// commit or rollback at connection level:
SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_COMMIT);
SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK);

// ENV-handle fan-out commits all connected child connections:
SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT);
```

After `SQLEndTran(ROLLBACK)`, the next query is immediately usable on the
reopened MGA boundary with no reconnect.

### Isolation level mapping

Set via `SQL_ATTR_TXN_ISOLATION`:

| ODBC constant | Canonical MGA mode |
|---|---|
| `SQL_TXN_READ_UNCOMMITTED` | Legacy alias only |
| `SQL_TXN_READ_COMMITTED` | `READ COMMITTED` |
| `SQL_TXN_REPEATABLE_READ` / `VERSIONING` | `SNAPSHOT` |
| `SQL_TXN_SERIALIZABLE` | `SNAPSHOT TABLE STABILITY` |

A distinct `READ COMMITTED READ CONSISTENCY` selector is not yet exposed through
the `SQL_ATTR_TXN_ISOLATION` surface in this driver version.

### Prepared / dormant / portal capabilities

| Capability | Value |
|---|---|
| `supportsPreparedTransactions()` | present; builds canonical SQL |
| `supportsDormantReattach()` | false — fail-closed |
| `supportsPortalResume()` | false — intentionally absent |

Retry is SQLSTATE-driven: `40001`/`40P01` → fresh statement boundary;
`08xxx` → reconnect or reopen. The retry helper stops immediately on operator-
intervention diagnostics such as `57014`.

---

## Type mapping

The `sbsql_core` profile applies. Key ODBC C-type / SQL-type mappings:

| SBsql canonical type | ODBC SQL type | C type |
|---|---|---|
| `BOOLEAN` | `SQL_BIT` | `SQL_C_BIT` |
| `SMALLINT` | `SQL_SMALLINT` | `SQL_C_SHORT` |
| `INTEGER` | `SQL_INTEGER` | `SQL_C_LONG` |
| `BIGINT` | `SQL_BIGINT` | `SQL_C_SBIGINT` |
| `REAL` | `SQL_REAL` | `SQL_C_FLOAT` |
| `DOUBLE PRECISION` | `SQL_DOUBLE` | `SQL_C_DOUBLE` |
| `NUMERIC` / `DECIMAL` | `SQL_NUMERIC` | `SQL_C_NUMERIC` |
| `TEXT` / `VARCHAR` | `SQL_VARCHAR` | `SQL_C_CHAR` / `SQL_C_WCHAR` |
| `BYTEA` | `SQL_VARBINARY` | `SQL_C_BINARY` |
| `DATE` | `SQL_TYPE_DATE` | `SQL_C_TYPE_DATE` |
| `TIME` | `SQL_TYPE_TIME` | `SQL_C_TYPE_TIME` |
| `TIMETZ` | `SQL_TYPE_TIME` | `SQL_C_TYPE_TIME` |
| `TIMESTAMP` | `SQL_TYPE_TIMESTAMP` | `SQL_C_TYPE_TIMESTAMP` |
| `TIMESTAMPTZ` | `SQL_TYPE_TIMESTAMP` | `SQL_C_TYPE_TIMESTAMP` |
| `UUID` | `SQL_GUID` | `SQL_C_GUID` |

Type information is exposed through `SQLGetTypeInfo`. Unicode handling uses
`SQL_C_WCHAR` on Windows.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Metadata and introspection

Catalog functions expose `sys.information.*`:

```c
SQLTables(hstmt, NULL, 0, (SQLCHAR*)"public", SQL_NTS, NULL, 0, NULL, 0);
SQLColumns(hstmt, NULL, 0, (SQLCHAR*)"public", SQL_NTS,
    (SQLCHAR*)"orders", SQL_NTS, NULL, 0);
SQLPrimaryKeys(hstmt, NULL, 0, (SQLCHAR*)"public", SQL_NTS,
    (SQLCHAR*)"orders", SQL_NTS);
```

Recursive schema-tree navigation is supported through `SQLBrowseConnect`:
database → default branch rows → dotted parent expansion → leaf tables.
`SQLBrowseConnect` uses slash-delimited path splitting to preserve dotted
schema segments. Full executable metadata family parity is partial in this
release.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Diagnostics are retrieved with `SQLGetDiagRec`. SQLSTATE codes are native
ScratchBird codes (e.g., `40001` for serialization failure, `08001` for
connection failure). `SQLGetDiagField` with `SQL_DIAG_MESSAGE_TEXT` returns the
full engine message including DETAIL and HINT.

Disconnect clears statement handles, the prepared SQL cache, transaction flags,
and bridge session state before any later reconnect.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The driver is `thread_safe`. Connection pooling is managed through the ODBC
Driver Manager's standard connection pooling (`SQL_CP_ONE_PER_DRIVER` or
`SQL_CP_ONE_PER_HENV`). The driver's internal `statement_cache` module
(C++ class `StatementCache`) caches prepared statement handles at the
connection level. Circuit-breaker and keepalive support are present at the
bridge layer.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

Conformance gate: `driver_odbc_gate`. JDBCBL-equivalent groups: CONN, TXN,
EXEC, ERR, TYPE, RES are implemented; META is partial (recursive schema shaping
is present; full catalog family parity is incomplete).

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/go.md -->

<a id="ch-client-driver-guide-drivers-go-md"></a>

# ScratchBird Go Driver — database/sql

The ScratchBird Go driver connects Go applications to a ScratchBird Convergent
Data Engine (CDE) through the standard `database/sql` interface. It speaks the
ScratchBird Native Wire Protocol (SBWP v1.1) directly and registers under the
driver name `"scratchbird"`.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0005-7000-8000-000000000005` |
| `api_surface_set` | `database_sql` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_go_gate` |

---

## Installation

```bash
go get github.com/scratchbird/scratchbird-go
```

Module path: `github.com/scratchbird/scratchbird-go`. Requires Go 1.19 or
later.

---

## Connecting

Import the driver package for its side-effect registration, then use
`database/sql` normally:

```go
import (
    "database/sql"
    _ "github.com/scratchbird/scratchbird-go"
)

db, err := sql.Open("scratchbird", "scratchbird://user:pass@localhost:3092/mydb")
if err != nil {
    log.Fatal(err)
}
defer db.Close()
```

### DSN forms

URI form:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value form:

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

The `ParseConfig(dsn string)` function parses both forms into a `Config`
struct. When the DSN is empty, `defaultConfig()` provides defaults:
`host=localhost`, `port=3092`, `sslmode=require`, `binary_transfer=true`,
`compression=off`.

### Config struct key fields

| Field | Type | Default |
|---|---|---|
| `Host` | `string` | `"localhost"` |
| `Port` | `int` | `3092` |
| `Database` | `string` | |
| `User` | `string` | |
| `Password` | `string` | |
| `SSLMode` | `string` | `"require"` |
| `FrontDoorMode` | `string` | `"direct"` |
| `AuthToken` | `string` | |
| `AuthMethodID` | `string` | |
| `ManagerAuthToken` | `string` | |
| `BinaryTransfer` | `bool` | `true` |
| `Compression` | `string` | `"off"` |

Accepted `sslmode` values: `disable`, `require`, `verify-ca`, `verify-full`.
Accepted `compression` values: `off`, `zstd`.

### Using a Connector directly

```go
connector, err := new(scratchbird.Driver).OpenConnector(dsn)
db := sql.OpenDB(connector)
```

### Auth preflight probe

```go
import sb "github.com/scratchbird/scratchbird-go"

result, err := sb.ProbeAuthSurface(ctx, dsn)
// or on a Connector:
result, err := connector.ProbeAuthSurface(ctx)
// resolved auth after connect:
ctx2, err := conn.GetResolvedAuthContext()
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed (`0A000`).

---

## Executing statements and transactions

### Query and exec

```go
rows, err := db.QueryContext(ctx, "SELECT id, name FROM users WHERE region = $1", "west")
defer rows.Close()
for rows.Next() {
    var id int64
    var name string
    rows.Scan(&id, &name)
}

result, err := db.ExecContext(ctx, "DELETE FROM sessions WHERE expires < $1", cutoff)
affected, _ := result.RowsAffected()
```

Parameter placeholder style is `$N` (positional).

### Multiple result sets

```go
rows, err := db.QueryContext(ctx, "SELECT 1; SELECT 2")
// consume first result set
rows.NextResultSet()
// consume second result set
```

### Transaction control

```go
tx, err := db.BeginTx(ctx, &sql.TxOptions{
    Isolation: sql.LevelReadCommitted,
    ReadOnly:  false,
})
tx.ExecContext(ctx, "UPDATE ...")
tx.Commit()
```

`BeginTx` exposes the standard `database/sql` isolation / read-only subset.
For the extended ScratchBird-specific options use `BeginTxEx`:

```go
// Extended form (driver-owned surface, not database/sql):
conn, _ := db.Conn(ctx)
defer conn.Close()
err = conn.Raw(func(rawConn any) error {
    sbconn := rawConn.(*scratchbird.Conn)
    return sbconn.BeginTxEx(ctx, scratchbird.TxOptions{
        Isolation:       sql.LevelReadCommitted,
        ReadCommittedMode: scratchbird.ReadCommittedModeReadConsistency,
    })
})
```

Isolation alias mapping:

| `sql.IsolationLevel` | Canonical MGA mode |
|---|---|
| `LevelReadUncommitted` | Legacy alias only |
| `LevelReadCommitted` | `READ COMMITTED` |
| `LevelRepeatableRead` | `SNAPSHOT` |
| `LevelSerializable` | `SNAPSHOT TABLE STABILITY` |

ScratchBird sessions are always in a transaction. `Commit` / `Rollback`
reopen the next boundary; the driver drains the reopen `READY` before the
next operation.

### Savepoints

```go
conn.Raw(func(rawConn any) error {
    sbconn := rawConn.(*scratchbird.Conn)
    sbconn.Savepoint(ctx, "sp1")
    sbconn.RollbackToSavepoint(ctx, "sp1")
    sbconn.ReleaseSavepoint(ctx, "sp1")
    return nil
})
```

### Prepared / limbo transaction control

```go
sbconn.PrepareTransaction(ctx, "xid1")
sbconn.CommitPrepared(ctx, "xid1")
sbconn.RollbackPrepared(ctx, "xid1")
```

`SupportsDormantReattach()` returns `false`; `DetachToDormant` and
`ReattachDormant` fail closed with `0A000`.

### Multi-result and batch convenience APIs (driver-owned surface)

```go
summaries, err := sbconn.QueryMultiContext(ctx, "SELECT 1; SELECT 2", nil)
batchSummary, err := sbconn.ExecuteBatchContext(ctx, "INSERT INTO t (x) VALUES ($1)", args)
keys, err := sbconn.ExecuteWithGeneratedKeys(ctx, "INSERT INTO orders (amount) VALUES ($1)", 99.5)
```

### Retry scope

```go
scope := scratchbird.RetryScopeForSQLState("40001") // "statement"
```

`40001`/`40P01` → `"statement"`; `08xxx` → `"reconnect"`; others → `"none"`.

### Pool reset

The `database/sql` pool calls `ResetSession` on checkout, which rolls back
any abandoned explicit transaction state and clears stale plan/SBLR borrow
caches.

---

## Type mapping

The `sbsql_core` profile applies. Go type mappings:

| SBsql canonical type | Go type (Scan target) |
|---|---|
| `BOOLEAN` | `bool` |
| `SMALLINT` (`INT2`) | `int16` |
| `INTEGER` (`INT4`) | `int32` |
| `BIGINT` (`INT8`) | `int64` |
| `REAL` (`FLOAT4`) | `float32` |
| `DOUBLE PRECISION` (`FLOAT8`) | `float64` |
| `NUMERIC` / `DECIMAL` | `string` or `[]byte` |
| `TEXT` / `VARCHAR` / `CHAR` | `string` |
| `BYTEA` | `[]byte` |
| `DATE` | `time.Time` |
| `TIME` | `time.Time` |
| `TIMETZ` | `time.Time` |
| `TIMESTAMP` | `time.Time` (naive) |
| `TIMESTAMPTZ` | `time.Time` (UTC) |
| `UUID` | `[16]byte` or `string` |
| `JSON` / `JSONB` | `[]byte` |
| `POINT` / geometry | `string` (text form) |
| array / range types | `[]byte` (raw wire form) |

OID constants are defined in `types.go` (e.g., `oidBool=16`, `oidTimetz=1266`,
`oidSBVector=16386`).

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Metadata and introspection

```go
conn.Raw(func(rawConn any) error {
    sbconn := rawConn.(*scratchbird.Conn)
    rows, err := sbconn.QueryMetadata(ctx, "tables", nil)
    // or with restrictions:
    rows, err = sbconn.QueryMetadataWithRestrictions(ctx, "columns",
        map[string]string{"TABLE_SCHEMA": "public", "TABLE_NAME": "orders"})
    return err
})
```

`NormalizeMetadataCollectionName` normalizes collection aliases.
`ResolveMetadataCollectionQuery` resolves the backing `sys.information.*`
query. Restriction filtering supports null matching and unknown-key ignore.
The `MetadataExpandSchemaParents` config flag enables dotted parent expansion.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Errors implement `error` and may be cast to `*scratchbird.Error` to access:
- `SQLState` — five-character SQLSTATE string
- `Message` — primary message
- `Detail` / `Hint` — optional detail and hint fields

```go
var sbErr *scratchbird.Error
if errors.As(err, &sbErr) {
    fmt.Println(sbErr.SQLState, sbErr.Message)
}
```

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The driver is `thread_safe`. `database/sql`'s built-in pool (`db.SetMaxOpenConns`,
`db.SetMaxIdleConns`, `db.SetConnMaxLifetime`) is the primary pooling surface.
The driver implements `driver.SessionResetter` so the pool calls `ResetSession`
on each checkout to roll back abandoned transactions and clear borrow caches.
Internal resilience helpers include `CircuitBreaker`, `KeepaliveManager`, and
`LeakDetector`.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

Conformance gate: `driver_go_gate`. All GOBL groups (CONN, TXN, EXEC, META,
TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`. A
manifest-driven conformance harness is in `conformance/harness.go`.

Run conformance tests:

```bash
export SCRATCHBIRD_GO_URL="scratchbird://user:pass@localhost:3092/mydb"
export SCRATCHBIRD_CONFORMANCE_MANIFEST="docs/fixtures/sbwp_conformance_manifest.json"
go test ./...
```

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/dotnet.md -->

<a id="ch-client-driver-guide-drivers-dotnet-md"></a>

# ScratchBird .NET Driver — ADO.NET

The ScratchBird .NET driver connects .NET applications to a ScratchBird
Convergent Data Engine (CDE) through the standard ADO.NET provider interfaces
(`DbConnection`, `DbCommand`, `DbDataReader`, `DbTransaction`). It speaks the
ScratchBird Native Wire Protocol (SBWP v1.1) directly. The NuGet package id is
`ScratchBird.Data`.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0003-7000-8000-000000000003` |
| `api_surface_set` | `ado_net` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_dotnet_gate` |

---

## Installation

The package is distributed as `ScratchBird.Data` (NuGet). Build from source
with the .NET 8 SDK:

```bash
dotnet build src/ScratchBird.Data/ScratchBird.Data.csproj
```

The `.csproj` specifies `TargetFramework=net8.0`, `PackageId=ScratchBird.Data`,
version `0.1.0`, license `Apache-2.0`.

Integration test environment variable: `SCRATCHBIRD_DOTNET_URL`. When unset,
live suites fall back to
`scratchbird://sb_admin:SbAdmin_Compat1!@127.0.0.1:13092/main?sslmode=disable&allow_insecure=true`.

---

## Connecting

### Connection string form

```
Host=db.example.com;Port=3092;Database=prod;Username=alice;Password=secret;SslMode=require
```

Or using the URI form:

```
scratchbird://alice:secret@db.example.com:3092/prod?sslmode=require
```

### Minimal connection example

```csharp
using ScratchBird.Data;

await using var conn = new ScratchBirdConnection(
    "Host=db.example.com;Port=3092;Database=prod;Username=alice;Password=secret;SslMode=require"
);
await conn.OpenAsync();
```

### Connection string builder

```csharp
var builder = new ScratchBirdConnectionStringBuilder
{
    Host = "db.example.com",
    Port = 3092,
    Database = "prod",
    Username = "alice",
    Password = "secret",
    SslMode = "require",
    FrontDoorMode = "direct",      // or "manager_proxy"
    AuthToken = "",                 // bearer token for TOKEN auth
    ManagerAuthToken = "",          // required when FrontDoorMode=manager_proxy
};
string connStr = builder.ToString();
```

### `ScratchBirdConfig` key properties

| Property | Default |
|---|---|
| `Host` | `"localhost"` |
| `Port` | `3092` |
| `Database` | `""` |
| `Username` | `""` |
| `Password` | `""` |
| `SslMode` | `"require"` |
| `FrontDoorMode` | `"direct"` |
| `AuthToken` | `""` |
| `AuthMethodId` | `""` |
| `ManagerAuthToken` | `""` |
| `BinaryTransfer` | `true` |
| `Compression` | `"off"` |

### Auth preflight probe

```csharp
var probe = await ScratchBirdConnection.ProbeAuthSurface(connectionString);
// resolved auth after connect:
var ctx = conn.GetResolvedAuthContext();
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed (`0A000`).

Dormant reattach is supported on this driver: `SupportsDormantReattach()`
returns `true`. `DetachToDormant()` returns the engine-issued `dormant_id` and
`dormant_reattach_token`; `ReattachDormant(dormantId, token)` uses them at
reconnect.

---

## Executing statements and transactions

### Command execution

```csharp
await using var cmd = conn.CreateCommand();
cmd.CommandText = "SELECT id, name FROM users WHERE region = $1";
cmd.Parameters.Add(new ScratchBirdParameter { Value = "west" });

await using var reader = await cmd.ExecuteReaderAsync();
while (await reader.ReadAsync())
{
    long id = reader.GetInt64(0);
    string name = reader.GetString(1);
}
```

### Prepared commands

```csharp
await cmd.PrepareAsync();     // sends PARSE to the server
await cmd.ExecuteReaderAsync();
```

### Transaction control

```csharp
await using var tx = await conn.BeginTransactionAsync(
    System.Data.IsolationLevel.ReadCommitted
);
// ...
await tx.CommitAsync();
await tx.RollbackAsync();
```

Extended options via `ScratchBirdTransactionOptions`:

```csharp
await using var tx = await conn.BeginTransaction(
    new ScratchBirdTransactionOptions
    {
        IsolationLevel = System.Data.IsolationLevel.ReadCommitted,
        ReadCommittedMode = ScratchBirdReadCommittedMode.ReadConsistency,
        AccessMode = "read write",
        Deferrable = false,
        Wait = true,
        TimeoutMs = 5000,
        AutoCommit = false,
    }
);
```

### Isolation level mapping

| `System.Data.IsolationLevel` | Canonical MGA mode |
|---|---|
| `ReadUncommitted` | Legacy alias only |
| `ReadCommitted` | `READ COMMITTED` |
| `RepeatableRead` | `SNAPSHOT` |
| `Serializable` / `Snapshot` / `Chaos` | `SNAPSHOT TABLE STABILITY` |

`ScratchBirdReadCommittedMode.ReadConsistency` selects canonical
`READ COMMITTED READ CONSISTENCY`.

ScratchBird sessions are always in a transaction. `CommitAsync` /
`RollbackAsync` reopen the next boundary.

### Savepoints

```csharp
await conn.SaveAsync("sp1");
await conn.RollbackAsync("sp1");
await conn.ReleaseAsync("sp1");
```

### Prepared / limbo transaction control

```csharp
await conn.PrepareTransaction("xid1");
await conn.CommitPrepared("xid1");
await conn.RollbackPrepared("xid1");
```

### Multi-result and batch convenience APIs

```csharp
// Multi-result traversal:
var results = await conn.QueryMulti("SELECT 1; SELECT 2", null);

// Batch execution with summaries:
var summary = await conn.ExecuteBatch(
    "INSERT INTO t (x) VALUES ($1)",
    new[] { new[] { (object)1 }, new[] { (object)2 } }
);

// Generated keys:
var keys = await conn.ExecuteWithGeneratedKeys(
    "INSERT INTO orders (amount) VALUES ($1)", 99.5m
);
```

### Retry scope

```csharp
var scope = ScratchBirdSqlStateMapper.RetryScopeForSqlState("40001");
// "statement"
bool retryable = ScratchBirdSqlStateMapper.IsRetryableSqlState("40001");
```

---

## Type mapping

The `sbsql_core` profile applies. Key ADO.NET type mappings:

| SBsql canonical type | .NET type | `ScratchBirdDataReader.Get*` method |
|---|---|---|
| `BOOLEAN` | `bool` | `GetBoolean` |
| `SMALLINT` | `short` | `GetInt16` |
| `INTEGER` | `int` | `GetInt32` |
| `BIGINT` | `long` | `GetInt64` |
| `REAL` | `float` | `GetFloat` |
| `DOUBLE PRECISION` | `double` | `GetDouble` |
| `NUMERIC` / `DECIMAL` | `decimal` | `GetDecimal` |
| `TEXT` / `VARCHAR` | `string` | `GetString` |
| `BYTEA` | `byte[]` | `GetValue` |
| `DATE` | `DateOnly` | `GetDateOnly` |
| `TIME` | `TimeOnly` | `GetTimeOnly` |
| `TIMETZ` | `DateTimeOffset` | `GetDateTimeOffset` |
| `TIMESTAMP` | `DateTime` (unspecified kind) | `GetDateTime` |
| `TIMESTAMPTZ` | `DateTime` (UTC) | `GetDateTime` |
| `UUID` | `Guid` | `GetGuid` |
| `JSON` / `JSONB` | `string` | `GetString` |
| `INET` / `CIDR` | `string` | `GetString` |
| `MACADDR` / `MACADDR8` | `string` | `GetString` |
| array / range types | `object` | `GetValue` |

Decoded by `TypeDecoder`. Parameter binding uses `ScratchBirdParameter` with
`Value` set to the appropriate .NET type.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Metadata and introspection

```csharp
DataTable tables = conn.GetSchema("Tables",
    new[] { null, "public", null, null });
DataTable cols = conn.GetSchema("Columns",
    new[] { null, "public", "orders", null });
DataTable pks = conn.GetSchema("PrimaryKeys",
    new[] { null, "public", "orders" });
```

Supported collections: `Tables`, `Columns`, `Schemas`, `Catalogs`, `Indexes`,
`IndexColumns`, `Constraints`, `PrimaryKeys`, `ForeignKeys`, `TablePrivileges`,
`ColumnPrivileges`, `Procedures`, `Functions`, `Routines`, `TypeInfo`.
Restrictions support `"null"` literal matching and `%`/`_` wildcard patterns.

Schema-parent expansion:

```
Host=...;MetadataExpandSchemaParents=true
```

(aliases: `metadataExpandSchemaParents`, `metadata_expand_schema_parents`,
`expandSchemaParents`, `expand_schema_parents`, `dbeaverExpandSchemaParents`).

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Errors are raised as `ScratchBirdException` (subclass of `DbException`):

```csharp
try
{
    await cmd.ExecuteNonQueryAsync();
}
catch (ScratchBirdException ex)
{
    Console.WriteLine(ex.SqlState);   // e.g., "23505"
    Console.WriteLine(ex.Message);
}
```

SQLSTATE codes are in `ex.SqlState`. Retry-boundary helpers:
`ScratchBirdSqlStateMapper.RetryScopeForSqlState(sqlstate)` and
`IsRetryableSqlState(sqlstate)`.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The driver is `thread_safe`. `ProtocolClientPool` provides internal connection
pooling with lease/acquire timeout, lifetime eviction, saturation fallback, and
cancellation cleanup. Set `Pooling=true` in the connection string and tune with
`MinPoolSize`, `MaxPoolSize`, `ConnectionLifetime`, and `PoolAcquireTimeoutMs`.

```
Host=...;Pooling=true;MaxPoolSize=20;ConnectionLifetime=300
```

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

Conformance gate: `driver_dotnet_gate`. All DOTNETBL groups (CONN, TXN, EXEC,
META, TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`.
TXN full live isolation-matrix validation and output-parameter semantics remain
pending.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/r2dbc.md -->

<a id="ch-client-driver-guide-drivers-r2dbc-md"></a>

# ScratchBird R2DBC Driver — R2DBC SPI

The ScratchBird R2DBC driver connects reactive Java applications to a
ScratchBird Convergent Data Engine (CDE) through the R2DBC SPI
(Service Provider Interface). It speaks the ScratchBird Native Wire Protocol
(SBWP v1.1) and is designed for non-blocking, back-pressure-capable access
patterns.

**Release status: beta_2 (release_candidate gate)**

> **Draft note:** This driver's source tree contains only a
> `package_contract.json` at this time. No README, build file, or
> implementation source files were present in
> `project/drivers/driver/r2dbc/`. All claims below are sourced from the
> `DriverPackageManifest.csv` row and the `package_contract.json` contract
> file. Implementation-level detail (artifact coordinates, connection URL
> form, code examples) will be added when source is available.

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0029-7000-8000-000000000029` |
| `api_surface_set` | `r2dbc_spi` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `reactive_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_r2dbc_gate` |

---

## R2DBC SPI surface (from package_contract.json)

The driver implements the following R2DBC SPI types:

| R2DBC type | Role |
|---|---|
| `ConnectionFactoryProvider` | Driver registration; matched by `r2dbc:scratchbird:` URL scheme |
| `ConnectionFactory` | Factory for reactive `Connection` instances |
| `Connection` | Single database connection with reactive lifecycle |
| `Statement` | Prepared or parameterized statement |
| `Result` | Reactive result stream (rows + row metadata) |
| `Row` | Single data row |
| `Batch` | Batch of statements executed as a unit |
| `TransactionDefinition` | Transaction option payload (isolation, read-only, etc.) |
| `ValidationDepth` | Connection validation depth for pool health checks |

---

## Route requirements

The driver enforces the following route requirements:

| Requirement | Description |
|---|---|
| `sbwp_v1_1` | ScratchBird Native Wire Protocol v1.1 |
| `scratchbird_tls_1_3_floor` | TLS 1.3 minimum |
| `engine_authentication_authority` | Auth resolved by the engine |
| `engine_authorization_authority` | Authorization resolved by the engine |
| `mga_transaction_finality` | Transaction finality owned by the MGA transaction inventory |
| `sys_information_metadata` | Metadata served through `sys.information.*` |
| `uuid_identity` | UUID-based catalog identity |
| `no_hidden_replay` | No implicit statement replay |

---

## Conformance profile

The declared conformance areas:

| Area | Included |
|---|---|
| `connect_auth` | Yes |
| `prepare_execute_fetch` | Yes |
| `transactions` | Yes |
| `metadata` | Yes |
| `type_mapping` | Yes |
| `error_mapping` | Yes |
| `reconnect` | Yes |
| `protocol_negotiation` | Yes |
| `cancellation` | Yes |

---

## Connecting (expected URL form)

Based on the manifest `dsn_key_set` (`database`, `host`, `port`, `user`,
`auth_method`), the expected R2DBC URL form is:

```
r2dbc:scratchbird://user:password@host:3092/database
```

Ingress modes `direct_listener` and `manager_proxy` are supported.
TLS floor is `scratchbird_tls_1_3_floor`. Auth methods: `engine_local_password`
and `scram_ready` (covering `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`).

> Confirmed connection API class names and URL parsing behavior are not
> verifiable from the current source tree. The URL form above is inferred
> from the manifest and R2DBC SPI conventions — confirm against driver source
> when available.

---

## Type mapping

The `sbsql_core` type-mapping profile applies (same as Python, JDBC, Go, and
.NET drivers). See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the canonical
SBsql-to-language-type mapping table.

---

## Metadata and introspection

The metadata profile is `sys_information_recursive`. The driver is expected
to expose `sys.information.*` views through the R2DBC `ConnectionMetadata`
and row-metadata surfaces.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

The diagnostic mapping profile is `native_sqlstate`. R2DBC exceptions carry
SQLSTATE codes through `R2dbcException.getSqlState()`. The retry-boundary
contract (verified for all other ScratchBird drivers) classifies `40001`/`40P01`
as statement-boundary retries and `08xxx` as reconnect-boundary retries.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The pooling capability is `reactive_pool`. The driver is `thread_safe`.
Connection pooling is handled reactively, compatible with R2DBC pool libraries
(`r2dbc-pool`). `ValidationDepth` is supported for pool health probes.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

Conformance gate: `driver_r2dbc_gate`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/cpp.md -->

<a id="ch-client-driver-guide-drivers-cpp-md"></a>

# C/C++ Driver — Native CLI C API (`libscratchbird_client`)

The C/C++ driver provides a dual-surface native client library for ScratchBird. The primary
surface is a stable C ABI (`scratchbird_client.h`) usable from any language with C FFI support.
A higher-level C++ surface is layered on top and exposes RAII wrappers, prepared statements,
connection pooling, and typed parameter binding. Both surfaces speak SBWP v1.1 (ScratchBird
Binary Wire Protocol) over TCP/TLS.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0001-7000-8000-000000000001` |
| API surface | `native_cli_c_api` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `session_pool`, `statement_cache` |
| Conformance profile ref | `driver_cpp_gate` |

---

## Installation

The library is built with CMake. The project name and output library are `scratchbird_client`
(version 0.1.0, C++17, license MPL-2.0). OpenSSL is a required dependency.

```bash
cmake -S . -B build
cmake --build build --config Release
```

The public header to include is `<scratchbird/client/scratchbird_client.h>` (C API) or the
C++ headers under `include/scratchbird/client/` (`connection.h`, `network_client.h`,
`pool.h`, `metadata.h`).

---

## Connecting

### C API — DSN string

```c
#include <scratchbird/client/scratchbird_client.h>

sb_error err = {0};
sb_connection* conn =
    sb_connect("scratchbird://user:pass@127.0.0.1:3092/mydb", &err);
if (!conn) {
    /* err.code and err.message describe the failure */
    return 1;
}
/* use conn ... */
sb_disconnect(conn);
```

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value:
```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

### Key DSN parameters

| Key | Description |
|---|---|
| `host` | Server hostname or IP |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password (PASSWORD or SCRAM auth) |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct_listener` (default) or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |
| `compression` | `off` (default) or `zstd` |

### Auth probe (staged bootstrap)

Before a full connect, the driver can probe which auth methods the server advertises:

```c
char* json = sb_probe_auth_surface_json(
    "scratchbird://user@host:3092/mydb", &err);
/* json is driver-owned; release with sb_memory_release(json) */
```

The C++ equivalent is `scratchbird::client::probeAuthSurface(...)` or
`NetworkClient::probeAuthSurface(...)`.

After a successful connection, `sb_get_resolved_auth_context_json(conn, &err)` returns
the negotiated auth context.

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### C API: query and execute

```c
/* Simple query — returns result handle */
sb_result* result = sb_query(conn, "SELECT id, name FROM items", &err);
if (!result) { /* check err */ }

size_t nrows = sb_result_row_count(result);
size_t ncols = sb_result_col_count(result);
for (size_t r = 0; r < nrows; r++) {
    sb_row row = sb_result_get_row(result, r);
    sb_value val = sb_row_get_value_by_index(&row, 0);
    /* use val */
}
sb_result_free(result);

/* Execute (no rows returned) */
sb_execute(conn, "INSERT INTO items VALUES ($1)", &err);
```

### Prepared statements (C API)

```c
sb_prepared* stmt = sb_prepare(conn, "SELECT id FROM items WHERE id=$1", &err);
sb_bind_index(stmt, 0, SB_TYPE_INT4, &my_int, sizeof(my_int));
sb_result* res = sb_execute_prepared(stmt, &err);
sb_result_free(res);
sb_prepared_free(stmt);
```

The C++ surface exposes `PreparedStatement` with typed parameter setters:

```cpp
#include <scratchbird/client/connection.h>

scratchbird::client::Connection conn(...);
conn.open();
auto stmt = conn.prepare("SELECT id FROM items WHERE id = $1");
stmt.setInt(0, 42);
auto result = stmt.execute();
```

### Transaction control

```c
/* Begin */
sb_txn_options opts = {0};
opts.isolation = SB_TXN_READ_COMMITTED;      /* byte 1 */
/* opts.isolation = SB_TXN_SNAPSHOT;         byte 2 */
/* opts.isolation = SB_TXN_SNAPSHOT_TABLE_STABILITY; byte 3 */
sb_tx_begin(conn, &opts, &err);

/* Savepoint */
sb_tx_savepoint(conn, "sp1", &err);
sb_tx_rollback_to(conn, "sp1", &err);
sb_tx_release_savepoint(conn, "sp1", &err);

/* Commit / rollback */
sb_tx_commit(conn, &err);
sb_tx_rollback(conn, &err);
```

ScratchBird sessions are always in a transaction. `COMMIT` and `ROLLBACK` immediately
reopen the next transaction boundary. `sb_canonical_isolation_name(byte)` returns the
canonical isolation name for auditing.

Retry scope helper: `sb_retry_scope_for_sqlstate(sqlstate)` — returns
`40001`/`40P01` => fresh statement only; `08xxx` => reconnect only; everything else =>
no automatic replay.

The C++ `Connection` surface provides `beginTransaction(...)`, `commit()`, `rollback()`,
`savepoint()`, `releaseSavepoint()`, `rollbackToSavepoint()`, `prepareTransaction()`,
`commitPrepared()`, and `rollbackPrepared()`.

---

## Type mapping

The cpp driver maps to the `sbsql_core` type profile. The C API represents values via
`sb_value` / `sb_type` (an `sb_type_code` enum). The C++ surface uses `sb_value`
accessed through typed getters.

| SBSQL canonical type | C API sb_type_code | Notes |
|---|---|---|
| BOOLEAN | `SB_TYPE_BOOL` | |
| SMALLINT | `SB_TYPE_INT2` | |
| INTEGER | `SB_TYPE_INT4` | |
| BIGINT | `SB_TYPE_INT8` | |
| REAL | `SB_TYPE_FLOAT4` | |
| DOUBLE PRECISION | `SB_TYPE_FLOAT8` | |
| NUMERIC / DECIMAL | `SB_TYPE_NUMERIC` | |
| TEXT / VARCHAR / BPCHAR | `SB_TYPE_TEXT` / `SB_TYPE_VARCHAR` / `SB_TYPE_BPCHAR` | |
| BYTEA | `SB_TYPE_BYTEA` | |
| DATE | `SB_TYPE_DATE` | |
| TIME | `SB_TYPE_TIME` | |
| TIMESTAMP | `SB_TYPE_TIMESTAMP` | |
| TIMESTAMPTZ | `SB_TYPE_TIMESTAMPTZ` | |
| INTERVAL | `SB_TYPE_INTERVAL` | |
| UUID | `SB_TYPE_UUID` | |
| JSON | `SB_TYPE_JSON` | |
| JSONB | `SB_TYPE_JSONB` | |
| ARRAY | `SB_TYPE_ARRAY` | |
| RANGE | `SB_TYPE_RANGE` | |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full SBSQL canonical type set.

---

## Metadata and introspection

Metadata collections are accessed via `sb_metadata_query(conn, collection_name, &err)`,
which resolves the collection name and executes the appropriate `sys.*` query. Supported
collections include `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`, and `routines`.

Column metadata for a result is available via `sb_column_count(result)` and
`sb_get_column_meta(result, col_index, &meta_out)`.

The C++ surface provides `metadataSchemaPathsForNavigation(...)`,
`buildMetadataSchemaTree(...)`, `buildMetadataSchemaTreeRows(...)`, and
`buildMetadataDdlEditorSchemaPayloadJson(...)` in `metadata.h`.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) for the
`sys.information.*` catalog family reference.

---

## Errors and diagnostics

The C API communicates errors via the caller-owned `sb_error` struct (an `sb_error_code`
plus a 256-byte message string). The driver never allocates `sb_error`.

```c
sb_error err = {0};
sb_result* r = sb_query(conn, "bad sql", &err);
if (!r) {
    printf("error %d: %s\n", err.code, err.message);
}
```

Key `sb_error_code` values: `SB_ERR_CONNECTION_FAILED`, `SB_ERR_AUTH_FAILED`,
`SB_ERR_TXN_CONFLICT`, `SB_ERR_DEADLOCK`, `SB_ERR_SYNTAX`, `SB_ERR_CONSTRAINT`,
`SB_ERR_TXN_ABORTED`, `SB_ERR_NOT_IMPLEMENTED`.

The SQLSTATE string is propagated through `ErrorContext` in the C++ layer. Use
`sb_retry_scope_for_sqlstate(sqlstate)` to classify whether a statement or a full
reconnect is warranted.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) for SQLSTATE
classes and retry semantics.

---

## Memory ownership

Driver-allocated handles (`sb_connection`, `sb_result`, `sb_prepared`) must be released
with their matching `sb_*_free` function. `char*` values returned by the API are
driver-owned and must be released with `sb_memory_release(ptr)` or `sb_memory_free(ptr)`.
`const char*` and `sb_value` pointers borrowed from row/metadata callbacks are valid only
for the documented parent handle lifetime and must not be freed.

---

## Pooling and concurrency

The C++ surface provides `ConnectionPool` and `ConnectionLease` as RAII wrappers over the
pooled C API. The driver is rated `thread_safe`. A checked-out lease is exclusive to its
thread while held. A `LeakDetector` is built in for diagnosing unreturned leases.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

This driver targets conformance profile `driver_cpp_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented` as of the current
S0 baseline. Remaining gaps are live integration depth, not missing API surfaces.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/rust.md -->

<a id="ch-client-driver-guide-drivers-rust-md"></a>

# Rust Driver — Async language binding for ScratchBird

The Rust driver (`scratchbird` crate) is an async-native client for ScratchBird built on
Tokio. It speaks SBWP v1.1 over TCP/TLS using `rustls`, exposes a `Client` type with
`async/await` methods, and provides connection pooling, circuit breaking, keepalive, and
leak detection. It is the idiomatic choice for Rust applications connecting directly.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0015-7000-8000-000000000015` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `connection_pool` |
| Conformance profile ref | `driver_rust_gate` |

---

## Installation

Add to `Cargo.toml` (crate name: `scratchbird`, version 0.1.0, license MPL-2.0):

```toml
[dependencies]
scratchbird = "0.1"
tokio = { version = "1", features = ["rt-multi-thread", "macros"] }
```

The crate requires `tokio` as the async runtime. `rustls` is used for TLS; no native
OpenSSL dependency.

---

## Connecting

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value:
```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

### Minimal connection example

```rust
use scratchbird::{Client, Config};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut client = Client::new(Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb"
    )?);
    client.connect().await?;
    let result = client.query("SELECT 1").await?;
    println!("{:?}", result.rows[0][0]);
    client.close().await;
    Ok(())
}
```

### Key DSN / Config fields

| Key | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |
| `auth_payload_json` / `auth_payload_b64` | Structured auth payloads |
| `workload_identity_token` | Workload identity token |
| `proxy_principal_assertion` | Proxy principal assertion |
| `compression` | `off` (default) or `zstd` |
| `binary_transfer` | `true` (default) or `false` |
| `metadata_expand_schema_parents` | Enable recursive parent expansion |

### Auth probe (staged bootstrap)

```rust
use scratchbird::probe_auth_surface;

let probe = probe_auth_surface("scratchbird://user@host:3092/mydb").await?;
// or on an existing client: client.probe_auth_surface().await?
let ctx = client.get_resolved_auth_context();
```

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### Query and execute

```rust
// Simple query
let result: QueryResult = client.query("SELECT id, name FROM items").await?;

// Parameterized query
use scratchbird::Params;
let result = client.query_params(
    "SELECT id FROM items WHERE id = $1",
    Params::positional(vec![42.into()])
).await?;

// Execute (no rows)
client.execute("DELETE FROM items WHERE id = $1",
    Params::positional(vec![99.into()])
).await?;
```

### Multi-result, batch, and callable

```rust
// Multi-result
let results = client.query_multi("SELECT 1; SELECT 2").await?;

// Batch execution
let summaries: BatchSummary = client.execute_batch(
    "INSERT INTO t VALUES ($1)", batch_params
).await?;

// Generated keys
let keys = client.execute_with_generated_keys(
    "INSERT INTO t(name) VALUES ($1) RETURNING id", params
).await?;

// Callable normalization
let sql = scratchbird::normalize_callable("{ call my_proc(?) }")?;
let result = client.call(&sql, params).await?;
```

### Transaction control

```rust
use scratchbird::protocol::{
    READ_COMMITTED_MODE_READ_CONSISTENCY,
    canonical_read_committed_mode_label,
};

// Begin with explicit options
client.begin(scratchbird::TxnBeginOptions {
    isolation_level: Some("READ COMMITTED".to_string()),
    read_committed_mode: Some(READ_COMMITTED_MODE_READ_CONSISTENCY),
    access_mode: Some("READ WRITE".to_string()),
    ..Default::default()
}).await?;

client.savepoint("sp1").await?;
client.rollback_to_savepoint("sp1").await?;
client.release_savepoint("sp1").await?;
client.commit().await?;
// or:
client.rollback().await?;
```

ScratchBird sessions are always in a transaction. `commit()` / `rollback()` immediately
reopen the next boundary. `begin()` restarts the current boundary with the requested
options rather than assuming idle-session semantics.

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

Autocommit:
```rust
client.set_autocommit(true).await?;   // commits active txn, then switches mode
client.set_autocommit(false).await?;  // eagerly begins new txn if none active
```

Retry scope:
```rust
use scratchbird::retry_scope_for_sqlstate;
let scope = retry_scope_for_sqlstate("40001"); // RetryScope::Statement
```

---

## Type mapping

The crate maps to the `sbsql_core` profile. Types are exchanged via the `Value` and
`Param` enums, with concrete structs for complex types.

| SBSQL canonical type | Rust type |
|---|---|
| BOOLEAN | `bool` |
| SMALLINT | `i16` |
| INTEGER | `i32` |
| BIGINT | `i64` |
| REAL | `f32` |
| DOUBLE PRECISION | `f64` |
| NUMERIC / DECIMAL | `Decimal` (struct) |
| TEXT / VARCHAR | `String` |
| BYTEA | `Vec<u8>` |
| DATE | `Date` (struct) |
| TIME | `Time` (struct) |
| TIMESTAMP | `Timestamp` (struct) |
| TIMESTAMPTZ | `TimestampTz` (struct) |
| INTERVAL | `Interval` (struct) |
| UUID | `String` |
| JSON | `Json` (struct) |
| JSONB | `Jsonb` (struct) |
| MONEY | `Money` (struct) |
| ARRAY | `Value::Array` |
| RANGE | `Range<T>` (struct) |
| Geometry | `Geometry` (struct) |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full type reference.

---

## Metadata and introspection

```rust
// Named collection
let schemas = client.get_schema("schemas", None).await?;

// With restrictions
let tables = client.get_schema(
    "tables",
    Some(vec![("schema", "public")].into_iter().collect())
).await?;

// Recursive schema tree
let tree = client.get_schema_tree("my_schema", true).await?;

// DDL editor payload
let payload = client.ddl_editor_schema_payload("my_schema", true).await?;
```

Supported collections include `schemas`, `tables`, `columns`, `indexes`,
`index_columns`, `constraints`, `primary_keys`, `foreign_keys`, `table_privileges`,
`column_privileges`, `procedures`, `functions`, `routines`, `type_info`, `catalogs`.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Errors are returned as `scratchbird::Error` with a `kind: ErrorKind` discriminant:

```rust
use scratchbird::{Error, ErrorKind};

match result {
    Err(e) if e.kind() == ErrorKind::TxnConflict => { /* retry */ }
    Err(e) => eprintln!("SQLSTATE={:?}: {}", e.sqlstate(), e),
    Ok(r) => { /* use r */ }
}
```

`scratchbird::is_retryable_sqlstate(s)` and `retry_scope_for_sqlstate(s)` classify
SQLSTATEs for retry logic without hard-coding error codes.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

`ConnectionPool` / `PooledConnection` wrap `Client` for bounded reuse:

```rust
use scratchbird::pool::{ConnectionPool, PoolConfig};

let pool = ConnectionPool::new(config, PoolConfig { max_size: 10, ..Default::default() });
let conn = pool.get().await?;
conn.query("SELECT 1").await?;
// conn returns to pool on drop
```

`with_retry(pool, config, op)` is a convenience wrapper for retry-on-conflict.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

This driver targets conformance profile `driver_rust_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented` as of the current
baseline.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/node.md -->

<a id="ch-client-driver-guide-drivers-node-md"></a>

# Node.js / TypeScript Driver — Async language binding for ScratchBird

The Node.js driver (`scratchbird` npm package) is a native TypeScript client for
ScratchBird. It speaks SBWP v1.1 over TCP/TLS, exports a `Client` class with
`async/await` methods and full TypeScript type declarations, and includes a `Pool` class
for connection pooling. It is the idiomatic choice for Node.js / TypeScript applications.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0008-7000-8000-000000000008` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `connection_pool` |
| Conformance profile ref | `driver_node_gate` |

---

## Installation

```bash
npm install scratchbird
```

Package name: `scratchbird`, version 0.1.0, license MPL-2.0. Main entry point:
`dist/index.js`. TypeScript declarations: `dist/index.d.ts`.

Build from source:
```bash
npm install
npm run build   # tsc -p tsconfig.json
```

---

## Connecting

### Object config (recommended)

```typescript
import { Client } from "scratchbird";

const client = new Client({
  host: "localhost",
  port: 3092,
  user: "user",
  password: "pass",
  database: "db",
});

await client.connect();
const res = await client.query("SELECT 1 AS one");
console.log(res.rows);
await client.end();
```

### DSN string

```typescript
const client = new Client({
  connectionString: "scratchbird://user:pass@localhost:3092/mydb",
});
```

### TLS

```typescript
const client = new Client({
  host: "localhost",
  user: "user",
  password: "pass",
  database: "db",
  sslmode: "verify-full",
  sslrootcert: "/etc/ssl/certs/ca.pem",
});
```

### Key `ClientConfig` fields

| Field | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `authMethod` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `frontDoorMode` | `direct_listener` or `manager_proxy` |
| `managerAuthToken` | Required when `frontDoorMode=manager_proxy` |
| `authToken` | Bearer token for TOKEN auth |
| `metadataExpandSchemaParents` | Enable recursive parent expansion (also: `metadata_expand_schema_parents`, `expandSchemaParents`) |

### Auth probe (staged bootstrap)

```typescript
import { Client } from "scratchbird";

const probe = await Client.probeAuthSurface({ host: "localhost", port: 3092 });
// or on a connected client: client.probeAuthSurface()
const ctx = client.getResolvedAuthContext();
```

Admitted-but-unsupported methods (`MD5`, `PEER`) fail closed. Integration test DSN:
set `SCRATCHBIRD_NODE_URL` or `SCRATCHBIRD_TEST_DSN`.

---

## Executing statements and transactions

### Query and execute

```typescript
// Simple query
const res = await client.query("SELECT id, name FROM items");
console.log(res.rows); // Array<Record<string, any>>

// Parameterized (positional)
const res2 = await client.query(
  "SELECT id FROM items WHERE id = $1", [42]
);

// Execute (no rows)
await client.query("DELETE FROM items WHERE id = $1", [99]);
```

### Multi-result, batch, callable, streaming

```typescript
// Multi-result
const results = await client.queryMulti("SELECT 1; SELECT 2");

// Batch
const summary = await client.executeBatch(
  "INSERT INTO t VALUES ($1)", [[1], [2], [3]]
);

// Generated keys
const keys = await client.executeWithGeneratedKeys(
  "INSERT INTO t(name) VALUES ($1) RETURNING id", ["Alice"]
);

// Callable (JDBC escape syntax)
const callResult = await client.call("{ call my_proc(?) }", [arg]);

// Streaming with pagination
const stream = await client.queryStream("SELECT * FROM big_table", [], { maxRows: 100 });
for await (const batch of stream) { /* process batch */ }
```

### Native SQL normalization

```typescript
const normalized = client.nativeSQL("SELECT * FROM t WHERE id = ?");
const callable = client.nativeCallableSQL("{ call proc(?) }");
```

### Session schema

```typescript
await client.setSessionSchema("my_schema");
const current = client.getSessionSchema();
```

### Transaction control

```typescript
// Begin with options
await client.beginTransaction({
  isolationLevel: "READ COMMITTED",
  readCommittedMode: "READ COMMITTED READ CONSISTENCY",
  accessMode: "READ WRITE",
});

await client.savepoint("sp1");
await client.rollbackToSavepoint("sp1");
await client.releaseSavepoint("sp1");
await client.commitTransaction();
// or:
await client.rollbackTransaction();
```

ScratchBird sessions are always in a transaction. `commitTransaction()` /
`rollbackTransaction()` immediately reopen the next boundary.

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

Autocommit:
```typescript
await client.setAutoCommit(true);
await client.setAutoCommit(false); // eager begin
const ac = client.getAutoCommit();
```

Retry scope:
```typescript
import { retryScopeForSqlState } from "scratchbird";
const scope = retryScopeForSqlState("40001"); // "statement"
```

---

## Type mapping

Typed value classes are exported from the package for encoding complex types.

| SBSQL canonical type | TypeScript / JavaScript type |
|---|---|
| BOOLEAN | `boolean` |
| SMALLINT / INTEGER / BIGINT | `number` / `bigint` |
| REAL / DOUBLE PRECISION | `number` |
| NUMERIC / DECIMAL | `ScratchbirdDecimal` |
| TEXT / VARCHAR | `string` |
| BYTEA | `Buffer` |
| DATE | `ScratchbirdDate` |
| TIME | `ScratchbirdTime` |
| TIMESTAMP | `ScratchbirdTimestamp` |
| TIMESTAMPTZ | `ScratchbirdTimestampTZ` |
| INTERVAL | `ScratchbirdInterval` |
| UUID | `string` |
| JSON | `ScratchbirdJson` |
| JSONB | `ScratchbirdJsonb` |
| MONEY | `ScratchbirdMoney` |
| ARRAY | `any[]` |
| RANGE | `ScratchbirdRange<T>` |
| Geometry | `ScratchbirdGeometry` |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full type reference.

---

## Metadata and introspection

```typescript
// Named collection
const schemas = await client.queryMetadata("schemas");
const tables  = await client.getSchema("tables", { schema: "public" });

// Recursive schema tree
const tree = await client.getSchema("schemas"); // with metadataExpandSchemaParents config
```

Supported collections: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Errors are thrown as typed classes:

```typescript
import { ScratchbirdConnectionError, ScratchbirdTransactionError } from "scratchbird";

try {
  await client.query("bad sql");
} catch (e) {
  if (e instanceof ScratchbirdTransactionError) {
    const scope = retryScopeForSqlState(e.sqlState);
    // scope: "statement" | "reconnect" | "none"
  }
}
```

Normalization failures map to `ScratchbirdSyntaxError` (SQLSTATE `07001`).

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

```typescript
import { Pool } from "scratchbird";

const pool = new Pool({
  host: "localhost", port: 3092, user: "user", password: "pass", database: "db",
  max: 10,
});
const client = await pool.connect();
await client.query("SELECT 1");
client.release();
await pool.end();
```

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

This driver targets conformance profile `driver_node_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/ruby.md -->

<a id="ch-client-driver-guide-drivers-ruby-md"></a>

# Ruby Driver — Native language binding for ScratchBird

The Ruby driver (`scratchbird` gem) is a native client for ScratchBird using the
ScratchBird wire protocol (SBWP v1.1). It exposes a `Scratchbird::Connection` class as
the primary application API and a lower-level `Scratchbird::Client` for protocol
interaction. The gem is pure Ruby; no native extensions are required.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0014-7000-8000-000000000014` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `connection_pool` |
| Conformance profile ref | `driver_ruby_gate` |

---

## Installation

Build and install from the gem specification (`scratchbird.gemspec`, version 0.1.0,
license MIT, requires Ruby >= 2.7):

```bash
gem build scratchbird.gemspec
gem install scratchbird-0.1.0.gem
```

Require in code:

```ruby
require "scratchbird"
```

---

## Connecting

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value hash:
```ruby
{ host: "localhost", port: 3092, dbname: "mydb", user: "myuser", password: "mypass" }
```

### Minimal connection example

```ruby
require "scratchbird"

conn = Scratchbird.connect("scratchbird://user:pass@localhost:3092/mydb")
result = conn.query("SELECT 1 AS one")
puts result.first[0]
conn.close
```

`Scratchbird.connect` returns a `Scratchbird::Connection`. The lower-level
`Scratchbird::Client` is used internally and exposed for advanced use.

### Key DSN / Config parameters

| Key | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `require`, `verify-ca`, `verify-full` (disable is rejected) |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |

### Auth probe (staged bootstrap)

```ruby
# Module-level probe
surface = Scratchbird.probe_auth_surface("scratchbird://user@host:3092/mydb")

# Client-level probe
client = Scratchbird::Client.new(config)
surface = client.probe_auth_surface

# After connection
ctx = conn.resolved_auth_context
# or on a Client instance: client.get_resolved_auth_context
```

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### Query and execute

```ruby
# Simple query — returns Result object
result = conn.query("SELECT id, name FROM items")
result.each { |row| puts row.inspect }

# Parameterized (positional)
result = conn.query("SELECT id FROM items WHERE id = $1", [42])

# Execute (no rows returned)
conn.execute("DELETE FROM items WHERE id = $1", [99])
```

### Multi-result, batch, callable, generated keys

```ruby
# Multi-result
results = conn.query_multi("SELECT 1; SELECT 2")

# Batch execution
summary = conn.execute_batch("INSERT INTO t VALUES ($1)", [[1], [2], [3]])
# summary.items => Array<BatchItemSummary> (:index, :row_count, :command, :last_id)

# Generated keys
result = conn.execute_with_generated_keys(
  "INSERT INTO t(name) VALUES ($1) RETURNING id", ["Alice"]
)

# Callable (JDBC escape syntax)
conn.call("{ call my_proc(?) }", [arg])

# Callable SQL normalization
normalized = Scratchbird::Sql.normalize_callable("{ call routine(?) }")
```

### Prepared statements

```ruby
stmt = conn.prepare("SELECT id FROM items WHERE id = $1")
result = conn.execute_prepared(stmt, [42])
stmt.close
```

### Transaction control

```ruby
# Begin with MGA options
conn.begin_transaction(
  isolation_level: "READ COMMITTED",
  read_committed_mode: "READ COMMITTED READ CONSISTENCY",
  access_mode: "READ WRITE"
)

conn.savepoint("sp1")
conn.rollback_to_savepoint("sp1")
conn.release_savepoint("sp1")
conn.commit
# or:
conn.rollback
```

ScratchBird sessions are always in a transaction. `commit` / `rollback` immediately
reopen the next boundary. With `autocommit: false`, statements execute against the
server-owned session boundary (no synthetic client-side `BEGIN` is injected).

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

`Scratchbird::Protocol.canonical_read_committed_mode_label(mode)` returns the
canonical label for auditing.

Retry scope:
```ruby
scope = Scratchbird::ErrorMapper.retry_scope_for_sqlstate("40001")
# :statement | :reconnect | :none
```

---

## Type mapping

The gem maps to the `sbsql_core` profile. Ruby types are decoded from wire OIDs
using `Scratchbird::Types`.

| SBSQL canonical type | Ruby class |
|---|---|
| BOOLEAN | `true` / `false` |
| SMALLINT / INTEGER / BIGINT | `Integer` |
| REAL / DOUBLE PRECISION | `Float` |
| NUMERIC / DECIMAL | `BigDecimal` |
| TEXT / VARCHAR | `String` |
| BYTEA | `String` (binary encoding) |
| DATE | `Date` |
| TIME | `Time` |
| TIMESTAMP | `Time` |
| TIMESTAMPTZ | `Time` |
| INTERVAL | `Hash` (`:micros`, `:days`, `:months`) |
| UUID | `String` |
| JSON | `Hash` / `Array` (parsed) |
| JSONB | `Scratchbird::JSONB` |
| ARRAY | `Array` |
| RANGE | `Scratchbird::RangeValue` |
| Geometry | `Scratchbird::Geometry` |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full type reference.

---

## Metadata and introspection

```ruby
# Metadata collection with optional restrictions
rows = conn.query_metadata_with_restrictions(
  "tables", { "schema" => "public" }
)

# Schema tree (with optional parent expansion)
tree = conn.get_schema_tree("my_schema",
  expand_schema_parents: true
)

# Restriction-aware schema tree
tree = conn.get_schema_with_restrictions(
  "schemas", { "schema" => "public" },
  expand_schema_parents: true
)
```

Supported collections: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`, `routines`.

`Scratchbird::Metadata` provides `SchemaTreeNode`, `schema_paths_for_navigation`,
`build_schema_tree`, and `build_database_default_metadata_rows` for tree shaping.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Errors are raised as subclasses of `Scratchbird::Error`:

```ruby
begin
  conn.query("bad sql")
rescue Scratchbird::AuthorizationError => e
  puts "Auth failed: #{e.message}"
rescue Scratchbird::TransactionError => e
  scope = Scratchbird::ErrorMapper.retry_scope_for_sqlstate(e.sqlstate)
  retry if scope == :statement
rescue Scratchbird::Error => e
  puts "SQLSTATE=#{e.sqlstate}: #{e.message}"
end
```

`Scratchbird::Connection#supports_dormant_reattach?` returns `false`; dormant
detach/reattach methods fail closed until a public dormant token flow is available.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Resource resilience

`close` is idempotent — it finalizes keepalive unregistration and leak guard release
even when called from partial-startup or error paths. The `with_resilience` helper
provides circuit-breaker and telemetry accounting.

---

## Pooling and concurrency

The driver is rated `thread_safe`. The `connection_pool` capability is present.
Consult the application framework's connection pool (e.g., ActiveRecord connection
pool adapter) for pool management; the gem itself does not ship a standalone pool
class but is designed for safe use with external pools.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

This driver targets conformance profile `driver_ruby_gate`. JDBCBL baseline groups
CONN, TXN (Implemented), EXEC (Implemented), META (Partial — recursive tree shaping
present; full family and live integration coverage still expanding), TYPE, ERR, and
RES (Implemented) are covered. See the baseline mapping for remaining gaps.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/php.md -->

<a id="ch-client-driver-guide-drivers-php-md"></a>

# PHP Driver — PDO-style language binding for ScratchBird

The PHP driver (`scratchbird/pdo-scratchbird` Composer package) is a pure-PHP PDO-style
client for ScratchBird. It speaks SBWP v1.1 over TCP/TLS directly from PHP — no
extensions or native libraries are required. The primary entry points are
`ScratchBird\PDO\ScratchBirdPDO` (a PDO-compatible facade) and
`ScratchBird\Connection` (the lower-level driver connection). The driver is
`connection_thread_confined`, meaning each connection must be used from a single
PHP request/process thread.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0011-7000-8000-000000000011` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `connection_thread_confined` |
| Pooling capability | `session_pool` |
| Conformance profile ref | `driver_php_gate` |

---

## Installation

Composer package: `scratchbird/pdo-scratchbird`, version 0.1.0, license Apache-2.0.
Requires PHP >= 8.1.

```bash
composer require scratchbird/pdo-scratchbird
```

Autoload namespace: `ScratchBird\` and `ScratchBird\PDO\` map to `src/`.

---

## Connecting

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value string:
```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

### PDO-style connection (recommended)

```php
use ScratchBird\PDO\ScratchBirdPDO;

$pdo = new ScratchBirdPDO("scratchbird://user:pass@localhost:3092/mydb");
$stmt = $pdo->query("SELECT 1");
$row = $stmt->fetch();
```

The constructor signature is:
```php
ScratchBirdPDO(string $dsn, ?string $username = null, ?string $password = null, array $options = [])
```

### Lower-level `Connection`

```php
use ScratchBird\Connection;

$conn = new Connection("scratchbird://user:pass@localhost:3092/mydb");
$stream = $conn->query("SELECT id FROM items");
foreach ($stream as $row) {
    echo $row['id'] . "\n";
}
$conn->close();
```

### Key DSN parameters

| Key | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |
| `auth_payload_json` / `auth_payload_b64` | Structured auth payloads |
| `workload_identity_token` | Workload identity token |
| `proxy_principal_assertion` | Proxy principal assertion |

### Auth probe (staged bootstrap)

```php
use ScratchBird\Connection;

$surface = Connection::probeAuthSurface("scratchbird://user@host:3092/mydb");
// After connect:
$ctx = $conn->getResolvedAuthContext();
```

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### Query and execute via PDO

```php
// Query returning rows
$stmt = $pdo->query("SELECT id, name FROM items");
while ($row = $stmt->fetch(\PDO::FETCH_ASSOC)) {
    echo $row['name'] . "\n";
}

// Prepared statement
$stmt = $pdo->prepare("SELECT id FROM items WHERE id = ?");
$stmt->execute([42]);
$row = $stmt->fetch();

// Execute (no rows)
$affected = $pdo->exec("DELETE FROM items WHERE id = 99");
```

### Extended execution via `Connection`

```php
// Multi-result
$results = $conn->queryMulti("SELECT 1; SELECT 2");

// Batch
$summary = $conn->executeBatch("INSERT INTO t VALUES (?)", [[1], [2], [3]]);
// $summary->totalRowCount, $summary->items[0]->rowCount etc.

// Generated keys
$result = $conn->executeWithGeneratedKeys(
    "INSERT INTO t(name) VALUES (?) RETURNING id", ["Alice"]
);
$id = $conn->lastInsertId();

// Callable (JDBC escape syntax)
$conn->call("{ call my_proc(?) }", [$arg]);

// Native SQL normalization
$sql  = $conn->nativeSql("SELECT * FROM t WHERE id = ?");
$csql = $conn->nativeCallableSql("{ call proc(?) }");
```

### Statement traversal across multi-result

```php
$stmt = $pdo->query("SELECT 1; SELECT 2");
do {
    while ($row = $stmt->fetch()) { /* process */ }
} while ($stmt->nextRowset());
```

### Transaction control

```php
// Standard PDO transaction
$pdo->beginTransaction();
$pdo->exec("INSERT INTO t VALUES (1)");
$pdo->commit();
// or $pdo->rollBack();
echo $pdo->inTransaction() ? "in txn" : "no txn";

// Extended begin with MGA options
$conn->beginTransactionEx([
    'isolation_level'     => 'READ COMMITTED',
    'read_committed_mode' => 'READ COMMITTED READ CONSISTENCY',
    'access_mode'         => 'READ WRITE',
]);
$conn->savepoint("sp1");
$conn->rollbackToSavepoint("sp1");
$conn->releaseSavepoint("sp1");
$conn->commit();
```

ScratchBird sessions are always in a transaction. `commit()` / `rollback()` drain the
immediate reopen boundary before returning so the next statement sees real result frames.
`beginTransaction()` and `beginTransactionEx()` restart the current boundary.

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

`Protocol::canonicalReadCommittedModeLabel($mode)` returns the canonical label for
auditing.

Retry scope:
```php
use ScratchBird\ErrorMapper;
$scope = ErrorMapper::retryScopeForSqlState("40001"); // "statement"|"reconnect"|"none"
```

---

## Type mapping

The driver maps to the `sbsql_core` profile via `ScratchBird\TypeDecoder`. PHP
types received from the wire:

| SBSQL canonical type | PHP type |
|---|---|
| BOOLEAN | `bool` |
| SMALLINT / INTEGER / BIGINT | `int` |
| REAL / DOUBLE PRECISION | `float` |
| NUMERIC / DECIMAL | `string` (preserves precision) |
| TEXT / VARCHAR | `string` |
| BYTEA | `string` (binary) |
| DATE | `string` (ISO 8601) |
| TIME | `string` (ISO 8601) |
| TIMESTAMP | `string` (ISO 8601) |
| TIMESTAMPTZ | `string` (ISO 8601 with timezone) |
| INTERVAL | `string` |
| UUID | `string` |
| JSON | `array` / `string` (parsed) |
| JSONB | `ScratchBird\Jsonb` (object) |
| ARRAY | `array` |
| RANGE | `ScratchBird\Range` (object) |
| Geometry | `ScratchBird\Geometry` (object) |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full type reference.

---

## Metadata and introspection

```php
// Named collection
$rows = $conn->queryMetadata("tables");

// With restrictions
$rows = $conn->getSchema("columns", ["schema" => "public", "table" => "items"]);

// Recursive schema tree
$tree = $conn->getSchemaTree("my_schema", expandParents: true);
```

Supported collections: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`, `routines`.

`ScratchBird\Metadata` provides `buildMetadataSchemaTree`, `expandSchemaMetadataRows`,
`normalizeRestrictions`, and `filterRowsByRestrictions` for tree shaping and
restriction-aware filtering.

PDO wrappers: `$pdo->getSchema(collection)` and `$pdo->getSchemaTree(pattern)` are
also available.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Errors are thrown as typed subclasses of `ScratchBird\Errors\ScratchBirdException`:

```php
use ScratchBird\Errors\{
    ScratchBirdConnectionException,
    ScratchBirdTransactionException,
    ScratchBirdNotSupportedException,
};

try {
    $conn->query("bad sql");
} catch (ScratchBirdTransactionException $e) {
    $scope = ErrorMapper::retryScopeForSqlState($e->getSqlState());
} catch (ScratchBirdConnectionException $e) {
    // reconnect
}
```

`Connection::resumePortal()` fails closed with SQLSTATE `55000` unless the server first
reported `MSG_PORTAL_SUSPENDED`. `Connection::supportsDormantReattach()` returns
`false`; dormant methods fail closed with `0A000`.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

The driver is rated `connection_thread_confined` — each `Connection` or `ScratchBirdPDO`
instance must be used from a single thread/request. Pooling at the PHP FPM / application
level (e.g., persistent connections or a pool library) is appropriate. The driver's
`session_pool` capability means individual session lifecycle helpers are present in the
driver.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

This driver targets conformance profile `driver_php_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/swift.md -->

<a id="ch-client-driver-guide-drivers-swift-md"></a>

# Swift Driver — async/await language binding for ScratchBird

The Swift driver (`ScratchBird` SwiftPM package) is a native async/await client for
ScratchBird. It speaks SBWP v1.1 over TCP/TLS using SwiftNIO and NIOSSL (or Apple
Network framework when no certificate files are supplied on Apple platforms). The
primary entry point is `ScratchBirdConnection`, created via
`ScratchBirdConnection.connect(_:)`. A lightweight `ScratchBirdConnectionPool` is also
provided.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux. macOS is
expected to work with `swift build` but is not currently in CI. Windows is not
supported for this driver.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0016-7000-8000-000000000016` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `session_pool` |
| Conformance profile ref | `driver_swift_gate` |

---

## Installation

SwiftPM package: name `ScratchBird`, swift-tools-version 5.10.1, license MPL-2.0.
Platforms: macOS 13+, iOS 16+.

`Package.swift` dependency:
```swift
.package(url: "https://github.com/scratchbird/scratchbird-swift.git", from: "0.1.0")
```

Add to your target:
```swift
.target(
    name: "MyApp",
    dependencies: [.product(name: "ScratchBird", package: "scratchbird-swift")]
)
```

Build from source:
```bash
swift build
```

The package depends on `swift-crypto`, `swift-nio`, and `swift-nio-ssl`.

---

## Connecting

### Minimal connection example

```swift
import ScratchBird

let config = ScratchBirdConfig(dsn: "scratchbird://user:pass@localhost:3092/mydb")
let conn = try await ScratchBirdConnection.connect(config)
let result = try await conn.query("SELECT 1")
print(result.rows)
try await conn.close()
```

### Struct-based config

```swift
let config = ScratchBirdConfig(
    host: "localhost",
    port: 3092,
    database: "mydb",
    user: "user",
    password: "pass",
    sslmode: "verify-full",
    sslrootcert: "/etc/ssl/certs/ca.pem"
)
```

### Key `ScratchBirdConfig` fields

| Field | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `sslmode` | `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full` |
| `sslrootcert` / `sslcert` / `sslkey` / `sslpassword` | Certificate paths (loaded via NIOSSL) |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `manager_username` / `manager_database` | Manager bootstrap fields |
| `auth_token` | Bearer token for TOKEN auth |
| `auth_method_id` / `auth_method_payload` | Auth method override |
| `auth_payload_json` / `auth_payload_b64` | Structured auth payloads |
| `workload_identity_token` | Workload identity token |
| `proxy_principal_assertion` | Proxy principal assertion |
| `keepalive_interval_ms` | Keepalive interval |
| `leak_detection_threshold_ms` | Leak detection threshold |

### Auth probe (staged bootstrap)

```swift
let probe = try await ScratchBirdConnection.probeAuthSurface(config)
// After connect:
let ctx = conn.getResolvedAuthContext()
```

Supported direct auth: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`.
Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed.

---

## Executing statements and transactions

### Query and execute

```swift
// Simple query
let result: ScratchBirdResult = try await conn.query("SELECT id, name FROM items")
for row in result.rows {
    print(row)
}

// Parameterized
let result2 = try await conn.query("SELECT id FROM items WHERE id = $1", [42])

// Execute (no rows)
let _ = try await conn.query("DELETE FROM items WHERE id = $1", [99])
```

### Batch, multi-statement, and first-column helpers

```swift
// Sequential batch execution
let batchResults = try await conn.executeBatch(
    "INSERT INTO t(name) VALUES ($1)",
    paramsBatch: [["Alice"], ["Bob"]]
)

// Multi-statement helper
let multiResults = try await conn.queryMulti(["SELECT 1", "SELECT 2"])

// First-column extraction (generated-key style)
let newId = try await conn.executeReturningFirstColumn(
    "INSERT INTO t(name) VALUES ($1) RETURNING id", ["Carol"]
)
```

### Transaction control

```swift
// Begin with MGA options
try await conn.begin(ScratchBirdTxnOptions(
    isolationLevel: .readCommitted,
    readCommittedMode: ScratchBirdReadCommittedMode.readConsistency,
    accessMode: .readWrite
))

try await conn.savepoint("sp1")
try await conn.rollbackToSavepoint("sp1")
try await conn.releaseSavepoint("sp1")
try await conn.commit()
// or:
try await conn.rollback()
```

ScratchBird sessions are always in a transaction. `commit()` / `rollback()` drain the
reopen boundary before returning. `begin(...)` adopts an already-active fresh native
boundary when using compatible default options; non-default fresh-boundary adoption
fails closed with `0A000`.

Isolation aliases: `readCommitted` => canonical `READ COMMITTED`;
`repeatableRead` => canonical `SNAPSHOT`; `serializable` => canonical
`SNAPSHOT TABLE STABILITY`.

`ScratchBirdReadCommittedMode.readConsistency` maps to canonical
`READ COMMITTED READ CONSISTENCY`.

Retry scope:
```swift
let scope = conn.retryScope(forSqlState: "40001")
// .statement | .reconnect | .none
let retryable = conn.isRetryable(sqlState: "40001")
```

Prepared and limbo control:
```swift
let supported = conn.supportsPreparedTransactions()
try await conn.prepareTransaction("txn_name")
try await conn.commitPrepared("txn_name")
try await conn.rollbackPrepared("txn_name")
```

Dormant transactions: `conn.supportsDormantReattach()` returns `false`;
`detachToDormant()` and `reattachDormant(...)` fail closed with `0A000`.

---

## Type mapping

The driver maps to the `sbsql_core` profile. Swift types used in `ScratchBirdResult`
rows and parameters:

| SBSQL canonical type | Swift type |
|---|---|
| BOOLEAN | `Bool` |
| SMALLINT / INTEGER / BIGINT | `Int` |
| REAL / DOUBLE PRECISION | `Double` |
| NUMERIC / DECIMAL | `String` (preserves precision) |
| TEXT / VARCHAR | `String` |
| BYTEA | `Data` |
| DATE | `String` (ISO 8601) |
| TIME | `String` (ISO 8601) |
| TIMESTAMP | `String` (ISO 8601) |
| TIMESTAMPTZ | `String` (ISO 8601 with timezone) |
| INTERVAL | `Interval` (struct: `micros`, `days`, `months`) |
| UUID | `String` |
| JSON | `Json` (struct: `raw: Data`) |
| JSONB | `Jsonb` (struct: `raw: Data`) |
| INET / CIDR | `ScratchBirdInet` / `ScratchBirdCidr` (structs) |
| ARRAY | `[Any?]` |
| RANGE | `ScratchBirdRange` (struct) |
| Geometry | `Geometry` (struct: `wkb: Data`) |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full type reference.

---

## Metadata and introspection

Connection-level metadata wrappers query `sys.*` catalog families:

```swift
let schemas   = try await conn.metadataSchemas()
let tables    = try await conn.metadataTables()
let columns   = try await conn.metadataColumns()
let indexes   = try await conn.metadataIndexes()
let pkeys     = try await conn.metadataPrimaryKeys()
let fkeys     = try await conn.metadataForeignKeys()
let typeInfo  = try await conn.metadataTypeInfo()
// also: metadataIndexColumns, metadataConstraints, metadataProcedures,
//       metadataFunctions, metadataRoutines, metadataCatalogs,
//       metadataTablePrivileges, metadataColumnPrivileges

// Recursive schema tree
let treeRows = try await conn.metadataSchemaTreeRows()
let tree     = try await conn.metadataSchemaTree()
```

`Metadata.swift` provides `buildMetadataSchemaTree`, `buildMetadataSchemaTreeRows`,
`metadataSchemaPathsForNavigation`, and `splitMetadataSchemaPath` for tree shaping
with optional parent expansion and per-parent uniqueness.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md).

---

## Errors and diagnostics

Wire errors are mapped to typed Swift exceptions by SQLSTATE class or exact code:

```swift
import ScratchBird

do {
    let _ = try await conn.query("bad sql")
} catch let e as ScratchBirdConnectionException {
    print("Connection error: \(e.sqlState ?? "?") \(e.localizedDescription)")
} catch let e as ScratchBirdTransactionException {
    let scope = conn.retryScope(forSqlState: e.sqlState ?? "")
    if scope == .statement { /* retry statement */ }
} catch let e as ScratchBirdProgrammingException {
    // syntax / parameter error
}
```

Typed exception classes:
`ScratchBirdConnectionException`, `ScratchBirdAuthorizationException`,
`ScratchBirdDataException`, `ScratchBirdIntegrityException`,
`ScratchBirdTransactionException`, `ScratchBirdProgrammingException`,
`ScratchBirdNotSupportedException`, `ScratchBirdTimeoutException`,
`ScratchBirdOperationalException`.

All carry `sqlState`, `severity`, `detail`, and `hint` fields and preserve
`NSError` compatibility via `errorUserInfo`.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## Pooling and concurrency

```swift
let pool = ScratchBirdConnectionPool(config: config, maxSize: 4)

// Checkout / release
let conn = try await pool.acquire()
let result = try await conn.query("SELECT 1")
await pool.release(conn)

// Scoped helper
let result = try await pool.withConnection { conn in
    try await conn.query("SELECT 1")
}
await pool.close()
```

`ScratchBirdPoolStats` tracks active, idle, and total connections.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

---

## Conformance

This driver targets conformance profile `driver_swift_gate`. JDBCBL groups CONN and TXN
are `Implemented`. EXEC and META are `Partial` — API surfaces are present and unit-tested
but full live integration depth (cancellation timing, portal suspend/resume, live metadata
completeness) is still in progress. See the baseline mapping for remaining gaps.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/dart.md -->

<a id="ch-client-driver-guide-drivers-dart-md"></a>

# ScratchBird Dart Driver — Language Binding

> **Status: beta\_2 / release\_candidate** — The Dart driver is production-capable on
> Linux and Windows (CI-verified). macOS is untested. All public APIs are stable for
> the SBWP v1.1 wire protocol. Use cautiously in production until the full
> release gate is cleared.

## Purpose

The Dart driver provides a native, async-first client for ScratchBird (CDE —
Convergent Data Engine) applications written in Dart or Flutter. It speaks
SBWP v1.1 directly over TCP/TLS and does not require any ODBC or JDBC layer.
The driver is a pure Dart package: no compiled native extensions are needed.

Target audience: Dart/Flutter developers building server-side or mobile
applications that need direct ScratchBird access.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0002-7000-8000-000000000002`     |
| `driver_family`          | `dart`                                     |
| `api_surface_set`        | `language_binding`                         |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `session_pool`                             |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_dart_gate`                         |

## Install

The package name is `scratchbird` (see `pubspec.yaml`). Add it to your project:

```yaml
# pubspec.yaml
dependencies:
  scratchbird:
    path: /path/to/scratchbird   # local dev; pub.dev publication pending
```

Then fetch dependencies:

```bash
dart pub get
```

Runtime requirements: Dart SDK `>=3.2.0 <4.0.0`. Transitive dependencies:
`crypto: ^3.0.3`, `convert: ^3.1.1`.

## Connecting

The entry-point module is `package:scratchbird/scratchbird.dart`. The
primary public classes are `ScratchBirdConfig` and `ScratchBirdClient`.

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. Optional DSN query parameters include `sslmode`
(`disable` | `allow` | `prefer` | `require` | `verify-ca` | `verify-full`),
`application_name`, `front_door_mode` (`direct_listener` | `manager_proxy`),
`manager_auth_token`, and standard timeout aliases (`connect_timeout`,
`socket_timeout`). See ../connection\_and\_dsn.md for the full key reference.

**Minimal connection example (verified against `lib/src/client.dart`):**

```dart
import 'package:scratchbird/scratchbird.dart';

Future<void> main() async {
  final config = ScratchBirdConfig.fromDsn(
    'scratchbird://user:pass@localhost:3092/mydb',
  );
  final client = await ScratchBirdClient.connect(config);

  final result = await client.query('SELECT 1');
  print(result.rows);

  await client.close();
}
```

### Manager-proxy ingress

For environments where connections pass through the ScratchBird manager
process, add `front_door_mode=manager_proxy` and supply `manager_auth_token`
in the DSN query string or config. See ../authentication.md.

### Auth discovery

Before committing credentials you can probe the front-door auth surface:

```dart
final surface = await ScratchBirdClient.probeAuthSurface(config);
final ctx     = await client.resolvedAuthContext();
```

Supported native auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. Methods `MD5`, `PEER`, and `REATTACH` are admitted by the wire but
fail closed with a typed `ScratchBirdAuthException`.

## Executing Statements and Transactions

ScratchBird sessions are always in a transaction. `begin()` starts an
explicit transaction boundary; `commit()` and `rollback()` close it and
reopen the next boundary. There is no idle-session auto-begin.

```dart
// Simple query
final rows = await client.query('SELECT id, name FROM users WHERE active = $1',
    params: [true]);

// Explicit transaction
await client.begin(isolationLevel: 2); // SNAPSHOT (REPEATABLE READ alias)
try {
  await client.query('UPDATE counters SET n = n + 1 WHERE id = $1',
      params: [42]);
  await client.commit();
} catch (_) {
  await client.rollback();
  rethrow;
}

// Savepoints
await client.begin();
await client.savepoint('sp1');
// ... work ...
await client.releaseSavepoint('sp1');
await client.commit();
```

**Isolation-level aliases** (source: `lib/src/client.dart`):

| `isolationLevel` int | Wire canonical label             |
|----------------------|----------------------------------|
| `1` (`READ_COMMITTED`) | `READ COMMITTED`               |
| `2` (`REPEATABLE_READ`) | `SNAPSHOT`                    |
| `3` (`SERIALIZABLE`)  | `SNAPSHOT TABLE STABILITY`      |

The `READ COMMITTED` sub-mode is controlled by the `readCommittedMode`
parameter; use `canonicalReadCommittedModeLabel(mode)` to inspect the
canonical label at runtime.

**Retry guidance** (source: `lib/src/client.dart` — `retryScopeForSqlState`):

| SQLSTATE class | Retry boundary                 |
|----------------|--------------------------------|
| `40001`, `40P01` | Fresh statement only         |
| `08xxx`          | Reconnect / reopen only      |
| All others       | No automatic replay          |

### Prepared transactions and dormant sessions

`supportsPreparedTransactions()` → `true`; use `prepareTransaction(gid)`,
`commitPrepared(gid)`, `rollbackPrepared(gid)` for XA-style two-phase commit.
`supportsDormantReattach()` → `false`; `detachToDormant()` and
`reattachDormant()` fail closed with SQLSTATE `0A000` until the public front
door exposes the dormant token flow.

## Type Mapping

The driver uses OID-based binary and text wire encoding. Full type mapping
documentation: [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md).

| SBsql core type    | Dart type                | OID constant (src)      |
|--------------------|--------------------------|-------------------------|
| `BOOLEAN`          | `bool`                   | `oidBool` (16)          |
| `SMALLINT`         | `int`                    | `oidInt2` (21)          |
| `INTEGER`          | `int`                    | `oidInt4` (23)          |
| `BIGINT`           | `int`                    | `oidInt8` (20)          |
| `REAL`             | `double`                 | `oidFloat4` (700)       |
| `DOUBLE PRECISION` | `double`                 | `oidFloat8` (701)       |
| `NUMERIC`          | `String` (text form)     | `oidNumeric` (1700)     |
| `TEXT` / `VARCHAR` | `String`                 | `oidText`/`oidVarchar`  |
| `BYTEA`            | `Uint8List`              | `oidBytea` (17)         |
| `DATE`             | `DateTime`               | `oidDate` (1082)        |
| `TIME`             | `DateTime`               | `oidTime` (1083)        |
| `TIMESTAMP`        | `DateTime`               | `oidTimestamp` (1114)   |
| `TIMESTAMPTZ`      | `DateTime`               | `oidTimestamptz` (1184) |
| `UUID`             | `String`                 | `oidUuid` (2950)        |
| `JSON` / `JSONB`   | `String` / `Map`         | `oidJson`/`oidJsonb`    |
| `VECTOR`           | `List<double>`           | `oidVector` (16386)     |
| Arrays             | `List<T>`                | per element OID         |
| `COMPOSITE`        | `List<dynamic>`          | `oidRecord` (2249)      |
| Range types        | `Map` with bounds        | `oidInt4Range` etc.     |
| `INET` / `CIDR`    | `String`                 | `oidInet`/`oidCidr`     |
| `MACADDR`          | `String`                 | `oidMacaddr` (829)      |

## Metadata via `sys.information.*`

Full metadata reference: [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md).

The client exposes typed metadata wrapper methods (source: `lib/src/client.dart`):

```dart
final schemas    = await client.metadataSchemas();
final tables     = await client.metadataTables();
final columns    = await client.metadataColumns();
final indexes    = await client.metadataIndexes();
final pks        = await client.metadataPrimaryKeys();
final fks        = await client.metadataForeignKeys();
final routines   = await client.metadataRoutines();
final typeInfo   = await client.metadataTypeInfo();
final schemaTree = await client.getSchemaTree();
```

Lower-level access uses `client.query(metadataQuery)` with the SQL constants
from `lib/src/metadata.dart` (`MetadataCollectionName`,
`resolveMetadataCollectionQuery(name)`).

## Errors and Diagnostics

Full diagnostics reference: [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

Exception hierarchy (source: `lib/src/errors.dart`):

| Exception class                     | Typical SQLSTATE class   |
|-------------------------------------|--------------------------|
| `ScratchBirdException`              | base (all errors)        |
| `ScratchBirdConnectionException`    | `08xxx`                  |
| `ScratchBirdProtocolException`      | `08xxx`, framing errors  |
| `ScratchBirdAuthException`          | `28xxx`                  |
| `ScratchBirdTransactionException`   | `40xxx`, `25xxx`         |
| `ScratchBirdExecutionException`     | `42xxx`, `22xxx`         |
| `ScratchBirdOperationalException`   | `57xxx`, `54xxx`         |

Each exception exposes `.sqlState` (String?) and `.code` (int?).
Use `retryScopeForSqlState(sqlState)` to classify whether to retry at
statement or connection scope.

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

The driver is `thread_safe` (Dart isolate-safe) with `session_pool`
capability. Built-in resilience primitives:

- `lib/src/circuit_breaker.dart` — state-based breaker (SQLSTATE `08006` on open)
- `lib/src/keepalive.dart` — idle-connection ping with configurable window
- `lib/src/leak_detector.dart` — checkout tracking with `onLeakDetected` hook
- `lib/src/telemetry.dart` — slow-query retention, metrics, tracing

Pipeline capacity is controlled by `pipeline_max_in_flight` DSN key; overflow
emits SQLSTATE `54000`.

## Conformance

Full conformance reference: [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

Conformance gate: `driver_dart_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status      |
|--------------|-------------|
| `CONN`       | Implemented |
| `TXN`        | Partial     |
| `EXEC`       | Partial     |
| `META`       | Partial     |
| `TYPE`       | Partial     |
| `ERR`        | Partial     |
| `RES`        | Partial     |

Known open gaps: live integration coverage for pagination
(`portalSuspended`), SBLR execution, and complex binary type round-trips.

## Platform Support

| Platform | Status    |
|----------|-----------|
| Linux    | Supported (CI) |
| Windows  | Supported (CI) |
| macOS    | Untested  |

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire\_protocol\_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)
- [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md)
- [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md)
- [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md)
- [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md)
- [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/elixir.md -->

<a id="ch-client-driver-guide-drivers-elixir-md"></a>

# ScratchBird Elixir Driver — Language Binding (Ecto Adapter)

> **Status: beta\_2 / release\_candidate** — The Elixir driver is production-capable
> on Linux and Windows (CI-verified). macOS is untested. The Ecto adapter and
> native DBConnection client are stable for SBWP v1.1. Use cautiously in
> production until the full release gate is cleared.

## Purpose

The Elixir driver provides a native ScratchBird client for Elixir applications
using both a bare `ScratchBird.Connection` module and a full Ecto adapter
(`ScratchBird.Ecto`) for ORM-style access. It speaks SBWP v1.1 directly over
TCP/TLS via the DBConnection framework and does not wrap any JDBC or ODBC layer.

Target audience: Elixir/Phoenix developers who want native ScratchBird
connectivity with or without Ecto, and teams that need explicit access to
ScratchBird's MGA transaction semantics.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0004-7000-8000-000000000004`     |
| `driver_family`          | `elixir`                                   |
| `api_surface_set`        | `language_binding`                         |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `session_pool`                             |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_elixir_gate`                       |

## Install

The Mix package is `:scratchbird_ecto` (`mix.exs` app `:scratchbird_ecto`).
Add to your `mix.exs` (local dev path shown; Hex publication pending):

```elixir
# mix.exs
defp deps do
  [
    {:scratchbird_ecto, path: "/path/to/scratchbird"},
    {:ecto_sql, "~> 3.11"},
    {:db_connection, "~> 2.6"}
  ]
end
```

Elixir `~> 1.14` is required (the `mix.exs` declares `~> 1.14`; note that the
README states `~> 1.15` as the practical minimum for current OTP
compatibility). Then:

```bash
mix local.hex --force
mix local.rebar --force
mix deps.get
```

## Connecting

The primary entry-point modules are `ScratchBird.Connection` and
`ScratchBird` (the public facade).

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. Accepted `sslmode` values: `disable`, `allow`, `prefer`,
`require`, `verify-ca`, `verify-full`. `disable` is valid for local/dev use;
production deployments should prefer TLS-enabled modes. See
../connection\_and\_dsn.md for the full key reference.

**Minimal connection example (bare `ScratchBird.Connection`):**

```elixir
config = [
  url: "scratchbird://user:pass@localhost:3092/mydb",
  application_name: "my_app"
]

{:ok, conn} = ScratchBird.Connection.connect(config)
{:ok, result, conn} = ScratchBird.Connection.query(conn, "SELECT 1", [])
IO.inspect(result.rows)
```

**Ecto adapter** (source: `lib/scratchbird_ecto/connection.ex`):

```elixir
# config/config.exs
config :my_app, MyApp.Repo,
  adapter: ScratchBird.Ecto,
  url: "scratchbird://user:pass@localhost:3092/mydb"
```

### Manager-proxy ingress

Set `front_door_mode: "manager_proxy"` in the connection config and supply
`manager_auth_token`. Manager-proxy mode uses staged probe/bootstrap: call
`ScratchBird.probe_auth_surface/1` or
`ScratchBird.Connection.probe_auth_surface/1` to inspect the front-door auth
requirement before committing credentials. See ../authentication.md.

### Auth discovery

```elixir
{:ok, surface} = ScratchBird.probe_auth_surface(config)
context = ScratchBird.get_resolved_auth_context(conn)
```

Supported native auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`, `manager_proxy` token bootstrap. Methods `MD5`, `PEER`, and
`REATTACH` are admitted by wire negotiation but fail closed.

## Executing Statements and Transactions

ScratchBird sessions are always in a transaction. Native `READY`, `TXN_STATUS`,
and `current_txn_id` are authoritative for transaction activity. `COMMIT` and
`ROLLBACK` reopen the next boundary; `begin/2` is documented against this
always-in-transaction contract rather than idle-session semantics.

```elixir
# Simple query
{:ok, result, conn} = ScratchBird.Connection.query(conn, "SELECT id FROM users", [])

# Explicit transaction with options
{:ok, conn} = ScratchBird.Connection.begin(conn, isolation: :repeatable_read)
case ScratchBird.Connection.query(conn, "UPDATE ...", []) do
  {:ok, _, conn} -> ScratchBird.Connection.commit(conn)
  {:error, _}   -> ScratchBird.Connection.rollback(conn)
end
```

**Isolation-level aliases for `begin/2`** (source: `lib/scratchbird/connection.ex`):

| Elixir atom        | Wire canonical label             |
|--------------------|----------------------------------|
| `:read_committed`  | `READ COMMITTED`                 |
| `:repeatable_read` | `SNAPSHOT`                       |
| `:serializable`    | `SNAPSHOT TABLE STABILITY`       |

The `READ COMMITTED` sub-mode is selected via `:read_committed_mode` in the
`begin/2` option map. `ScratchBird.canonical_read_committed_mode_label/1`
returns the canonical label for a given sub-mode integer.

**Retry boundary** (source: `lib/scratchbird/errors.ex` — `retry_scope/1`):

| SQLSTATE         | Retry boundary                  |
|------------------|---------------------------------|
| `40001`, `40P01` | Fresh statement only            |
| `08xxx`          | Reconnect / reopen only         |
| All others       | No automatic replay             |

### Reconnect behaviour

This lane uses fresh-connect-only recovery. `disconnect/2` tears down the
current wire session; replacement sessions come from a new
`ScratchBird.Connection.connect/1` handshake or DBConnection reconnect cycle.
Reconnect never replays abandoned transactions.

### Prepared transactions

`supports_prepared_transactions/0` is `true`. Use
`prepare_transaction/2`, `commit_prepared/2`, and `rollback_prepared/2` for
two-phase commit with canonical transaction-control SQL.
`supports_dormant_reattach/0` is `false`; related helpers fail closed with
SQLSTATE `0A000`.

## Type Mapping

Full type mapping: [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md).

| SBsql core type    | Elixir type                | OID constant (src)      |
|--------------------|----------------------------|-------------------------|
| `BOOLEAN`          | `true` / `false`           | `@oid_bool` (16)        |
| `SMALLINT`         | `integer`                  | `@oid_int2` (21)        |
| `INTEGER`          | `integer`                  | `@oid_int4` (23)        |
| `BIGINT`           | `integer`                  | `@oid_int8` (20)        |
| `REAL`             | `float`                    | `@oid_float4` (700)     |
| `DOUBLE PRECISION` | `float`                    | `@oid_float8` (701)     |
| `NUMERIC`          | `Decimal.t()` (`decimal`)  | `@oid_numeric` (1700)   |
| `TEXT` / `VARCHAR` | `String`                   | `@oid_text`/`@oid_varchar` |
| `BYTEA`            | binary                     | (bytea OID)             |
| `DATE`             | `Date.t()`                 | `@oid_date` (1082)      |
| `TIME`             | `Time.t()`                 | `@oid_time` (1083)      |
| `TIMESTAMP`        | `NaiveDateTime.t()`        | `@oid_timestamp` (1114) |
| `TIMESTAMPTZ`      | `DateTime.t()`             | `@oid_timestamptz` (1184)|
| `UUID`             | `String`                   | `@oid_uuid` (2950)      |
| `JSON` / `JSONB`   | decoded term (via `:jason`) | `@oid_json`/`@oid_jsonb`|
| `VECTOR`           | `[float]` list             | `@oid_sb_vector` (16386)|
| Arrays             | `[T]` list                 | per element OID         |
| `COMPOSITE`        | list of values             | `@oid_record` (2249)    |
| Range types        | map with bounds            | `@oid_int4range` etc.   |
| `INET` / `CIDR`    | `String`                   | `@oid_inet`/`@oid_cidr` |
| `MACADDR`          | `String`                   | `@oid_macaddr` (829)    |

## Metadata via `sys.information.*`

Full reference: [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md).

The lane exposes direct metadata query helpers (source: `lib/scratchbird/metadata.ex`):

```elixir
ScratchBird.Connection.schemas_query(conn)
ScratchBird.Connection.tables_query(conn)
ScratchBird.Connection.columns_query(conn)
ScratchBird.Connection.indexes_query(conn)
ScratchBird.Connection.primary_keys_query(conn)
ScratchBird.Connection.foreign_keys_query(conn)
ScratchBird.Connection.routines_query(conn)
ScratchBird.Connection.catalogs_query(conn)
ScratchBird.Connection.type_info_query(conn)
```

The remaining metadata gap is live-wire proof for restrictions and wildcards;
local query families are present.

## Errors and Diagnostics

Full reference: [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

Error classification is handled by `ScratchBird.Errors` (source:
`lib/scratchbird/errors.ex`):

```elixir
ScratchBird.Errors.sqlstate_class("08006")  # => :connection_exception
ScratchBird.Errors.retry_scope("40001")     # => :statement
ScratchBird.Errors.retryable?("08001")      # => true
```

SQLSTATE class mapping covers: `:warning`, `:no_data`,
`:connection_exception`, `:feature_not_supported`, `:data_exception`,
`:integrity_constraint_violation`, `:invalid_authorization`,
`:transaction_rollback`, `:syntax_error_or_access_rule_violation`,
`:insufficient_resources`, `:program_limit_exceeded`,
`:operator_intervention`, `:system_error`, `:internal_error`.

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

The driver is `thread_safe` with `session_pool` capability. Resilience
primitives (source: `lib/scratchbird/circuit_breaker.ex`, `keepalive.ex`,
`leak_detector.ex`, `telemetry.ex`):

- **Circuit breaker** — state-based failure/recovery tracking
- **Keepalive** — idle-window validation and periodic ping
- **Leak detector** — checkout tracking
- **Telemetry** — slow-query retention, metrics

Pooling uses DBConnection's built-in pool infrastructure. For external pool
configuration see ../pooling\_and\_concurrency.md.

## Conformance

Full reference: [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

Conformance gate: `driver_elixir_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status      |
|--------------|-------------|
| `CONN`       | Implemented |
| `TXN`        | Implemented |
| `EXEC`       | Partial     |
| `META`       | Implemented |
| `TYPE`       | Implemented |
| `ERR`        | Implemented |
| `RES`        | Partial     |

Known open gap: deterministic stream/paging proof and live metadata
integration (local query families are complete).

## Platform Support

| Platform | Status    |
|----------|-----------|
| Linux    | Supported (CI) |
| Windows  | Supported (CI) |
| macOS    | Untested  |

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire\_protocol\_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)
- [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md)
- [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md)
- [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md)
- [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md)
- [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/mojo.md -->

<a id="ch-client-driver-guide-drivers-mojo-md"></a>

# ScratchBird Mojo Driver — Language Binding (Experimental)

> **Status: beta\_2 / release\_candidate (experimental)** — The Mojo driver is
> validated on Linux only using a pixi-managed Mojo toolchain. Windows and macOS
> are not supported. The current implementation is a Mojo-Python interop lane:
> the public API is expressed in Mojo syntax but execution delegates to a Python
> transport shim (`src/scratchbird.py`) with an opt-in SBWP wire bridge
> (`sb_wire_transport=python`). Native Mojo socket/TLS transport is future work.

## Purpose

The Mojo driver provides a ScratchBird client surface for programs written in
Mojo (Magic's AI-native programming language). Because Mojo's native socket and
TLS ecosystem is still maturing, the lane currently operates as a Mojo-Python
interop shim. The facade module (`src/scratchbird.mojo`) re-exports the public
API from the native bootstrap (`src/scratchbird_native.mojo`), while real wire
execution routes through the Python-backed `_PythonWireConnection` adapter in
`src/scratchbird.py`.

Target audience: Mojo/MAX developers who need ScratchBird connectivity today
and want to migrate to pure native transport as the Mojo toolchain matures.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0007-7000-8000-000000000007`     |
| `driver_family`          | `mojo`                                     |
| `api_surface_set`        | `language_binding`                         |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `session_pool`                             |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_mojo_gate`                         |

## Requirements and Install

- Python 3.10+ (for the bridge shim)
- Mojo toolchain via `pixi` (recommended workspace: `~/mojo-work/sb-mojo`)

There is no published package yet. Use the lane directly from the source tree:

```bash
# Install pixi (https://prefix.dev/docs/pixi/overview)
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 \
  -I src -I src/scratchbird \
  tests/scratchbird_surface.mojo
```

## Connecting

The entry-point modules are `src/scratchbird.mojo` (Mojo facade) and
`src/scratchbird.py` (Python-backed shim/bridge).

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. Bracketed IPv6 hosts are supported (`[::1]:3092`). Key
aliases are extensive; for example `user|username|pguser`, `host|hostname|servername|pghost`,
`database|dbname|databaseName|pgdatabase`. The full alias list and all DSN
options are documented in ../connection\_and\_dsn.md.

`sslmode` / `ssl` values: `disable`, `allow`, `prefer`, `require`,
`verify-ca`, `verify-full`. `sslmode=disable` is accepted; production should
use a TLS-enabled mode.

**Connection from Mojo (via facade, verified against `src/scratchbird.mojo`):**

```mojo
from scratchbird import ScratchBirdConfig, ScratchBirdConnection, connect

fn main() raises:
    let cfg = ScratchBirdConfig.from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb"
    )
    let conn = connect(cfg)
    let rows = conn.query("SELECT 1")
    print(rows)
    conn.close()
```

**Connection from the Python shim (bridge mode, `src/scratchbird.py`):**

```python
from scratchbird import ScratchBirdConnection

conn = ScratchBirdConnection.connect(
    "scratchbird://user:pass@localhost:3092/mydb"
)
rows = conn.query("SELECT 1")
conn.close()
```

Wire execution is activated via `sb_wire_transport=python` DSN key or the
`SCRATCHBIRD_MOJO_WIRE_TRANSPORT` environment variable. Without it the
connection operates in deterministic (test-shim) mode.

### Auth discovery

```python
surface = probe_auth_surface("scratchbird://user@localhost:3092/mydb")
ctx     = conn.get_resolved_auth_context()
```

Supported auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`,
`manager_proxy` token bootstrap. Methods `MD5`, `PEER`, and `REATTACH` fail
closed.

## Executing Statements and Transactions

```python
# Simple query
rows = conn.query("SELECT id FROM users WHERE active = ?", [True])

# Parameterized (explicit)
rows = conn.query("SELECT * FROM t WHERE n = ?", [42])

# Transaction
conn.begin(isolation_level=2)  # SNAPSHOT
conn.query("UPDATE ...")
conn.commit()

# Savepoint
conn.begin()
conn.savepoint("sp1")
conn.rollback_to_savepoint("sp1")
conn.commit()
```

**Isolation-level aliases** (source: `src/scratchbird.py` —
`canonical_isolation_label`):

| Integer / alias       | Wire canonical label             |
|-----------------------|----------------------------------|
| `1` / `READ_COMMITTED`  | `READ COMMITTED`               |
| `2` / `REPEATABLE_READ` | `SNAPSHOT`                     |
| `3` / `SERIALIZABLE`    | `SNAPSHOT TABLE STABILITY`     |
| `0` / `READ_UNCOMMITTED`| legacy compatibility alias     |

`READ COMMITTED` sub-mode is set via `read_committed_mode`; use
`canonical_read_committed_mode_label(mode)` for the canonical label.

**Retry boundary** (source: `src/scratchbird.py` —
`retry_scope_for_sqlstate`):

| SQLSTATE         | Retry boundary                  |
|------------------|---------------------------------|
| `40001`, `40P01` | Fresh statement only            |
| `08xxx`          | Reconnect / reopen only         |
| All others       | No automatic replay             |

### Prepared transactions and dormant sessions

`supports_prepared_transactions()` → `true`. Use `prepare_transaction(gid)`,
`commit_prepared(gid)`, `rollback_prepared(gid)`.
`supports_dormant_reattach()` → `false`; related helpers fail closed with
SQLSTATE `0A000`.

### Streaming and cancellation

```python
stream = conn.stream("SELECT * FROM big_table")
for row in stream:
    process(row)
stream.close()

# Cancel in-flight
conn.cancel()  # SQLSTATE 57014 on the server side
```

Closed-stream reads raise with SQLSTATE `HY010`. Active-stream reads on a
closed connection raise `08003`.

### Pipeline and circuit breaker

Pipeline capacity is bounded by `pipeline_max_in_flight` DSN key; overflow
emits SQLSTATE `54000`. Circuit-breaker open state emits SQLSTATE `08006`.
The half-open recovery window is controlled by `cb_half_open_max_requests`.

## Type Mapping

Full reference: [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md).

| SBsql core type    | Python/Mojo type       | OID constant (src)         |
|--------------------|------------------------|----------------------------|
| `INTEGER`          | `int`                  | `OID_INT4` (23)            |
| `TEXT` / `VARCHAR` | `str`                  | `OID_TEXT`/`OID_VARCHAR`   |
| `DATE`             | `datetime.date`        | `OID_DATE` (1082)          |
| `TIME`             | `datetime.time`        | `OID_TIME` (1083)          |
| `TIMESTAMP`        | `datetime.datetime`    | `OID_TIMESTAMP` (1114)     |
| `TIMESTAMPTZ`      | `datetime.datetime`    | `OID_TIMESTAMPTZ` (1184)   |
| `JSON` / `JSONB`   | `dict` / `list`        | `OID_JSON`/`OID_JSONB`     |
| `UUID`             | `str`                  | `OID_UUID` (2950)          |
| `INET` / `CIDR`    | `str`                  | `OID_INET`/`OID_CIDR`      |
| `MACADDR`          | `str`                  | `OID_MACADDR` (829)        |
| `VECTOR`           | `[float]`              | `OID_SB_VECTOR` (16386)    |
| Arrays             | `list`                 | e.g. `OID_INT4_ARRAY` (1007)|
| `COMPOSITE`        | `list`                 | `OID_RECORD` (2249)        |

Type codecs (source: `src/scratchbird.py`) cover array/range/vector, composite,
geometry, inet/cidr/macaddr, json/jsonb, uuid, and temporal types including
intervals.

## Metadata via `sys.information.*`

Full reference: [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md).

Metadata query constants are defined in `src/scratchbird.py` and exported via
`src/scratchbird.mojo`. Access via the native bootstrap or shim:

```python
conn.query_metadata("schemas")
conn.query_metadata("tables")
conn.query_metadata("columns")
conn.query_metadata_restricted("tables", {"TABLE_SCHEM": "public"})
conn.query_metadata_restricted_multi("tables",
    {"TABLE_SCHEM": "public", "TABLE_NAME": "users"})
conn.get_schema("tables")
conn.ddl_editor_schema_payload(schema_pattern="public")
```

Collection names: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `procedures`, `functions`, `routines`, `catalogs`,
`primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`type_info`.

## Errors and Diagnostics

Full reference: [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

The shim raises `ScratchBirdError(message, sqlstate=..., detail=..., hint=...)`
(source: `src/scratchbird.py`). Use `extract_sqlstate(err_str)` (source:
`src/scratchbird_native.mojo` / `src/scratchbird.mojo`) to extract the SQLSTATE
code from error strings in Mojo tests.

Key SQLSTATE codes emitted by the driver:

| Condition                          | SQLSTATE  |
|------------------------------------|-----------|
| Connection closed, op attempted    | `08003`   |
| Circuit breaker open               | `08006`   |
| Manager proxy token missing        | `08001`   |
| Auth failure                       | `28P01`   |
| Invalid DSN integer parameter      | `22023`   |
| Unsupported operation              | `0A000`   |
| Closed statement / stream          | `HY010`   |
| Pipeline capacity exceeded         | `54000`   |
| Query cancelled                    | `57014`   |
| Parameter count mismatch           | `07001`   |
| Savepoint missing                  | `3B001`   |

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

The driver is `thread_safe` with `session_pool` capability. Lifecycle
scaffolds (source: `src/scratchbird/` directory):

- `circuit_breaker.mojo` — state-based breaker
- `keepalive.mojo` — idle-window tracker and manager
- `leak_detector.mojo` — checkout/checkin bookkeeping
- `telemetry.mojo` — tracing, metrics, slow-query retention
- `pipeline.mojo` — queue and flush management

## Conformance

Full reference: [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

Conformance gate: `driver_mojo_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status                                    |
|--------------|-------------------------------------------|
| `CONN`       | Implemented (native facade + wire bridge) |
| `TXN`        | Implemented (hybrid parity)               |
| `EXEC`       | Implemented (hybrid parity)               |
| `META`       | Implemented (hybrid parity)               |
| `TYPE`       | Implemented (deterministic + wire bridge) |
| `ERR`        | Implemented (deterministic + wire negative-path) |
| `RES`        | Implemented (deterministic + wire lifecycle) |

Open gap: pure Mojo-native socket/TLS transport (replacing the Python bridge).

## Platform Support

| Platform | Status      |
|----------|-------------|
| Linux    | Experimental (pixi-managed Mojo toolchain) |
| Windows  | Not supported |
| macOS    | Not supported |

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire\_protocol\_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)
- [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md)
- [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md)
- [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md)
- [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md)
- [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/pascal.md -->

<a id="ch-client-driver-guide-drivers-pascal-md"></a>

# ScratchBird Pascal/Delphi Driver — Language Binding

> **Status: beta\_2 / release\_candidate** — The Pascal driver is supported on
> Linux and Windows (CI-verified via FreePascal). macOS is untested. The driver
> includes native TLS via OpenSSL and adapters for FireDAC, IBX, Zeos, and
> SQLdb. Use cautiously in production until the full release gate is cleared.

## Purpose

The Pascal/Delphi driver (`ScratchBird.Client`) provides native ScratchBird
wire-protocol access for Object Pascal applications compiled with FreePascal
(`fpc -Mdelphi`) or Delphi. It includes a standalone low-level client class
(`TScratchBirdClient`) and adapter shims for four major Pascal database
frameworks: FireDAC, IBX, Zeos, and SQLdb.

Target audience: Pascal/Delphi developers using RAD tools or writing server
applications who want a first-party ScratchBird client without a PostgreSQL
compatibility layer.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0010-7000-8000-000000000010`     |
| `driver_family`          | `pascal`                                   |
| `api_surface_set`        | `language_binding`                         |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `session_pool`                             |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_pascal_gate`                       |

## Install / Build

The driver is compiled from source using FreePascal. Runtime TLS requires
`libssl` and `libcrypto` to be present on the target system.

**Linux / Windows (FreePascal):**

```bash
fpc -Mdelphi \
    -Fu./src \
    -FU./build \
    -FE./bin \
    YourApp.pas
```

Source units in `src/` include:
`ScratchBird.Client.pas`, `ScratchBird.Config.pas`,
`ScratchBird.Protocol.pas`, `ScratchBird.Types.pas`,
`ScratchBird.Metadata.pas`, `ScratchBird.Errors.pas`,
`ScratchBird.Transport.Native.pas`, `ScratchBird.Tls.Context.pas`,
`ScratchBird.Scram.pas`, `ScratchBird.AuthBootstrap.pas`,
`SBCircuitBreaker.pas`, `SBKeepalive.pas`, `SBLeakDetector.pas`,
`SBPipeline.pas`, `SBTelemetry.pas`.

Adapter units: `ScratchBird.FireDAC.pas`, `ScratchBird.IBX.pas`,
`ScratchBird.Zeos.pas`, `ScratchBird.SQLdb.pas`.

**Legacy Indy transport:** compile with `-dSCRATCHBIRD_USE_INDY` and add
vendored Indy paths (`third_party/indy/Lib/Core`, `Lib/Protocols`,
`Lib/System`, `Lib/Security`) if migration from Indy is still in progress.

## Connecting

The primary entry-point unit is `ScratchBird.Client`. The main class is
`TScratchBirdClient`.

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. `sslmode` options: `disable`, `allow`, `prefer`,
`require`, `verify-ca`, `verify-full`. `sslmode=disable` maps to plaintext
socket mode. See ../connection\_and\_dsn.md for the full key reference.

**Minimal connection example (core client, verified against
`src/ScratchBird.Client.pas`):**

```pascal
uses
  ScratchBird.Client;

var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    Client.Connect('scratchbird://user:pass@localhost:3092/mydb');
    Client.ExecSQL('SELECT 1');
  finally
    Client.Free;
  end;
end;
```

### Manager-proxy ingress

Manager-proxy mode requires `manager_auth_token` to be present in the DSN or
config before `Connect` is called; the client performs a fail-fast check
before any network dial. See ../authentication.md.

### Auth discovery

```pascal
var Surface: TAuthSurface;
    Ctx: TResolvedAuthContext;
begin
  Surface := TScratchBirdClient.ProbeAuthSurface(dsn);
  Ctx := Client.GetResolvedAuthContext();
end;
```

Supported native auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. Methods `MD5`, `PEER`, and `REATTACH` are admitted by wire
negotiation but fail closed with typed auth errors.

## Executing Statements and Transactions

```pascal
// Simple execution
Client.ExecSQL('UPDATE counters SET n = n + 1 WHERE id = 1');

// Parameterized
Client.ExecSQL('SELECT * FROM users WHERE id = $1', [42]);

// Explicit transaction
Client.BeginTransactionEx(isoReadCommitted, amReadWrite);
try
  Client.ExecSQL('UPDATE ...');
  Client.Commit;
except
  Client.Rollback;
  raise;
end;

// Savepoint
Client.BeginTransactionEx(isoSnapshot, amReadWrite);
Client.Savepoint('sp1');
// ... work ...
Client.ReleaseSavepoint('sp1');
Client.Commit;
```

**`BeginTransactionEx` isolation options** (source:
`src/ScratchBird.Client.pas`, `src/ScratchBird.Protocol.pas`):

| Pascal constant      | Wire canonical label             |
|----------------------|----------------------------------|
| `isoReadUncommitted` | legacy compatibility alias       |
| `isoReadCommitted`   | `READ COMMITTED`                 |
| `isoRepeatableRead`  | `SNAPSHOT`                       |
| `isoSerializable`    | `SNAPSHOT TABLE STABILITY`       |

The `READ COMMITTED` sub-mode is set via the overloaded
`BeginTransactionEx(..., ReadCommittedMode)` or the adapter surface
`StartTransactionEx(..., ReadCommittedMode)`. Use
`CanonicalReadCommittedModeName(mode)` to retrieve the canonical string.

**Retry boundary** (source: `src/ScratchBird.Errors.pas` —
`RetryScopeForSqlState`):

| SQLSTATE         | Retry boundary                  |
|------------------|---------------------------------|
| `40001`, `40P01` | Fresh statement only            |
| `08xxx`          | Reconnect / reopen only         |
| All others       | No automatic replay             |

### Batch and multi-result execution

```pascal
// ExecuteBatch: per-statement summary output
Client.ExecuteBatch(['INSERT ...', 'UPDATE ...']);

// QueryMulti: per-statement rowset materialization
var Results: TQueryMultiResult;
Results := Client.QueryMulti(['SELECT ...', 'SELECT ...']);
```

### Generated keys

`TScratchBirdResultStream` exposes `LastInsertId` and `HasLastInsertId`
from the `MSG_COMMAND_COMPLETE` message when the engine returns a generated
key. Opt-in live verification requires the `SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL`
environment variable (see integration tests).

### Prepared transactions and dormant sessions

`SupportsPreparedTransactions()` → `true`. Use `PrepareTransaction(gid)`,
`CommitPrepared(gid)`, `RollbackPrepared(gid)` for two-phase commit.
`SupportsDormantReattach()` → `false`; `DetachToDormant()` and
`ReattachDormant()` fail closed with SQLSTATE `0A000`.

### Portal resume

Result stream continuation (`TScratchBirdResultStream`) is gated on receiving
an explicit `MSG_PORTAL_SUSPENDED` state; blind resume attempts fail closed
with SQLSTATE `55000`.

## Adapter Usage

Each adapter unit wraps `TScratchBirdClient` under the standard framework
interface (source: `src/ScratchBird.FireDAC.pas` etc.):

```pascal
// FireDAC
uses ScratchBird.FireDAC;
var Conn: TSBFireDACConnection;
begin
  Conn := TSBFireDACConnection.Create(nil);
  Conn.ConnectionString := 'scratchbird://user:pass@localhost:3092/mydb';
  Conn.Connected := True;
end;

// IBX, Zeos, SQLdb follow the same pattern with their own adapter classes.
```

Transaction begin/commit/rollback are forwarded to `TScratchBirdClient`
through overridable execution hooks. The `StartTransactionEx` adapter method
exposes the full `ReadCommittedMode` parameter.

## Type Mapping

Full reference: [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md).

| SBsql core type       | Pascal type                  | OID constant (src)        |
|-----------------------|------------------------------|---------------------------|
| `BOOLEAN`             | `Boolean`                    | `OID_BOOL` (16)           |
| `SMALLINT`            | `SmallInt`                   | `OID_INT2` (21)           |
| `INTEGER`             | `Integer`                    | `OID_INT4` (23)           |
| `BIGINT`              | `Int64`                      | `OID_INT8` (20)           |
| `REAL`                | `Single`                     | `OID_FLOAT4` (700)        |
| `DOUBLE PRECISION`    | `Double`                     | `OID_FLOAT8` (701)        |
| `NUMERIC`             | `Currency` / `Extended`      | `OID_NUMERIC` (1700)      |
| `TEXT` / `VARCHAR`    | `String`                     | `OID_TEXT`/`OID_VARCHAR`  |
| `BYTEA`               | `TBytes`                     | `OID_BYTEA` (17)          |
| `DATE`                | `TDate`                      | `OID_DATE` (1082)         |
| `TIME`                | `TTime`                      | `OID_TIME` (1083)         |
| `TIMETZ`              | `TTimeTz` record             | `OID_TIMETZ` (1266)       |
| `TIMESTAMP`           | `TDateTime`                  | `OID_TIMESTAMP` (1114)    |
| `TIMESTAMPTZ`         | `TDateTime`                  | `OID_TIMESTAMPTZ` (1184)  |
| `INTERVAL`            | `TScratchBirdInterval`       | `OID_INTERVAL` (1186)     |
| `UUID`                | `String`                     | `OID_UUID` (2950)         |
| `JSON` / `JSONB`      | `String`                     | `OID_JSON`/`OID_JSONB`    |
| `VECTOR`              | `array of Double`            | `OID_SB_VECTOR` (16386)   |
| `INET` / `CIDR`       | `String`                     | `OID_INET`/`OID_CIDR`     |
| `MACADDR` / `MACADDR8`| `String`                     | `OID_MACADDR`/`OID_MACADDR8`|
| Geometry              | OID-typed wrapper            | `OID_POINT` (600) etc.    |
| `COMPOSITE`           | `TArray<Variant>` (guarded)  | `OID_RECORD` (2249)       |
| Range types           | map/record with bounds       | `OID_INT4RANGE` (3904) etc.|

Pascal-specific: `TIMETZ` decode handles both 12-byte and backward-compatible
8-byte payloads. Geometry wrappers (`TSBPoint`, `TSBLseg`, etc.) preserve the
OID on decode. Malformed composite frames produce null-materialized records
rather than raising.

## Metadata via `sys.information.*`

Full reference: [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md).

Source unit: `src/ScratchBird.Metadata.pas`. API:

```pascal
// Stream API
var Stream: TScratchBirdResultStream;
Stream := Client.QueryMetadata('tables');

// Materialized rows
var Rows: TMetadataRows;
Rows := Client.QueryMetadataRows('tables', ['TABLE_SCHEM=public']);

// Typed wrappers
var Schemas: TMetadataRows;
Schemas := Client.Schemas;
Tables   := Client.Tables;
Columns  := Client.Columns;
// Also: Indexes, IndexColumns, Constraints, Routines, Catalogs,
//       PrimaryKeys, ForeignKeys, TablePrivileges, ColumnPrivileges, TypeInfo.

// Recursive schema tree
var Tree: TMetadataSchemaTree;
Tree := Client.BuildMetadataSchemaTree(expandParents := True);
```

Restriction filtering supports exact match (`=`), wildcard (`LIKE ... ESCAPE
'\'`), and null (`IS NULL`) predicates via `FilterMetadataRowsByRestrictions`.

## Errors and Diagnostics

Full reference: [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

Source unit: `src/ScratchBird.Errors.pas`.

```pascal
var Cat: TScratchBirdErrorCategory;
Cat := MapSqlState('08006');       // Returns ecConnectionException
var Scope: TRetryScope;
Scope := RetryScopeForSqlState('40001');  // Returns rsStatement
var Retryable: Boolean;
Retryable := IsRetryableSqlState('40001'); // Returns True
```

Error categories: `ecWarning`, `ecNoData`, `ecConnectionException`,
`ecFeatureNotSupported`, `ecDataException`, `ecIntegrityConstraint`,
`ecInvalidAuthorization`, `ecTransactionRollback`,
`ecSyntaxErrorOrAccessRule`, `ecInsufficientResources`,
`ecProgramLimitExceeded`, `ecOperatorIntervention`, `ecSystemError`,
`ecInternalError`, `ecGeneric`.

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md).

The driver is `thread_safe` with `session_pool` capability. Resilience
primitives (source: `src/SBCircuitBreaker.pas`, `src/SBKeepalive.pas`,
`src/SBLeakDetector.pas`, `src/SBTelemetry.pas`, `src/SBPipeline.pas`):

- **Circuit breaker** — state-based, half-open recovery
- **Keepalive** — idle-window tracking with `MarkActive`; pinger invocation
- **Leak detector** — checkout/checkin bookkeeping with background thread
- **Telemetry** — operation tracking and slow-query retention
- **Pipeline** — queued flush management

## Conformance

Full reference: [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md).

Conformance gate: `driver_pascal_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status      |
|--------------|-------------|
| `CONN`       | Implemented |
| `TXN`        | Implemented |
| `EXEC`       | Implemented |
| `META`       | Implemented |
| `TYPE`       | Implemented |
| `ERR`        | Implemented |
| `RES`        | Implemented |

This is the most complete JDBCBL implementation among the batch C drivers.
Live integration checks remain environment-gated via `SCRATCHBIRD_PASCAL_URL`.

## Platform Support

| Platform | Status    |
|----------|-----------|
| Linux    | Supported (CI, `fpc` compile) |
| Windows  | Supported (CI, `fpc` compile) |
| macOS    | Untested  |

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire\_protocol\_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)
- [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md)
- [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md)
- [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md)
- [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md)
- [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/julia.md -->

<a id="ch-client-driver-guide-drivers-julia-md"></a>

# ScratchBird Julia Driver — DBInterface / Tables.jl Binding

> **Status: beta\_2 / release\_candidate (draft stub)** — The Julia driver
> source tree contains only a `package_contract.json` at the time of this
> writing. No `README.md`, `Project.toml`, source files, or implementation
> docs are present in the source tree. This page documents what is verifiable
> from the manifest and the package contract; all API examples below are drawn
> exclusively from those sources. The driver is not yet usable for application
> development. This page will be updated when source is committed.

## Purpose

The Julia driver is intended to provide ScratchBird connectivity for Julia
programs using the [DBInterface.jl](https://github.com/JuliaDatabases/DBInterface.jl)
standard and the [Tables.jl](https://github.com/JuliaData/Tables.jl) interface.
These two abstractions are the idiomatic Julia database API: `DBInterface.connect`,
`DBInterface.execute`, `DBInterface.prepare`, and the `Tables.rows` /
`Tables.columns` sinks.

Target audience: Julia data scientists and engineers who need ScratchBird
access from within the Julia ecosystem.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0027-7000-8000-000000000027`     |
| `driver_family`          | `julia`                                    |
| `api_surface_set`        | `dbinterface_tables`                       |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `connection_pool`                          |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_julia_gate`                        |

## Public API Surface (from `package_contract.json`)

The following entry points are declared in the contract. They follow standard
DBInterface.jl conventions:

| Symbol                   | Role                                       |
|--------------------------|--------------------------------------------|
| `ScratchBird.Connection` | Connection type (implements `DBInterface.Connection`) |
| `DBInterface.connect`    | Open a connection                          |
| `DBInterface.execute`    | Execute a statement, return a cursor       |
| `DBInterface.prepare`    | Prepare a statement                        |
| `Tables.rows`            | Iterate cursor results as rows             |
| `Tables.columns`         | Access cursor results as column vectors    |

## Install

Not yet available. When published, the expected form is:

```julia
# Project.toml (anticipated; not yet verified)
[deps]
ScratchBird = "<uuid>"
```

```julia
using Pkg
Pkg.add("ScratchBird")
```

The package name is expected to be `ScratchBird` in the Julia registry
(unconfirmed; no `Project.toml` is present in the source tree).

## Connecting

The DSN form is expected to follow the standard ScratchBird URL convention:

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. See ../connection\_and\_dsn.md for the full key reference.

Anticipated usage based on DBInterface.jl conventions:

```julia
using ScratchBird, DBInterface, Tables

conn = DBInterface.connect(ScratchBird.Connection,
    "scratchbird://user:pass@localhost:3092/mydb")
```

> No source files are present to confirm the actual keyword arguments,
> connection options, or error types. The above is illustrative only.

## Executing Statements and Transactions

Anticipated pattern (DBInterface.jl standard):

```julia
# Query
result = DBInterface.execute(conn, "SELECT id, name FROM users")
for row in Tables.rows(result)
    println(row.id, row.name)
end

# Prepared statement
stmt = DBInterface.prepare(conn, "SELECT * FROM t WHERE id = ?")
result = DBInterface.execute(stmt, [42])
```

Transaction semantics conform to the MGA engine contract (source:
`package_contract.json` — `mga_transaction_finality`):

- sessions are always in a transaction
- `COMMIT` / `ROLLBACK` reopen the next boundary
- no hidden replay of abandoned in-flight transactions
- `mga_transaction_finality` route requirement is declared in the contract

## Conformance (Declared in Contract)

The following conformance areas are declared in `package_contract.json`:

| Area                 | Declared |
|----------------------|----------|
| `connect_auth`       | yes      |
| `prepare_execute_fetch` | yes   |
| `transactions`       | yes      |
| `metadata`           | yes      |
| `type_mapping`       | yes      |
| `error_mapping`      | yes      |
| `reconnect`          | yes      |
| `protocol_negotiation` | yes    |
| `cancellation`       | yes      |

> These are contract declarations, not verified against implemented source.
> See [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md) for the
> `driver_julia_gate` conformance profile once source is available.

## What Is Not Yet Covered

Because the source tree contains only `package_contract.json`, the following
sections cannot be documented from source and are omitted:

- Actual `Project.toml` package name, version, and dependencies
- Concrete connection constructor keyword arguments
- Error type hierarchy and exception names
- Type mapping table (Julia types for each SBsql OID)
- Metadata query wrapper methods
- Pooling configuration parameters
- TLS and authentication option details beyond DSN key names

These sections will be added when source is committed to the driver directory.

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire\_protocol\_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)
- [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md)
- [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md)
- [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md)
- [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md)
- [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/perl.md -->

<a id="ch-client-driver-guide-drivers-perl-md"></a>

# ScratchBird Perl Driver — DBI / DBD Binding

> **Status: beta\_2 / release\_candidate (draft stub)** — The Perl driver
> source tree contains only a `package_contract.json` at the time of this
> writing. No `README.md`, `Makefile.PL`, source files, or implementation docs
> are present in the source tree. This page documents what is verifiable from
> the manifest and the package contract; all API examples below are drawn
> exclusively from those sources. The driver is not yet usable for application
> development. This page will be updated when source is committed.

## Purpose

The Perl driver is intended to provide ScratchBird connectivity via the
standard Perl DBI interface using a `DBD::ScratchBird` driver module. DBI is
the de-facto Perl database abstraction layer; a DBD module implements the
database-specific backend. This driver exposes standard DBI handle methods
including `prepare`, `execute`, `fetchrow_arrayref`, `commit`, and `rollback`,
plus the DBI metadata methods `table_info` and `column_info`.

Target audience: Perl developers using DBI-based applications (web apps,
ETL pipelines, data scripts) who need ScratchBird connectivity.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0028-7000-8000-000000000028`     |
| `driver_family`          | `perl`                                     |
| `api_surface_set`        | `perl_dbi`                                 |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `connection_thread_confined`               |
| `pooling_capability`     | `connection_pool`                          |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_perl_gate`                         |

> **Thread safety:** The Perl driver is `connection_thread_confined`. Each DBI
> connection handle must be used from the thread that created it. Do not share
> connection handles across threads; create a separate handle per thread.

## Public API Surface (from `package_contract.json`)

The following DBI/DBD entry points are declared in the contract:

| Symbol                  | DBI role                                  |
|-------------------------|-------------------------------------------|
| `DBD::ScratchBird`      | Driver module name (for `DBI->connect`)   |
| `DBI->connect`          | Open a database handle (`$dbh`)           |
| `prepare`               | Prepare a statement, return `$sth`        |
| `execute`               | Execute a prepared statement handle       |
| `fetchrow_arrayref`     | Fetch next row as array reference         |
| `commit`                | Commit current transaction                |
| `rollback`              | Roll back current transaction             |
| `table_info`            | DBI metadata: list tables                 |
| `column_info`           | DBI metadata: list columns                |

## Install

Not yet available. When published, the expected CPAN form is:

```bash
# Via cpanm (anticipated; not yet verified)
cpanm DBD::ScratchBird
```

Or from source:

```bash
perl Makefile.PL
make
make test
make install
```

> No `Makefile.PL` or `META.json` is present in the source tree, so the
> package name and CPAN namespace are unconfirmed.

## Connecting

The DSN form follows standard DBI conventions with the ScratchBird URL embedded
or as colon-separated DBI DSN attributes:

```perl
# URL form (anticipated)
my $dbh = DBI->connect(
    "dbi:ScratchBird:database=mydb;host=localhost;port=3092",
    "user",
    "password",
    { AutoCommit => 0, RaiseError => 1 }
);
```

The `package_contract.json` declares `database`, `host`, `port`, `user`, and
`auth_method` as DSN keys. See ../connection\_and\_dsn.md for the full key
reference.

> No source files are present to confirm the exact DBI DSN attribute names or
> which `$dbh` attributes are accepted. The above is illustrative only.

## Executing Statements and Transactions

Anticipated DBI pattern:

```perl
# Prepare and execute
my $sth = $dbh->prepare("SELECT id, name FROM users WHERE active = ?");
$sth->execute(1);
while (my $row = $sth->fetchrow_arrayref) {
    print "$row->[0]  $row->[1]\n";
}

# Transaction
$dbh->{AutoCommit} = 0;
eval {
    $dbh->do("UPDATE counters SET n = n + 1 WHERE id = 1");
    $dbh->commit;
};
if ($@) {
    $dbh->rollback;
    die $@;
}
```

Transaction semantics conform to the MGA engine contract (source:
`package_contract.json` — `mga_transaction_finality`):

- sessions are always in a transaction
- `commit` / `rollback` reopen the next boundary
- no hidden replay of abandoned in-flight transactions

## Metadata

DBI metadata methods declared in the contract:

```perl
# List tables
my $sth = $dbh->table_info(undef, "public", "%", "TABLE");

# List columns
my $sth = $dbh->column_info(undef, "public", "users", "%");
```

These map to `sys_information_recursive` profile queries on the server.
Full reference: [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md).

## Conformance (Declared in Contract)

The following conformance areas are declared in `package_contract.json`:

| Area                 | Declared |
|----------------------|----------|
| `connect_auth`       | yes      |
| `prepare_execute_fetch` | yes   |
| `transactions`       | yes      |
| `metadata`           | yes      |
| `type_mapping`       | yes      |
| `error_mapping`      | yes      |
| `reconnect`          | yes      |
| `protocol_negotiation` | yes    |
| `cancellation`       | yes      |

> These are contract declarations, not verified against implemented source.
> See [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md) for the
> `driver_perl_gate` conformance profile once source is available.

## What Is Not Yet Covered

Because the source tree contains only `package_contract.json`, the following
sections cannot be documented from source and are omitted:

- Actual `Makefile.PL` / CPAN package name and dependencies
- Concrete DBI DSN attribute names and accepted handle attributes
- Error handling (`$DBI::errstr`, `$DBI::state`, exception behaviour)
- Type mapping table (Perl scalar/reference types for each SBsql OID)
- Additional metadata methods beyond `table_info` and `column_info`
- Pooling configuration (e.g. `DBI::Pool` integration)
- TLS and authentication option details beyond DSN key names
- Thread-confinement enforcement details

These sections will be added when source is committed to the driver directory.

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](#ch-client-driver-guide-connection-and-dsn-md)
- [../authentication.md](#ch-client-driver-guide-authentication-md)
- [../wire\_protocol\_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md)
- [../type\_mapping.md](#ch-client-driver-guide-type-mapping-md)
- [../metadata\_sys\_information.md](#ch-client-driver-guide-metadata-sys-information-md)
- [../diagnostics\_and\_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md)
- [../pooling\_and\_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md)
- [../conformance\_baseline.md](#ch-client-driver-guide-conformance-baseline-md)




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/adbc.md -->

<a id="ch-client-driver-guide-drivers-adbc-md"></a>

# ScratchBird ADBC Driver — Arrow Database Connectivity (ADBC C API)

The ScratchBird ADBC driver gives Arrow-native applications access to a
ScratchBird Convergent Data Engine (CDE) through the Arrow Database
Connectivity (ADBC) C API specification. Query results are returned as
Arrow `RecordBatch` streams rather than row-by-row result sets, which
makes the driver the natural choice for high-throughput analytical
pipelines, columnar compute engines, and tools in the Apache Arrow
ecosystem.

The driver speaks the ScratchBird Native Wire Protocol (SBWP v1.1)
directly and does not sit on top of ODBC, JDBC, or any other
intermediate layer.

**Release status: beta_2 (release_candidate gate)**

> **Draft stub.** The `project/drivers/driver/adbc/` source tree
> contains only `package_contract.json` at the time of this writing. No
> README, CMakeLists, header files, or implementation source are present.
> All sections below reflect only what is verifiable from that contract
> and the shared DriverPackageManifest. Sections that would require
> implementation source have been omitted rather than invented.

---

## Manifest metadata

| Field | Value |
|---|---|
| `component_id` | `driver:adbc` |
| `driver_package_uuid` | `019e12a0-0025-7000-8000-000000000025` |
| `driver_family` | `adbc` |
| `api_surface_set` | `adbc_c_api` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `arrow_recordbatch` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_adbc_gate` |

---

## Installation

> Source tree: `project/drivers/driver/adbc/`
>
> No build manifest (CMakeLists.txt, Makefile, or equivalent) is present
> in the current source snapshot. The artifact name, build instructions,
> and system requirements cannot be stated without that file. This section
> will be completed when the build manifest is committed.

---

## API objects (ADBC C API)

The `package_contract.json` declares the following ADBC C API surface
objects for this driver:

| Symbol | Role |
|---|---|
| `AdbcDatabase` | Holds driver-level configuration (host, port, database, credentials). |
| `AdbcConnection` | Represents an authenticated session; wraps one SBWP v1.1 transport connection. |
| `AdbcStatement` | Owns a prepared or ad-hoc query and its execution state. |
| `ArrowArrayStream` | The result-set carrier; rows are consumed as Arrow `RecordBatch` chunks. |
| `catalog_objects` | Exposes catalog/schema/table enumeration through the ADBC metadata API. |
| `bulk_ingest` | Provides Arrow-native bulk data ingest (Arrow `RecordBatch` in, table append/replace out). |

These symbols follow the ADBC C API specification. Refer to the upstream
ADBC specification for the full function signatures (`AdbcDatabaseNew`,
`AdbcConnectionNew`, `AdbcStatementNew`, etc.).

---

## Connecting

> Connection form and minimal example cannot be stated without
> implementation source confirming the exact `AdbcDatabase` key names
> used by this driver. The manifest records the DSN key set as
> `database`, `host`, `port`, `user`, `auth_method`. Expected connection
> form (subject to verification once source is committed):
>
> ```c
> // Illustrative only — key names unverified without implementation source
> AdbcDatabase db = {0};
> AdbcDatabaseNew(&db, &error);
> AdbcDatabaseSetOption(&db, "host",     "localhost",    &error);
> AdbcDatabaseSetOption(&db, "port",     "3092",         &error);
> AdbcDatabaseSetOption(&db, "database", "mydb",         &error);
> AdbcDatabaseSetOption(&db, "user",     "myuser",       &error);
> AdbcDatabaseSetOption(&db, "password", "mypass",       &error);
> AdbcDatabaseInit(&db, &error);
> ```
>
> This example must be verified against the driver header / implementation
> before publishing.

Both `direct_listener` and `manager_proxy` ingress modes are listed in
the manifest. TLS floor: `scratchbird_tls_1_3_floor` (TLS 1.3 minimum).

---

## Results — Arrow RecordBatch

The `type_mapping_profile` is `arrow_recordbatch`. ScratchBird SBsql
types are projected onto Arrow logical types when the driver returns
results. The concrete Arrow type used for each SBsql type is defined in
the shared type mapping reference.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full SBsql-to-Arrow
column type table.

---

## Metadata

Schema, table, and column metadata are available through the ADBC
`catalog_objects` surface, backed by `sys.information.*` views on the
server (metadata profile: `sys_information_recursive`).

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) for
the view hierarchy and query patterns.

---

## Errors and diagnostics

The diagnostic mapping profile is `native_sqlstate`. Errors are surfaced
through the ADBC `AdbcError` struct; the `sqlstate` field carries a
five-character SQLSTATE code following the ScratchBird native SQLSTATE
classification.

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) for
the SQLSTATE class map and retry guidance.

---

## Pooling and concurrency

The manifest records `thread_safe` thread-safety and `connection_pool`
pooling capability. Concrete pool configuration parameters cannot be
stated without implementation source.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) for
ScratchBird pooling concepts.

---

## Conformance

Conformance profile reference: `driver_adbc_gate`.

The manifest lists the following conformance areas:
`connect_auth`, `prepare_execute_fetch`, `transactions`, `metadata`,
`type_mapping`, `error_mapping`, `reconnect`, `protocol_negotiation`,
`cancellation`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) for the
baseline conformance definition.

---

## Route requirements

The driver declares the following route requirements in its package
contract:

| Requirement | Purpose |
|---|---|
| `sbwp_v1_1` | Wire protocol version floor |
| `scratchbird_tls_1_3_floor` | Minimum TLS version |
| `engine_authentication_authority` | Engine handles auth |
| `engine_authorization_authority` | Engine handles authz |
| `mga_transaction_finality` | MGA-based transaction model |
| `sys_information_metadata` | Metadata via `sys.information.*` |
| `uuid_identity` | UUID-based catalog identity |
| `no_hidden_replay` | Explicit retry semantics only |

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1 wire protocol
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — SBsql-to-Arrow type mapping
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE reference
- [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) — Pooling
- [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) — Conformance baseline




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/flightsql.md -->

<a id="ch-client-driver-guide-drivers-flightsql-md"></a>

# ScratchBird FlightSQL Driver — Arrow Flight SQL over gRPC

The ScratchBird FlightSQL driver gives applications access to a
ScratchBird Convergent Data Engine (CDE) through the Arrow Flight SQL
protocol over gRPC. Queries are dispatched as Flight SQL RPC calls;
result data is streamed back as Arrow `RecordBatch` payloads carried in
Flight `DoGet` streams. The driver is the standard integration path for
clients that consume the Apache Arrow Flight SQL specification, including
ADBC Flight SQL backends, Substrait-based query planners, and gRPC-native
analytics tools.

The underlying wire protocol remains SBWP v1.1 (ScratchBird Native Wire
Protocol). gRPC Status codes are mapped to SQLSTATE using the
`grpc_status_sqlstate` diagnostic profile.

**Release status: beta_2 (release_candidate gate)**

> **Draft stub.** The `project/drivers/driver/flightsql/` source tree
> contains only `package_contract.json` at the time of this writing. No
> README, CMakeLists, proto files, or implementation source are present.
> All sections below reflect only what is verifiable from that contract
> and the shared DriverPackageManifest. Sections that would require
> implementation source have been omitted rather than invented.

---

## Manifest metadata

| Field | Value |
|---|---|
| `component_id` | `driver:flightsql` |
| `driver_package_uuid` | `019e12a0-0026-7000-8000-000000000026` |
| `driver_family` | `flight_sql` |
| `api_surface_set` | `flight_sql_grpc` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `flight_endpoint`, `database`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `arrow_recordbatch` |
| `diagnostic_mapping_profile` | `grpc_status_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `stream_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_flightsql_gate` |

---

## Installation

> Source tree: `project/drivers/driver/flightsql/`
>
> No build manifest (CMakeLists.txt, pom.xml, go.mod, or equivalent) is
> present in the current source snapshot. The artifact name, build
> instructions, and system requirements cannot be stated without that
> file. This section will be completed when the build manifest is
> committed.

---

## Flight SQL RPC surface

The `package_contract.json` declares the following Flight SQL gRPC
operations for this driver:

| RPC | Role |
|---|---|
| `GetFlightInfo` | Plans a query and returns `FlightInfo` with one or more `FlightEndpoint` locations. |
| `DoGet` | Streams result data as Arrow `RecordBatch` payloads for a `Ticket` from `GetFlightInfo`. |
| `GetTables` | Returns table metadata; corresponds to `CommandGetTables` in the Flight SQL spec. |
| `GetSqlInfo` | Returns server capability flags; corresponds to `CommandGetSqlInfo`. |
| `BeginTransaction` | Opens an MGA-engine transaction on the Flight SQL session. |
| `EndTransaction` | Commits or rolls back the active transaction. |
| `CreatePreparedStatement` | Prepares a parameterised statement; returns a `PreparedStatementHandle`. |
| `Execute` | Executes a prepared statement with bound parameters. |

These operations follow the Arrow Flight SQL specification. Refer to the
upstream Arrow Flight SQL specification for full message and RPC
definitions.

---

## Connecting

The manifest records the DSN key `flight_endpoint` in addition to the
standard `database`, `user`, and `auth_method` keys. The `flight_endpoint`
key carries the gRPC endpoint address (host and port) of the ScratchBird
Flight SQL listener.

> Concrete connection examples (channel options, metadata headers, TLS
> certificate configuration) cannot be stated without implementation
> source confirming the exact client constructor used by this driver.
> This section will be completed when implementation source is committed.

Both `direct_listener` and `manager_proxy` ingress modes are listed in
the manifest. TLS floor: `scratchbird_tls_1_3_floor` (TLS 1.3 minimum).

---

## Results — Arrow RecordBatch streams

The `type_mapping_profile` is `arrow_recordbatch`. ScratchBird SBsql
types are projected onto Arrow logical types when the driver streams
`RecordBatch` payloads back from `DoGet`. The result of `GetFlightInfo`
contains `FlightEndpoint` references that the client then fetches with
`DoGet`.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full SBsql-to-Arrow
column type table.

---

## Metadata

Schema, table, and column metadata are available through the Flight SQL
`GetTables` and related `CommandGetXxx` operations, backed by
`sys.information.*` views on the server (metadata profile:
`sys_information_recursive`).

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) for
the view hierarchy and query patterns.

---

## Errors and diagnostics

The diagnostic mapping profile is `grpc_status_sqlstate`. gRPC `Status`
codes returned by the server are mapped to SQLSTATE codes following the
ScratchBird diagnostic classification. The gRPC `Status.details` field
carries additional ScratchBird diagnostic fields where available.

| gRPC Status | SQLSTATE class |
|---|---|
| `OK` | — (no error) |
| `INVALID_ARGUMENT` | `22xxx` (data exception) |
| `NOT_FOUND` | `42xxx` (syntax error or access rule) |
| `ALREADY_EXISTS` | `23xxx` (integrity constraint) |
| `PERMISSION_DENIED` | `28xxx` (invalid authorization) |
| `UNAUTHENTICATED` | `28000` |
| `UNAVAILABLE` | `08xxx` (connection exception) |
| `ABORTED` | `40xxx` (transaction rollback) |
| Other | Mapped per `grpc_status_sqlstate` profile |

> The table above reflects the `grpc_status_sqlstate` profile name from
> the manifest. The detailed per-code mapping is defined in the shared
> diagnostics chapter rather than driver source (which is absent).

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) for
the full SQLSTATE classification and retry guidance.

---

## Pooling and concurrency

The manifest records `thread_safe` thread-safety and `stream_pool`
pooling capability. A stream pool manages the lifecycle of active `DoGet`
streams, allowing concurrent result consumption across multiple callers
without opening redundant gRPC channels.

Concrete pool configuration parameters cannot be stated without
implementation source.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) for
ScratchBird pooling concepts and stream pool semantics.

---

## Conformance

Conformance profile reference: `driver_flightsql_gate`.

The manifest lists the following conformance areas:
`connect_auth`, `prepare_execute_fetch`, `transactions`, `metadata`,
`type_mapping`, `error_mapping`, `reconnect`, `protocol_negotiation`,
`cancellation`.

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) for the
baseline conformance definition.

---

## Route requirements

The driver declares the following route requirements in its package
contract:

| Requirement | Purpose |
|---|---|
| `sbwp_v1_1` | Wire protocol version floor |
| `scratchbird_tls_1_3_floor` | Minimum TLS version |
| `engine_authentication_authority` | Engine handles auth |
| `engine_authorization_authority` | Engine handles authz |
| `mga_transaction_finality` | MGA-based transaction model |
| `sys_information_metadata` | Metadata via `sys.information.*` |
| `uuid_identity` | UUID-based catalog identity |
| `no_hidden_replay` | Explicit retry semantics only |

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1 wire protocol
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — SBsql-to-Arrow type mapping
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE / gRPC status reference
- [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) — Stream pool
- [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) — Conformance baseline




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/drivers/r.md -->

<a id="ch-client-driver-guide-drivers-r-md"></a>

# ScratchBird R Driver — R DBI Interface

The ScratchBird R driver gives R applications access to a ScratchBird
Convergent Data Engine (CDE) through the standard R DBI interface (the
`DBI` package). It implements the `DBIDriver`, `DBIConnection`, and
`DBIResult` virtual classes and speaks the ScratchBird Native Wire
Protocol (SBWP v1.1) directly — no ODBC or JDBC layer is involved.

The package name is `scratchbird` (R package, CRAN-style). It carries its
own native TLS transport (`src/tls_transport.c`) and a pure-R SBWP
protocol implementation (`R/protocol.R`, `R/client.R`).

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `component_id` | `driver:r` |
| `driver_package_uuid` | `019e12a0-0013-7000-8000-000000000013` |
| `driver_family` | `r` |
| `api_surface_set` | `dbi` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `session_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_r_gate` |

---

## Installation

The package is distributed as `scratchbird` (R package, version `0.1.0`).
Build and install from the source tree at `project/drivers/driver/r/`:

```r
# From within the project/drivers/driver/r/ directory:
install.packages(".", repos = NULL, type = "source")
```

Or, for development with `pkgload`:

```bash
Rscript -e "pkgload::load_all(quiet=TRUE)"
```

**System requirements:** OpenSSL (`libssl`, `libcrypto`) must be present.
The native TLS transport (`src/tls_transport.c`) is compiled on install
via `src/Makevars` (Linux/macOS) or `src/Makevars.win` (Windows).

**R package dependencies:** `DBI`, `openssl`, `jsonlite`, `methods`.
Test dependency: `testthat`.

**Platform support:**

| Platform | Status |
|---|---|
| Linux | Supported — CI build and test coverage |
| Windows | Supported — CI build and test coverage |
| macOS | Untested — not currently covered in CI |

License: MPL-2.0.

---

## Connecting

The driver entry point is the `Scratchbird()` constructor, which returns a
`ScratchbirdDriver` object. Pass it to `DBI::dbConnect()` along with a
DSN string or keyword arguments. The lower-level `sb_connect()` function
is also exported for use outside the DBI calling convention.

### DBI entry point

```r
library(DBI)
library(scratchbird)

# URI DSN
con <- dbConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb")

# Key-value DSN (space-separated or semicolon-separated)
con <- dbConnect(Scratchbird(), "host=localhost port=3092 dbname=mydb user=myuser password=mypass")

# Test reachability without holding a connection
ok <- dbCanConnect(Scratchbird(), "scratchbird://user:pass@localhost:3092/mydb")
```

The `dbConnect` method delegates to `sb_connect(dsn, ...)`.

### DSN forms

**URI:**

```
scratchbird://user:password@host:3092/database?sslmode=require
```

**Key-value (space- or semicolon-separated):**

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

**Manager-proxy URI:**

```
scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token
```

### Selected DSN keys

| Key | Aliases | Default | Notes |
|---|---|---|---|
| `host` | `server`, `datasource` | `localhost` | |
| `port` | — | `3092` | |
| `database` | `dbname`, `initial catalog` | (required) | |
| `user` | `username`, `uid` | (required) | |
| `password` | `pwd` | | |
| `sslmode` | `ssl mode` | `require` | `require` enforces TLS 1.3 floor |
| `front_door_mode` | `connection_mode` | `direct` | `direct` or `manager_proxy` |
| `manager_auth_token` | `mcp_auth_token` | | Required for `manager_proxy` mode |
| `schema` | `search_path` | | Optional initial schema |
| `fetch_size` | `defaultrowfetchsize` | `0` (all) | Rows per incremental fetch |
| `binary_transfer` | `binarytransfer` | `TRUE` | Binary wire encoding |

The full key list is implemented in `R/config.R` (`apply_param`).

### Low-level connect

```r
# sb_connect returns a raw client environment (not a DBIConnection)
client <- sb_connect("scratchbird://user:pass@localhost:3092/mydb")
sb_disconnect(client)
```

### Authentication probe

```r
# Inspect available auth methods before connecting
surface <- sb_probe_auth_surface("scratchbird://user@localhost:3092/mydb")
ctx    <- sb_get_resolved_auth_context(client)
```

Natively supported auth methods: `PASSWORD`, `SCRAM_SHA_256`,
`SCRAM_SHA_512`, `TOKEN`, `manager_proxy` token bootstrap. Methods
`MD5`, `PEER`, and `REATTACH` are negotiated but fail-closed when locally
unsupported.

---

## Executing queries and fetching results

The R driver implements the standard DBI execution lifecycle.

### DBI methods

```r
# Simple one-shot query — returns a data.frame
df <- dbGetQuery(con, "SELECT id, name FROM orders WHERE status = $1", list("open"))

# Send / fetch / clear lifecycle
res  <- dbSendQuery(con, "SELECT id, name FROM orders")
df   <- dbFetch(res, n = 100)   # fetch up to 100 rows; n = -1 fetches all
info <- dbColumnInfo(res)        # column metadata data.frame (see below)
dbClearResult(res)

# DML / DDL
rows <- dbExecute(con, "UPDATE orders SET status = $1 WHERE id = $2", list("closed", 42L))
```

### Column metadata from `dbColumnInfo`

`dbColumnInfo(res)` returns a `data.frame` with one row per column:

| Column | Type | Description |
|---|---|---|
| `name` | `character` | Column name |
| `type_oid` | `integer` | ScratchBird OID |
| `type_size` | `integer` | Fixed size, or -1 for variable |
| `type_modifier` | `integer` | Precision/length modifier |
| `table_oid` | `integer` | Source table OID |
| `column_index` | `integer` | 1-based column index |
| `format` | `integer` | Wire format (`0` = text, `1` = binary) |
| `nullable` | `logical` | `TRUE` if column is nullable |

### Low-level query functions

| Symbol | Source | Notes |
|---|---|---|
| `sb_query(client, sql, ...)` | `R/client.R` | Send query, fetch all rows, return data.frame |
| `sb_get_query(client, sql, ...)` | `R/client.R` | Alias used by DBI layer |
| `sb_send_query(client, sql, ...)` | `R/client.R` | Non-blocking send; returns result env |
| `sb_fetch(result, n)` | `R/client.R` | Incremental fetch |
| `sb_clear_result(result)` | `R/client.R` | Drain and free result |
| `sb_cancel(client)` | `R/client.R` | Issue cancellation request |

---

## Transactions

ScratchBird sessions are always-in-a-transaction: `COMMIT` or `ROLLBACK`
immediately reopens the next transaction boundary. The driver tracks this
through native `READY`, `TXN_STATUS`, and `current_txn_id` protocol
fields.

### DBI transaction methods

```r
dbBegin(con)          # calls sb_begin(), sets autocommit FALSE
dbCommit(con)         # calls sb_commit(), sets autocommit TRUE
dbRollback(con)       # calls sb_rollback(), sets autocommit TRUE
```

### Isolation level mapping

The driver exposes the canonical mapping via `sb_canonical_isolation_label()`:

| SQL name | Canonical ScratchBird level |
|---|---|
| `READ UNCOMMITTED` | Legacy compatibility alias (not a distinct level) |
| `READ COMMITTED` | `READ COMMITTED` |
| `REPEATABLE READ` | `SNAPSHOT` |
| `SERIALIZABLE` | `SNAPSHOT TABLE STABILITY` |

`READ COMMITTED` sub-mode is selectable via
`sb_canonical_read_committed_mode_label()`, including
`READ COMMITTED READ CONSISTENCY`.

### `sb_begin` advanced options

`sb_begin(client, ...)` accepts named arguments that map directly to the
MGA engine begin payload:

| Argument | Notes |
|---|---|
| `isolation_level` | See canonical mapping above |
| `access_mode` | `READ WRITE` or `READ ONLY` |
| `deferrable` | Logical |
| `wait` | Logical (lock wait vs. no-wait) |
| `timeout_ms` | Lock wait timeout |
| `autocommit_mode` | |
| `conflict_action` | |
| `read_committed_mode` | Sub-mode selector for `READ COMMITTED` |

### Savepoints

```r
# Low-level savepoint operations
sb_savepoint(client, "sp1")
sb_release_savepoint(client, "sp1")
sb_rollback_to_savepoint(client, "sp1")
```

### Prepared / two-phase transactions

```r
sb_supports_prepared_transactions(client)      # TRUE/FALSE
sb_prepare_transaction(client, "xid_1")
sb_commit_prepared(client, "xid_1")
sb_rollback_prepared(client, "xid_1")
```

### Dormant sessions

Dormant session reattach is probed but intentionally fails closed:

```r
sb_supports_dormant_reattach(client)           # always FALSE currently
sb_detach_to_dormant(client)                   # raises SQLSTATE 0A000
sb_reattach_dormant(client, id, token)         # raises SQLSTATE 0A000
```

### Retry guidance

`sb_retry_scope_for_sqlstate(sqlstate)` returns the retry boundary:

| SQLSTATE | Retry scope |
|---|---|
| `40001`, `40P01` | Retry fresh statement only |
| `08xxx` | Reconnect or reopen only |
| Any other | No automatic replay |

---

## Type mapping

The type mapping profile is `sbsql_core`. The driver encodes parameters
in binary (`SB_FORMAT_BINARY = 1L`) by default. The following OID
constants are defined in `R/types.R` and govern encode/decode dispatch:

| R type | ScratchBird OID constant | Notes |
|---|---|---|
| `logical` (scalar) | `SB_OID_BOOL` (16) | |
| `integer` (scalar) | `SB_OID_INT4` (23) | |
| `numeric` (integer-valued, in INT4 range) | `SB_OID_INT4` (23) | Auto-promoted |
| `numeric` (double) | `SB_OID_FLOAT8` (701) | |
| `character` (scalar) | `SB_OID_TEXT` (25) | UUID strings detected and sent as `SB_OID_UUID` |
| `character` (UUID pattern) | `SB_OID_UUID` (2950) | Regex-matched |
| `Date` | `SB_OID_DATE` (1082) | |
| `POSIXct` / `POSIXt` | `SB_OID_TIMESTAMPTZ` (1184) | |
| `raw` | `SB_OID_BYTEA` (17) | |
| `sb_jsonb` | `SB_OID_JSONB` (3802) | Construct with `sb_jsonb(value = ...)` |
| `sb_geometry` | `SB_OID_POINT` (600) | Construct with `sb_geometry(wkb, srid)` |
| `sb_range` | OID from `encode_range()` | Construct with `sb_range(...)` |
| `sb_composite` | `SB_OID_RECORD` (2249) | Construct with `sb_composite(fields)` |
| `numeric` vector (length > 1) | `SB_OID_SB_VECTOR` (16386) | ScratchBird vector type |
| `list` / atomic vector (length > 1) | Array literal (OID 0) | |

**Decoding** (`decode_value` / `decode_binary_value` in `R/types.R`):

| ScratchBird OID | R result type |
|---|---|
| `SB_OID_BOOL` | `logical` |
| `SB_OID_INT2`, `SB_OID_INT4` | `integer` |
| `SB_OID_INT8` | `numeric` (64-bit via `read_i64_numeric`) |
| `SB_OID_FLOAT4`, `SB_OID_FLOAT8` | `numeric` |
| `SB_OID_NUMERIC` | `numeric` (or `character` if non-numeric) |
| `SB_OID_MONEY` | `numeric` (cents / 100) |
| `SB_OID_TEXT`, `SB_OID_VARCHAR`, `SB_OID_BPCHAR`, `SB_OID_CHAR` | `character` |
| `SB_OID_JSON`, `SB_OID_XML` | `character` |
| `SB_OID_JSONB` | `sb_jsonb` S3 object |
| `SB_OID_BYTEA` | `raw` |
| `SB_OID_UUID` | `character` (formatted UUID string) |
| `SB_OID_DATE` | `Date` (days from 2000-01-01) |
| `SB_OID_TIME` | `POSIXct` (seconds from epoch UTC) |
| `SB_OID_TIMESTAMP`, `SB_OID_TIMESTAMPTZ` | `POSIXct` (microseconds from 2000-01-01 UTC) |
| `SB_OID_INTERVAL` | Named `list` (`micros`, `days`, `months`) |
| Range OIDs (`INT4RANGE`, `INT8RANGE`, `NUMRANGE`, `TSRANGE`, `TSTZRANGE`, `DATERANGE`) | `sb_range` S3 object |
| `SB_OID_RECORD` | `sb_composite` S3 object |
| `SB_OID_TSVECTOR`, `SB_OID_TSQUERY`, `SB_OID_INET`, `SB_OID_CIDR`, `SB_OID_MACADDR`, `SB_OID_MACADDR8` | `character` |

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full SBsql type
reference.

---

## Metadata

Schema, table, and column metadata are available through DBI metadata
methods and dedicated helper functions:

### DBI metadata methods

```r
dbListTables(con)                          # schema-qualified table names
dbExistsTable(con, "myschema.orders")      # TRUE/FALSE
dbListFields(con, "orders")                # column name vector
```

### Low-level metadata helpers

| Symbol | Source | Returns |
|---|---|---|
| `sb_metadata_schemas_query()` | `R/metadata.R:3` | SQL string for schema enumeration |
| `sb_metadata_tables_query()` | `R/metadata.R:7` | SQL string for table enumeration |
| `sb_metadata_columns_query()` | `R/metadata.R:11` | SQL string for column enumeration |
| `sb_metadata_indexes_query()` | `R/metadata.R:18` | SQL string for index enumeration |
| `sb_metadata_index_columns_query()` | `R/metadata.R:22` | SQL string for index column detail |
| `sb_metadata_constraints_query()` | `R/metadata.R:26` | SQL string for constraint enumeration |
| `sb_metadata_procedures_query()` | `R/metadata.R:30` | SQL string for procedure enumeration |
| `sb_metadata_functions_query()` | `R/metadata.R:34` | SQL string for function enumeration |

### Recursive schema navigation

```r
paths <- sb_metadata_schema_paths_for_navigation(schemas_df)
tree  <- sb_metadata_build_schema_tree(schemas_df)
rows  <- sb_metadata_build_schema_tree_rows(database_name, schemas_df)
```

`sb_metadata_build_schema_tree_rows` returns a flattened depth-first row
sequence with a root `database` row followed by schema rows — suitable
for tree-view navigation in IDEs and tools.

**Conformance note:** The META conformance area is listed as `Partial`
in the S3 implementation record. Richer privilege, key, and DDL-editor
metadata coverage is pending.

See [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) for
the `sys.information.*` view hierarchy.

---

## Errors and diagnostics

The diagnostic mapping profile is `native_sqlstate`. Errors raised by the
driver are R conditions of class `c("<sqlstate_class>", "error")` where
`<sqlstate_class>` is derived from the five-character SQLSTATE code
returned by the server.

Key error-path symbols in `R/client.R`:

| Symbol | Source | Role |
|---|---|---|
| `sb_sqlstate_error_class(sqlstate)` | `R/client.R:565` | Maps SQLSTATE to R condition class |
| `sb_raise_query_error(...)` | `R/client.R:615` | Constructs and signals the condition |
| `parse_error_message(...)` | `R/protocol.R:590` | Parses the server error frame |

The condition carries `sqlstate`, `detail`, and `hint` fields.

**Retry guidance:**

```r
scope <- sb_retry_scope_for_sqlstate("40001")
# Returns "statement" — retry fresh statement only
```

See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) for
the full SQLSTATE class map.

---

## Pooling and concurrency

The manifest records `connection_thread_confined` thread-safety and
`session_pool` pooling capability. Each `ScratchbirdConnection` object
confines its transport and session state to the creating thread. Do not
share a `ScratchbirdConnection` across R threads or `parallel` workers;
create a separate connection per worker instead.

The `session_pool` capability means connections can be held in an
application-managed pool and reused across requests via `dbConnect` /
`dbDisconnect` lifecycle management.

See [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) for
ScratchBird pooling and session concurrency concepts.

---

## Reconnection and resilience

The driver follows the MGA recovery contract:

- `sb_prepare_connection(client)` resets abandoned transaction and
  prepared-statement state before re-entering the startup/auth sequence.
- Reconnect repairs transport and session state only; it never
  resurrects in-flight transactions or replays lost statements.
- Transaction recovery means reset, rollback, reopen, or retry against
  engine truth.

`sb_is_valid(client)` (exposed as `dbIsValid(conn)`) tests whether the
underlying transport is still live.

---

## Running the test suite

Local deterministic tests (no live server required):

```bash
Rscript -e "pkgload::load_all(quiet=TRUE); testthat::test_dir('tests/testthat')"
```

Live integration tests are environment-gated:

| Environment variable | Purpose |
|---|---|
| `SCRATCHBIRD_R_URL` | Direct-connect integration coverage |
| `SCRATCHBIRD_R_MANAGER_URL` | Manager-proxy connect/query coverage |
| `SCRATCHBIRD_R_CANCEL_SQL` | Cancel/drain lifecycle coverage |

---

## Conformance

Conformance profile reference: `driver_r_gate`.

Implementation status per baseline requirement mapping:

| Area | Status |
|---|---|
| CONN (connection/auth) | Partial — offline tests pass; live coverage is environment-gated |
| TXN (transactions) | Partial — DBI lifecycle implemented; live server proof pending |
| EXEC (execution) | Implemented |
| META (metadata) | Partial — recursive schema shaping implemented; richer metadata families pending |
| TYPE (type mapping) | Implemented |
| ERR (error/diagnostics) | Implemented |
| RES (resource cleanup) | Implemented |

See [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) for the
baseline conformance definition.

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Auth methods
- [../wire_protocol_sbwp.md](#ch-client-driver-guide-wire-protocol-sbwp-md) — SBWP v1.1 wire protocol
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — SBsql type mapping
- [../metadata_sys_information.md](#ch-client-driver-guide-metadata-sys-information-md) — sys.information metadata
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE reference
- [../pooling_and_concurrency.md](#ch-client-driver-guide-pooling-and-concurrency-md) — Pooling and concurrency
- [../conformance_baseline.md](#ch-client-driver-guide-conformance-baseline-md) — Conformance baseline




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/tableau.md -->

<a id="ch-client-driver-guide-adaptors-tableau-md"></a>

# Tableau Adaptor

> **Draft stub — beta_2 / release_candidate.**
> This page describes the Tableau Connector adaptor for ScratchBird.
> Source material is limited to `package_contract.json`; no connector bundle
> file or LookML/TACO source is present in the current tree.
> All claims below are sourced from that file and the
> `DriverPackageManifest.csv` row; nothing is inferred or invented.

## Purpose

The ScratchBird Tableau adaptor integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Tableau Desktop
and Tableau Server.  It presents ScratchBird as a named connector inside
Tableau's connector-plugin system and surfaces four query surfaces to Tableau:
live query, extract refresh, metadata enumeration, and an initial-SQL hook.

Unlike a raw JDBC or ODBC driver loaded through Tableau's generic JDBC/ODBC
path, this adaptor ships as a dedicated Tableau connector plugin (a
`tableau_connector_bundle` artifact).  The adaptor itself does not own a
transport layer: it shapes Tableau connection-dialog, metadata-enumeration,
live-query, and extract-refresh requests and relies on the ScratchBird engine
to enforce authentication, authorization, MGA transaction finality, and schema
identity.  The host runtime is the Tableau connector loader.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-tableau` |
| `driver_package_uuid` | `019e12a0-0034-7000-8000-000000000034` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `tableau_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `explicit_session` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_tableau_gate` |
| Delegates to | none (no underlying driver; host runtime is the Tableau connector) |
| Package type | `tableau_connector_bundle` |
| License | MPL-2.0 |

**Delegation posture:** `explicit_session`.  The adaptor shapes Tableau
requests but does not wrap an underlying ScratchBird driver at the protocol
level.  Authentication authority, MGA transaction finality, UUID identity, and
authorization are all enforced by the ScratchBird engine, not by the adaptor.

---

## API Surfaces

The connector plugin exposes the following Tableau-side surfaces (from
`package_contract.json` `api_surface`):

| Surface | Description |
|---|---|
| `connector_plugin` | The Tableau connector plugin registration |
| `connection_dialog` | Custom connection-dialog fields inside Tableau |
| `metadata_enumeration` | Schema and object discovery shown in Tableau's data source browser |
| `initial_sql` | Initial-SQL hook executed when a connection opens |
| `live_query` | Live-query mode (queries forwarded to ScratchBird at query time) |
| `extract_refresh` | Extract refresh mode (Tableau-side data extract) |

---

## Installation

> **Source note:** The build artifact (`tableau_connector_bundle`) is produced
> by a Tableau packaging gate (`tableau packaging gate`).  The output file
> name is not present in the current source tree (only `package_contract.json`
> exists).  Install steps below are drawn from the package contract; the
> exact `.taco` filename will be specified at release time.

1. Obtain the signed connector bundle (`.taco` file) from the ScratchBird
   release.  SBOM and signing are both gated on the release build; unsigned
   bundles are not supported in production Tableau environments.
2. Place the `.taco` file in the Tableau connector directory:
   - **Tableau Desktop (macOS):** `~/Documents/My Tableau Repository/Connectors/`
   - **Tableau Desktop (Windows):** `Documents\My Tableau Repository\Connectors\`
   - **Tableau Server:** the connectors directory configured for the site, or
     distributed via Tableau Server Manager.
3. Restart Tableau.
4. ScratchBird will appear in the **Connect** panel under **To a Server**.

---

## Configuring a Connection

When creating a new ScratchBird data source in Tableau, fill in the
connection-dialog fields.  These correspond to the `dsn_key_set` declared in
the manifest:

| Field | Manifest key | Notes |
|---|---|---|
| Server | `host` | Hostname or IP address of the ScratchBird server |
| Port | `port` | Default: 3092 |
| Database | `database` | Database name |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

TLS is always required: the `scratchbird_tls_1_3_floor` TLS profile enforces
TLS 1.3 as the minimum.  See [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md).

Authentication is engine-owned.  The adaptor does not cache, replay, or proxy
credentials.  See [../authentication.md](#ch-client-driver-guide-authentication-md).

---

## Type Mapping

This adaptor uses the `tableau_mapping` type-mapping profile.  Tableau data
types are mapped from ScratchBird wire types according to that profile.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| Live query | Supported |
| Extract refresh | Supported |
| Metadata enumeration | Supported |
| Initial SQL | Supported |
| Authentication | Engine-owned (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization / row-level security | Engine-owned; server revalidates all claims |
| UUID catalog identity | Engine-owned |
| Connection pooling | Explicit session (not pool-managed by adaptor) |
| Direct wire ownership | None — adaptor shapes Tableau requests only |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile.
See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection-string reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profiles
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — Diagnostics and SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/powerbi.md -->

<a id="ch-client-driver-guide-adaptors-powerbi-md"></a>

# Power BI Adaptor

> **Draft stub — beta_2 / release_candidate.**
> This page describes the Power BI (Power Query) connector adaptor for ScratchBird.
> Source material is limited to `package_contract.json`; no `.pq` or `.mez`
> connector source is present in the current tree.
> All claims are sourced from that file and the `DriverPackageManifest.csv`
> row; nothing is inferred or invented.

## Purpose

The ScratchBird Power BI adaptor integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Power BI
Desktop and Power BI Service.  It registers ScratchBird as a custom Power Query
connector and exposes DirectQuery mode, Import mode, a NavigationTable for
schema browsing, and a query-folding profile.

The adaptor ships as a Power Query connector artifact (`powerquery_connector`,
typically a `.mez` file compiled from M-language sources).  It does not embed
an underlying ScratchBird driver at the protocol level: the host runtime is the
Power Query connector loader.  Authentication, authorization, MGA transaction
finality, and schema identity are enforced by the ScratchBird engine; the
adaptor shapes navigation and folding requests and never bypasses server
revalidation.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-powerbi` |
| `driver_package_uuid` | `019e12a0-0033-7000-8000-000000000033` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `powerquery_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `explicit_session` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_powerbi_gate` |
| Delegates to | none (no underlying driver; host runtime is the Power Query connector) |
| Package type | `powerquery_connector` |
| License | MPL-2.0 |

**Delegation posture:** `explicit_session`.  The adaptor shapes Power Query
navigation and folding requests but does not own a transport layer.
Authentication authority, MGA transaction finality, UUID identity, and
authorization remain engine-owned.  The package contract explicitly requires
that query folding never bypasses server revalidation or
authorization-filtered metadata.

---

## API Surfaces

The Power Query connector exposes the following surfaces (from
`package_contract.json` `api_surface`):

| Surface | Description |
|---|---|
| `PowerQueryConnector` | The connector registration in Power Query |
| `NavigationTable` | Schema/table browser shown in Power BI's Navigator pane |
| `DirectQuery` | Live DirectQuery mode — queries execute against ScratchBird at report time |
| `ImportMode` | Import mode — data is loaded into the Power BI model |
| `credential_kind_username_password` | Username + password credential kind |
| `folding_profile` | Query-folding configuration (determines which M operations fold to SQL) |

---

## Installation

> **Source note:** The signed connector file (`.mez`) is produced by a Power
> Query packaging gate.  The output filename is not present in the current
> source tree (only `package_contract.json` exists).  Install steps below
> follow the package contract; the exact filename is specified at release time.

### Power BI Desktop

1. Obtain the signed `.mez` connector file from the ScratchBird release.
2. Place it in:
   - **Windows:** `Documents\Power BI Desktop\Custom Connectors\`
3. In Power BI Desktop, go to **File → Options and settings → Options →
   Security** and set **Data extensions** to allow any extension or to allow
   the specific connector (depending on your security posture).
4. Restart Power BI Desktop.
5. ScratchBird will appear in the **Get Data** connector list.

### Power BI Service (via On-premises Data Gateway)

1. Place the `.mez` file in the gateway's custom-connectors folder (configured
   in the gateway settings).
2. Restart the gateway service.
3. Datasets using the ScratchBird connector can now be refreshed through the
   gateway.

> SBOM and signing are both gated on the release build.

---

## Configuring a Connection

When adding ScratchBird as a data source in Power BI, fill in the following
fields (corresponding to the `dsn_key_set`):

| Field | Manifest key | Notes |
|---|---|---|
| Server | `host` | Hostname or IP address of the ScratchBird server |
| Port | `port` | Default: 3092 |
| Database | `database` | Database name |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

TLS is always required: the `scratchbird_tls_1_3_floor` TLS profile enforces
TLS 1.3 as the minimum.  See [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md).

Authentication is engine-owned.  See [../authentication.md](#ch-client-driver-guide-authentication-md).

---

## Type Mapping

This adaptor uses the `powerquery_mapping` type-mapping profile.  ScratchBird
wire types are mapped to Power Query data types according to that profile.

See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| DirectQuery mode | Supported |
| Import mode | Supported |
| NavigationTable (schema browser) | Supported |
| Query folding | Supported via `folding_profile` |
| Username/password credential | Supported |
| Authentication | Engine-owned (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization / filtered metadata | Engine-owned; server revalidates all claims |
| Query folding bypass of server auth | Explicitly prohibited by package contract |
| UUID catalog identity | Engine-owned |
| Connection pooling | Explicit session (not pool-managed by adaptor) |
| Direct wire ownership | None — adaptor shapes Power Query requests only |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile.
See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection-string reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profiles
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — Diagnostics and SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/looker.md -->

<a id="ch-client-driver-guide-adaptors-looker-md"></a>

# Looker Adaptor

> **Draft stub — beta_2 / release_candidate.**
> This page describes the Looker JDBC adaptor for ScratchBird.
> Source material is limited to `package_contract.json`; no LookML dialect
> source or Looker connection-profile files are present in the current tree.
> All claims are sourced from that file and the `DriverPackageManifest.csv`
> row; nothing is inferred or invented.

## Purpose

The ScratchBird Looker adaptor integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Google Looker
(formerly Looker Studio Enterprise / Looker Platform).  It registers
ScratchBird as a JDBC-backed Looker database dialect and exposes LookML
connection profiles, SQL Runner, LookML SQL table naming, explore metadata, and
persistent derived tables (PDTs).

Unlike Tableau or Power BI, the Looker adaptor delegates transport entirely to
the ScratchBird JDBC driver (`driver:jdbc`).  The adaptor itself is a thin
`looker_jdbc_adapter_bundle` that configures Looker's JDBC integration layer;
all network I/O, protocol negotiation, and authentication flow through the JDBC
driver.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-looker` |
| `driver_package_uuid` | `019e12a0-0032-7000-8000-000000000032` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `driver_embedded_jdbc` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `jdbc_url`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `jdbc_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_jdbc` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_looker_gate` |
| Delegates to | `driver:jdbc` (ScratchBird JDBC driver) |
| Package type | `looker_jdbc_adapter_bundle` |
| License | MPL-2.0 |

**Delegation posture:** `delegates_to_jdbc`.  The adaptor shapes Looker
LookML connection-profile and PDT metadata requests; the JDBC driver handles
all transport and protocol binding.  Authentication authority, MGA transaction
finality, authorization, and UUID identity remain engine-owned.  The server
revalidates all incoming command and metadata claims.

---

## API Surfaces

The Looker adaptor exposes the following surfaces (from
`package_contract.json` `api_surface`):

| Surface | Description |
|---|---|
| `connection_profile` | Looker database connection profile configuration |
| `sql_runner` | SQL Runner queries executed against ScratchBird |
| `lookml_sql_table_name` | LookML `sql_table_name` references resolved against ScratchBird schemas |
| `explore_metadata` | Explore-level metadata enumeration |
| `persistent_derived_table` | PDT creation and management through Looker's PDT lifecycle |

---

## Installation

> **Source note:** The bundle artifact is produced by the component runner
> script:
> ```
> python3 project/drivers/scripts/driver_component_runner.py --component adaptor:scratchbird-looker
> ```
> Only `package_contract.json` is present in the current tree.  The install
> steps below are drawn from the package contract and the Looker JDBC
> integration pattern; the exact bundle filename is specified at release time.

1. Build or obtain the `looker_jdbc_adapter_bundle` from the ScratchBird
   release.
2. Obtain the ScratchBird JDBC driver JAR (see [../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md)).
   The bundle embeds (or references) this JAR.
3. Place the JDBC JAR in Looker's JDBC driver directory and register the
   ScratchBird connection profile in Looker's database dialect configuration.
4. Restart Looker.

> SBOM and signing are both gated on the release build.

---

## Configuring a Connection

The Looker adaptor uses a `jdbc_url`-based connection (not individual
host/port/database keys at the Looker UI level).  Provide:

| Field | Manifest key | Notes |
|---|---|---|
| JDBC URL | `jdbc_url` | `jdbc:scratchbird://host:3092/database?sslmode=require` |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

Example JDBC URL:

```
jdbc:scratchbird://db.example.com:3092/analytics?sslmode=require
```

TLS is always required: the `scratchbird_tls_1_3_floor` TLS profile enforces
TLS 1.3 as the minimum.  Additional JDBC properties (TLS certificates, schema
override, manager-proxy fields, etc.) may be appended as URL query parameters
or passed as connection properties; see [../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md)
for the full JDBC property reference.

Authentication is engine-owned.  See [../authentication.md](#ch-client-driver-guide-authentication-md).

---

## Type Mapping

This adaptor uses the `jdbc_mapping` type-mapping profile (inherited from the
underlying JDBC driver).  See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for
the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| SQL Runner | Supported |
| LookML sql_table_name | Supported |
| Explore metadata | Supported |
| Persistent derived tables (PDTs) | Supported |
| Authentication | Engine-owned via JDBC driver (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization / row-level security | Engine-owned; server revalidates all claims |
| PDT and metadata server auth boundaries | Preserved (per package contract) |
| UUID catalog identity | Engine-owned |
| Transport / protocol | Delegated to ScratchBird JDBC driver |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the JDBC driver.
See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md) — ScratchBird JDBC driver (underlying driver)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection-string reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profiles
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — Diagnostics and SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/superset.md -->

<a id="ch-client-driver-guide-adaptors-superset-md"></a>

# Apache Superset Adaptor

> **beta_2 / release_candidate.**
> This page describes the ScratchBird Superset driver package, which provides
> a SQLAlchemy dialect and an Apache Superset DB engine spec.

## Purpose

The `scratchbird-superset` package integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Apache Superset.
It provides two entry points that Superset discovers at startup:

- A **SQLAlchemy dialect** (`ScratchBirdDialect`) registered under the
  `scratchbird` scheme, which translates SQLAlchemy operations to ScratchBird
  DB-API calls.
- A **Superset DB engine spec** (`ScratchBirdEngineSpec`) that teaches Superset
  ScratchBird-specific behaviour: time-grain expressions, schema metadata
  discovery, limit method, catalog support, and DirectQuery configuration.

Both components delegate all DB-API I/O to the ScratchBird Python driver
(`driver:python`).  The adaptor does not speak the SBWP wire itself; it shapes
Superset EngineSpec and SQLAlchemy requests and lets the Python driver handle
transport, authentication, and protocol binding.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-superset-driver` |
| `driver_package_uuid` | `019e12a0-0022-7000-8000-000000000022` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `driver_embedded_python` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `python_dbapi_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_python` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_superset_gate` |
| Delegates to | `driver:python` (ScratchBird Python driver) |
| Package type | `python_wheel` |
| Published package name | `scratchbird-superset` |
| License | Apache-2.0 |

**Delegation posture:** `delegates_to_python`.  The adaptor shapes Superset
EngineSpec and SQLAlchemy requests; the Python driver handles transport and
protocol binding.  Authentication authority, MGA transaction finality,
authorization, and UUID identity remain engine-owned.

---

## API Surfaces

| Surface | Implementation class / entry point |
|---|---|
| SQLAlchemy dialect | `scratchbird_superset.dialect.ScratchBirdDialect` |
| Superset DB engine spec | `scratchbird_superset.engine_spec.ScratchBirdEngineSpec` |
| Time-grain expressions | `ScratchBirdEngineSpec._time_grain_expressions` (second through year, via `DATE_TRUNC`) |
| Schema metadata | `supports_dynamic_schema = True`, `supports_catalog = True` |
| Limit method | `LimitMethod.FORCE_LIMIT` |

Entry points registered in `pyproject.toml`:

```
sqlalchemy.dialects: scratchbird = scratchbird_superset.dialect:ScratchBirdDialect
superset.db_engine_specs: scratchbird = scratchbird_superset.engine_spec:ScratchBirdEngineSpec
```

---

## Installation

Install the wheel into the Superset Python environment (the same environment
that runs Superset workers and the web server):

```bash
# From the repo (development / pre-release)
pip install -e project/drivers/adaptor/scratchbird-superset-driver

# Once published
pip install scratchbird-superset
```

This also installs `scratchbird>=0.1.0` (the Python driver) as a dependency.

After installing, **restart Superset** (web server and all Celery workers) so
that the new entry points are picked up.

Build the wheel for distribution:

```bash
cd project/drivers/adaptor/scratchbird-superset-driver
python3 -m build
# Output: dist/scratchbird_superset-0.1.0-py3-none-any.whl
```

---

## Configuring a Connection in Superset

In Superset, go to **Settings → Database Connections → + Database** and either:

- Select **ScratchBird** from the supported database list (if the EngineSpec
  registers the connector in the UI), or
- Choose **Other** and enter a SQLAlchemy URI manually.

The SQLAlchemy URI format:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

| URI component | Notes |
|---|---|
| `user` | ScratchBird user principal |
| `password` | Password (`engine_local_password` / `scram_ready`) |
| `host` | Hostname or IP address |
| `port` | Default: 3092 |
| `database` | Database name |
| `sslmode` | Recommended: `require`.  `disable` is available for local development only. |

**Additional connection-string options** recognized by the dialect:

| Option | Notes |
|---|---|
| `binary_transfer=true` | Recommended; enables binary-only protocol transfer |
| `currentSchema` / `searchPath` | Schema override; if omitted, the session schema resolves via `SHOW current_schema` with `users.public` fallback |
| `applicationName` | Application tag visible in server-side session views |
| `managerAuthToken` | Manager-proxy authentication token (if using `manager_proxy` ingress) |

The dialect normalizes JDBC-style aliases (`currentSchema`, `searchPath`,
`applicationName`, `managerAuthToken`, and the full staged auth/bootstrap
option family) to the Python driver contract.  See
[../drivers/python.md](#ch-client-driver-guide-drivers-python-md) for the full Python driver
parameter reference.

TLS is always required in production (`scratchbird_tls_1_3_floor`).  See
[../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md).

Authentication is engine-owned.  See [../authentication.md](#ch-client-driver-guide-authentication-md).

---

## Type Mapping

This adaptor uses the `python_dbapi_mapping` type-mapping profile.
See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| Charts and dashboards | Supported (standard Superset SQL execution) |
| Dynamic schema / catalog | Supported (`supports_dynamic_schema`, `supports_catalog`) |
| Joins in SQL Lab | Supported (`allows_joins = True`) |
| Subqueries | Supported (`allows_subqueries = True`) |
| Time-grain aggregations | Supported (second, minute, hour, day, week, month, quarter, year) |
| Authentication | Engine-owned via Python driver (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization | Engine-owned; invalid auth/bootstrap values fail closed |
| UUID catalog identity | Engine-owned |
| Transport / protocol | Delegated to ScratchBird Python driver |
| TLS | `scratchbird_tls_1_3_floor`; `sslmode=disable` available for local dev only |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the Python driver.
See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

Test suite: `python3 -m pytest -q tests` (includes `test_superset_contract.py`
and `test_package_contract.py`).

---

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/python.md](#ch-client-driver-guide-drivers-python-md) — ScratchBird Python driver (underlying driver)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection-string reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profiles
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — Diagnostics and SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/metabase.md -->

<a id="ch-client-driver-guide-adaptors-metabase-md"></a>

# Metabase Adaptor

> **beta_2 / release_candidate.**
> This page describes the ScratchBird Metabase driver plugin, which enables
> Metabase to connect to ScratchBird via the ScratchBird JDBC driver.

## Purpose

The `scratchbird-metabase-driver` package integrates ScratchBird — a Convergent
Data Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Metabase.  It
is a Metabase plugin written in Clojure that registers `scratchbird` as a
`sql-jdbc` child driver.

The adaptor is intentionally thin: it delegates all transport, authentication,
and protocol binding to the ScratchBird JDBC driver (`driver:jdbc`, artifact
`com.scratchbird/scratchbird-jdbc:0.1.0`) declared in `deps.edn`.  The adaptor
contributes Metabase-layer concerns: connection-property UI fields, JDBC spec
construction, type mapping, date-grain SQL, and feature-support flags.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-metabase-driver` |
| `driver_package_uuid` | `019e12a0-0019-7000-8000-000000000019` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `driver_embedded_jdbc` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `jdbc_url`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `jdbc_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_jdbc` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_metabase_gate` |
| Delegates to | `driver:jdbc` (`com.scratchbird/scratchbird-jdbc:0.1.0`) |
| Package type | `metabase_plugin_jar` |
| Build command | `clojure -T:build jar` |
| Output artifact | `target/scratchbird-metabase-driver-0.1.0.jar` |
| License | MPL-2.0 |

**Delegation posture:** `delegates_to_jdbc`.  The adaptor shapes Metabase
feature flags, metadata, and HoneySQL query requests; the JDBC driver handles
all transport and protocol binding.  Authentication authority, MGA transaction
finality, authorization, and UUID identity remain engine-owned.

---

## Plugin Manifest

The plugin is declared in `metabase-plugin.yaml`:

```yaml
info:
  name: ScratchBird
  version: 0.1.0
  description: ScratchBird JDBC driver for Metabase

driver:
  name: scratchbird
  init: metabase.driver.scratchbird/init
  parent: sql-jdbc
```

The `parent: sql-jdbc` declaration means the driver inherits all Metabase
`sql-jdbc` behaviour and overrides only what is ScratchBird-specific.

---

## Clojure Namespaces

| Namespace | File | Role |
|---|---|---|
| `metabase.driver.scratchbird` | `src/metabase/driver/scratchbird.clj` | Driver registration, `connection-details->spec`, `can-connect?`, `db-default-timezone`, `database-supports?`, type-mapping dispatch, HoneySQL date functions |
| `metabase.driver.scratchbird-support` | `src/metabase/driver/scratchbird_support.clj` | Pure config: type map, feature flags, connection-property definitions, JDBC property builder |

The JDBC driver class is `com.scratchbird.jdbc.SBDriver`, with subprotocol
`scratchbird`.  JDBC URLs are constructed as:

```
jdbc:scratchbird://host:port/database
```

---

## Installation

### Build the Plugin JAR

```bash
cd project/drivers/adaptor/scratchbird-metabase-driver
clojure -T:build jar
# Output: target/scratchbird-metabase-driver-0.1.0.jar
```

This bundles `metabase-plugin.yaml`, the compiled Clojure namespaces, and the
ScratchBird JDBC JAR into a single plugin JAR.

Expected JAR layout:

```
scratchbird-metabase-driver-0.1.0.jar
├── metabase-plugin.yaml
└── metabase/driver/scratchbird.clj   (compiled)
```

### Install in Metabase

1. Copy `target/scratchbird-metabase-driver-0.1.0.jar` to `MB_PLUGINS_DIR`
   (the directory Metabase scans for plugin JARs, typically `./plugins/`).
2. Restart Metabase.
3. ScratchBird will appear as a database option in the **Add Database** dialog.

> Update the `com.scratchbird/scratchbird-jdbc` version in `deps.edn` to match
> your ScratchBird release version before building.

---

## Configuring a Connection in Metabase

When adding ScratchBird in Metabase's **Add Database** dialog, the following
fields are presented (sourced from `scratchbird-support.clj`
`scratchbird-connection-properties`):

| Field | Key | Type | Default | Notes |
|---|---|---|---|---|
| Host | `host` | string | `localhost` | Hostname or IP address |
| Port | `port` | integer | `3092` | Default ScratchBird port |
| Database | `db` | string | — | Database name |
| Username | `user` | string | — | ScratchBird user principal |
| Password | `password` | password | — | |
| SSL Mode | `sslmode` | select | `require` | `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full` |
| CA Certificate | `sslrootcert` | string | — | Path to CA certificate file |
| Client Certificate | `sslcert` | string | — | Path to client certificate |
| Client Key | `sslkey` | string | — | Path to client key |
| SSL Key Password | `sslpassword` | password | — | |
| Role | `role` | string | — | Optional role to SET on connect |
| Current Schema | `currentSchema` | string | — | Optional schema override; if omitted, server default applies (`users.public` fallback) |

Additional JDBC fields exposed by the adaptor (from README): `search_path`,
manager-proxy fields (`managerAuthToken`, etc.), staged auth/bootstrap,
workload-identity, proxy-assertion, and dormant-reattach options.

TLS is always required in production (`sslmode=require` default,
`scratchbird_tls_1_3_floor`).  See [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md).

Authentication is engine-owned.  See [../authentication.md](#ch-client-driver-guide-authentication-md).

---

## Type Mapping

The adaptor maps ScratchBird database types to Metabase base types in
`scratchbird-support.clj`.  Representative mappings:

| ScratchBird type | Metabase base type |
|---|---|
| `BOOLEAN` | `:type/Boolean` |
| `SMALLINT`, `INTEGER`, `INT` | `:type/Integer` |
| `BIGINT`, `INT8` | `:type/BigInteger` |
| `REAL`, `FLOAT`, `DOUBLE` | `:type/Float` |
| `NUMERIC`, `DECIMAL` | `:type/Decimal` |
| `CHAR`, `VARCHAR`, `TEXT` | `:type/Text` |
| `DATE` | `:type/Date` |
| `TIME` | `:type/Time` |
| `TIMESTAMP` | `:type/DateTime` |
| `TIMESTAMPTZ` / `TIMESTAMP WITH TIME ZONE` | `:type/DateTimeWithTZ` |
| `UUID` | `:type/UUID` |
| `JSON`, `JSONB`, `VARIANT` | `:type/JSON` |
| `ARRAY`, `VECTOR` | `:type/Array` |
| `INET`, `CIDR` | `:type/IPAddress` |

This adaptor uses the `jdbc_mapping` type-mapping profile.
See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the full profile.

---

## Feature Flags

The adaptor declares which Metabase features ScratchBird supports via
`scratchbird-feature-support` in `scratchbird_support.clj`.  Selected flags:

| Feature | Supported |
|---|---|
| Foreign keys | Yes |
| Schemas | Yes |
| Basic / standard-deviation / expression / percentile aggregations | Yes |
| Expressions (today, datetime, date, integer, float, text) | Yes |
| Window functions (cumulative, offset) | Yes |
| Regex | Yes (lookaheads/lookbehinds: No) |
| Collate | Yes |
| UUID type | Yes |
| Key constraints / describe-fields / describe-indexes | Yes |
| Table privileges | No |
| Nested field columns | No |
| Uploads / upload-with-auto-pk | No |
| Multiple databases per connection | No |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the JDBC driver.
See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

Test suite: `clojure -T:build jar` (build smoke); `metabase_support_contract_smoke`.

---

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md) — ScratchBird JDBC driver (underlying driver)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection-string reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profiles
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — Diagnostics and SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/dbeaver.md -->

<a id="ch-client-driver-guide-adaptors-dbeaver-md"></a>

# DBeaver Adaptor

> **beta_2 / release_candidate.**
> This page describes the ScratchBird DBeaver plugin, which extends DBeaver CE
> and DBeaver Pro to connect to ScratchBird via the ScratchBird JDBC driver.

## Purpose

The `scratchbird-dbeaver-driver` adaptor integrates ScratchBird — a Convergent
Data Engine (CDE) speaking SBWP v1.1 on default port 3092 — into DBeaver
Community Edition (CE) and compatible DBeaver distributions.  It is an Eclipse
plugin bundle (p2 feature) that teaches DBeaver ScratchBird-specific behaviour:
recursive schema tree navigation, ScratchBird data types, SQL editor parser
annotations, quick-assist fixes, UI context actions, and mutation-review
warnings.

The adaptor delegates all network I/O to the ScratchBird JDBC driver
(`driver:jdbc`).  The plugin bundle includes the JDBC JAR automatically; manual
Driver Manager attachment is only needed when bypassing the provided installers.

**Release status:** beta_2, release_candidate.
**Validated against:** DBeaver CE 26.0.2.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-dbeaver-driver` |
| `driver_package_uuid` | `019e12a0-0017-7000-8000-000000000017` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `manager_proxy`, `driver_embedded_jdbc` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `jdbc_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_jdbc` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_dbeaver_gate` |
| Delegates to | `driver:jdbc` (ScratchBird JDBC driver) |
| Build system | Tycho 4.0.8 (Maven), Eclipse 2025-12 target platform |
| JDBC driver class | `com.scratchbird.jdbc.SBDriver` |
| License | MPL-2.0 |

**Delegation posture:** `delegates_to_jdbc`.  The plugin shapes DBeaver UI,
metadata, and SQL-editor requests; the JDBC driver handles all transport and
protocol binding.  Authentication authority, MGA transaction finality,
authorization, and UUID identity remain engine-owned.

---

## Eclipse Plugin Modules

The adaptor is structured as an Eclipse Tycho project with four modules:

| Module | Artifact ID | Role |
|---|---|---|
| `org.jkiss.dbeaver.ext.scratchbird` | Model plugin | JDBC metadata, recursive schema tree (`ScratchBirdSchema`), Generic DDL folders, `SBDriver` wiring |
| `org.jkiss.dbeaver.ext.scratchbird.ui` | UI plugin | Navigator context actions, form shell, parser validation, live editor annotations, quick-assist fixes and completion proposals |
| `org.jkiss.dbeaver.ext.scratchbird.feature` | p2 feature | Feature descriptor bundling both plugins for p2 install |
| `repository` | p2 update-site | Category.xml and p2 repository module |

Build artifacts (from `pom.xml`):

- `groupId`: `com.scratchbird.dbeaver`
- `artifactId`: `scratchbird-dbeaver-driver-build`
- `version`: `1.0.1-SNAPSHOT`
- `Bundle-Version`: `1.0.1.qualifier` (model plugin MANIFEST.MF)
- `Bundle-RequiredExecutionEnvironment`: JavaSE-21

---

## Installation into Stock DBeaver (No Source Build)

This is the recommended path for end users.

### 1. Build the p2 Update Site

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/build-p2-update-site.sh
```

The script invokes the JDBC wrapper at `project/drivers/driver/jdbc/gradlew`,
stages the current JDBC JAR into the plugin bundle, and produces:

- **Repository folder:**
  `build/drivers/adaptor/scratchbird-dbeaver-driver/p2-work/source/repository/target/repository`
- **Installable zip:**
  `build/drivers/adaptor/scratchbird-dbeaver-driver/dist/scratchbird-dbeaver-update-site-*.zip`

Alternatively, build a cross-platform tester bundle (includes installer scripts
and checksums):

```bash
./scripts/build-stock-test-bundle.sh
# Output: build/drivers/adaptor/scratchbird-dbeaver-driver/dist/scratchbird-dbeaver-stock-test-bundle-*.zip
```

### 2. Install in DBeaver

1. Open DBeaver.
2. Go to **Help → Install New Software...**
3. Click **Add... → Archive...** and select the generated zip from step 1.
4. Select **ScratchBird Extension**.
5. Complete the install wizard and restart DBeaver.

**Headless (p2 director) install** is also supported for automated environments:

```bash
dbeaver -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -repository "jar:file:<path-to-zip>!/" \
  -installIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group \
  -destination <dbeaver-install-dir> \
  -profile DefaultProfile \
  -bundlepool <dbeaver-install-dir> \
  -profileProperties org.eclipse.update.install.features=true
```

**Stock bundle installer scripts** (from `scripts/`):

| Script | Platform | Usage |
|---|---|---|
| `install-into-stock-dbeaver.sh` | Unix-like | `./install-into-stock-dbeaver.sh [/path/to/dbeaver]` |
| `install-into-stock-dbeaver.bat` | Windows | `install-into-stock-dbeaver.bat [DBEAVER_INSTALL_DIR]` |

If the `scratchbird-dbeaver-update-site-*.zip` is next to the installer script,
it is auto-detected.  The JDBC JAR is bundled in the update site; manual Driver
Manager attachment is not needed when using the provided installers.

> If **Help → Install New Software...** is disabled in your DBeaver install,
> p2 install policies are being enforced.  Use an unmanaged install or contact
> your administrator.

---

## Installation into DBeaver Source Checkout (Developer Flow)

```bash
cd project/drivers/adaptor/scratchbird-dbeaver-driver
./scripts/install-into-dbeaver.sh ../dbeaver
```

This copies plugin and test-plugin folders and patches the DBeaver aggregator
POMs (`plugins/pom.xml`, `test/pom.xml`) and feature XMLs.  Then build:

```bash
cd ../dbeaver
../dbeaver-common/mvnw -f product/aggregate/pom.xml \
  -pl ...,../../plugins/org.jkiss.dbeaver.ext.scratchbird,\
../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui,\
../../test/org.jkiss.dbeaver.ext.scratchbird.test \
  -am verify -DskipITs
```

Expected result: `BUILD SUCCESS`.

---

## Configuring a Connection in DBeaver

The plugin manifest exposes the full ScratchBird JDBC ingress/auth/bootstrap
property surface in DBeaver's connection form.  Standard fields:

| Field | Manifest key | Notes |
|---|---|---|
| Host | `host` | Hostname or IP address |
| Port | `port` | Default: 3092 |
| Database | `database` | Database name |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

TLS: set `sslmode=require` (or stronger) in the connection properties.
The `scratchbird_tls_1_3_floor` TLS profile enforces TLS 1.3 as the minimum.
See [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md).

Additional JDBC properties available in the connection form include:
`sslmode`, `sslrootcert`, `sslcert`, `sslkey`, `currentSchema`, manager-proxy
fields (`managerAuthToken`, `managerUsername`, etc.), token/assertion,
channel-binding, and dormant-reattach options.  See
[../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md) for the full JDBC property reference.

Authentication is engine-owned.  See [../authentication.md](#ch-client-driver-guide-authentication-md).

### Live JDBC Endpoint (Validated)

From the DBeaver README validation snapshot (2026-04-24):

```
jdbc:scratchbird://127.0.0.1:13092/main?sslmode=disable
```

(Port 13092 is a local test fixture; production port is 3092.)

---

## Recursive Schema Tree

The plugin uses first-class `ScratchBirdSchema` objects for schema metadata
and deterministic recursive tree ordering.  Root display policy (from README):

> User/application roots first, reserved management roots next, `sys` last.

The JDBC driver includes a metadata compatibility switch for recursive-schema
clients:

| JDBC property | Aliases | Default | Effect when `true` |
|---|---|---|---|
| `metadataExpandSchemaParents` | `metadata_expand_schema_parents`, `expandSchemaParents`, `dbeaver_expand_schema_parents` | `false` | `DatabaseMetaData.getSchemas()` emits dotted parent segments (e.g. `users`, `users.alice`, `users.alice.dev`) while preserving pattern filtering |

Enable this only when needed; the default preserves standard JDBC metadata
behaviour for non-DBeaver clients.

---

## SQL Editor Features

The UI plugin (`org.jkiss.dbeaver.ext.scratchbird.ui`) contributes:

- ScratchBird navigator context actions
- Form/report shell surfaces with a generic form shell validation tab
- SQL editor manual validation with live Java v3 parser annotations
- Quick-assist fixes for removed aliases
- Quick-assist Java v3 parser completion proposals (Ctrl+Space)
- Object-model identity/parentage hints and read-only live evidence hints
- Mutation-review warnings for:
  - DDL/DML/DCL on protected/control/client-only ScratchBird surfaces
  - DML on collection surfaces instead of concrete mutable child objects
  - Unrooted DML (no rooted ScratchBird target or selected object context)
  - DCL `GRANT`/`REVOKE ON` referencing collection or broad surfaces
  - DCL broad-principal warnings (`users`, `users.public`, etc.)
  - Transaction-publication batches containing schema/security metadata mutation
  - Dual-anchor batches mixing schema and security metadata mutation
  - Uncommitted-metadata-read warnings after uncommitted schema/security mutation in an explicit transaction

---

## Type Mapping

This adaptor uses the `jdbc_mapping` type-mapping profile (inherited from the
underlying JDBC driver).  See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) for the
full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| JDBC connection | Supported |
| Recursive schema tree | Supported (`ScratchBirdSchema`, deterministic ordering) |
| `metadataExpandSchemaParents` | Optional JDBC property; default `false` |
| SQL editor with parser annotations | Supported (Java v3 parser) |
| Quick-assist and completion | Supported |
| Mutation-review warnings | Supported (DDL/DML/DCL/transaction-publication) |
| Authentication | Engine-owned via JDBC driver |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization | Engine-owned; server revalidates all claims |
| Deeper GUI validation (Phase B/C) | Pending (see DBeaver README open items) |
| `sys.security.permission_probe` | Server implementation pending |
| Human GUI screenshot evidence | Pending |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the JDBC driver.
See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

Test suite: `test/org.jkiss.dbeaver.ext.scratchbird.test/` (26 ScratchBird
tests covering plugin metadata, URL-mode connection, recursive namespace
semantics, UI extension declarations, parser integration, and root ordering
policy; validated 2026-04-24).

---

## See Also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md) — ScratchBird JDBC driver (underlying driver)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection-string reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../tls_profiles.md](#ch-client-driver-guide-tls-profiles-md) — TLS profiles
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — Diagnostics and SQLSTATE




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/hibernate.md -->

<a id="ch-client-driver-guide-adaptors-hibernate-md"></a>

# Hibernate Adaptor

The Hibernate adaptor (`scratchbird-hibernate-dialect`) provides a Hibernate ORM dialect for ScratchBird. It enables Java applications using Hibernate to model entities against a ScratchBird database without writing raw JDBC. The adaptor delegates all wire transport to the ScratchBird JDBC driver; the dialect layer is responsible only for SQL rendering, type contribution, qualified-name formatting, and ORM metadata contracts.

**Status:** beta_2 (release_candidate bucket). Full live JPA bootstrap, entity lifecycle, and migration/runtime matrix coverage remains DSN/runtime-gated per the source README.

**Conformance profile:** `adaptor_hibernate_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-hibernate-dialect` |
| driver\_package\_uuid | `019e12a0-0018-7000-8000-000000000018` |
| api\_surface | `application_adapter` |
| ingress\_mode | `driver_embedded_jdbc` |
| delegates to / pooling | `delegates_to_jdbc` (driver:jdbc) |
| type\_mapping\_profile | `jdbc_mapping` |
| DSN / connection keys | `jdbc_url`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| license | MPL-2.0 |

---

## Installation

The dialect is distributed as a Maven JAR. Add it to your project alongside the ScratchBird JDBC driver.

**Maven coordinates** (from `pom.xml`):

```xml
<dependency>
  <groupId>com.scratchbird</groupId>
  <artifactId>scratchbird-hibernate-dialect</artifactId>
  <version>0.1.0-beta</version>
</dependency>
```

The artifact requires **Java 17** and **Hibernate ORM 6.4.x** (`org.hibernate.orm:hibernate-core:6.4.4.Final`). The JDBC driver (`driver:jdbc`) must also be on the classpath; the dialect does not bundle a driver copy.

**Dialect class** (from `ScratchBirdDialect.java`):

```
com.scratchbird.hibernate.ScratchBirdDialect
```

`ScratchBirdDialect` extends `org.hibernate.dialect.PostgreSQLDialect`. Register it in your Hibernate configuration via the `hibernate.dialect` property.

---

## Configuring a connection

Supply the JDBC URL and dialect class in `hibernate.properties` (or the equivalent `persistence.xml` / Spring `DataSource` properties). The following example is taken directly from `examples/hibernate.properties.example` in the source:

```properties
hibernate.connection.url=jdbc:scratchbird://127.0.0.1:3092/main?sslmode=require&binaryTransfer=true
hibernate.connection.username=sb_admin
hibernate.connection.password=change_me
hibernate.dialect=com.scratchbird.hibernate.ScratchBirdDialect
hibernate.hbm2ddl.auto=validate
hibernate.show_sql=true
hibernate.format_sql=true
hibernate.jdbc.batch_size=64
hibernate.connection.provider_disables_autocommit=true
```

**JDBC URL structure:**

```
jdbc:scratchbird://<host>:<port>/<database>[?param=value&...]
```

Default port is 3092. `sslmode=require` and `binaryTransfer=true` are the baseline policy values; the dialect enforces that unsafe combinations (`sslmode=disable`, `binaryTransfer=false`) are rejected before delegated JDBC execution (`ScratchBirdJdbcUrlPolicy`).

---

## Type and feature mapping

The dialect contributes ScratchBird-to-Hibernate type mappings via `ScratchBirdTypeMappings`. Representative mappings (from source):

| ScratchBird type | Hibernate mapping |
|---|---|
| BOOLEAN | boolean |
| SMALLINT | short |
| INTEGER / INT | integer |
| BIGINT | long |
| REAL / FLOAT / DOUBLE | float / double |
| NUMERIC / DECIMAL | big\_decimal |
| VARCHAR / CHAR / TEXT | string / text |
| DATE / TIME / TIMESTAMP | date / time / timestamp |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | timestamp\_utc |
| UUID | uuid-char |
| JSON / JSONB | json |
| BYTEA | binary |
| BLOB | materialized\_blob |
| VECTOR | string (unsupported note) |
| GEOMETRY | binary |

Array types (e.g. `BIGINT[]`) map to `string`.

For the full type mapping reference see [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- Dialect registration for Hibernate ORM 6.4.x via `ScratchBirdDialect`
- JDBC URL policy enforcement (`ScratchBirdJdbcUrlPolicy`) — rejects unsafe flags before delegated execution
- Type contribution map via `ScratchBirdTypeMappings`
- Schema-qualified name rendering via `formatQualifiedName(schema, objectName)`
- Identity column DDL clause via `formatIdentityColumnClause(columnName)` — renders `bigint generated by default as identity`
- Recursive schema metadata support (`supportsRecursiveSchemaMetadata()` returns `true`)
- Transaction/savepoint SQL lifecycle contract helpers (`ScratchBirdTransactionContract`)
- JDBC metadata to Hibernate column-definition mapping helpers (`ScratchBirdJdbcMetadataMapper`)

**Deferred / runtime-gated (per source README):**

- Full live JPA bootstrap and entity lifecycle against a running ScratchBird server
- `hbm2ddl.auto=create/update` DDL generation coverage — validate mode is tested; create/update against a live server is environment-gated
- Migration and full runtime matrix coverage

All connection-level authority (authentication, authorization, MGA transaction finality) is delegated to the ScratchBird JDBC driver and ultimately revalidated server-side. The dialect has `driver_local_authority: advisory_only`.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/jdbc.md](#ch-client-driver-guide-drivers-jdbc-md) — ScratchBird JDBC driver (the underlying transport)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and JDBC URL reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE and error mapping




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/sqlalchemy.md -->

<a id="ch-client-driver-guide-adaptors-sqlalchemy-md"></a>

# SQLAlchemy Adaptor

The SQLAlchemy adaptor (`scratchbird-sqlalchemy-dialect`) provides a SQLAlchemy dialect for ScratchBird. It enables Python applications using SQLAlchemy — including ORM-mapped classes, Core expressions, and Inspector-based reflection — to connect to ScratchBird using a `scratchbird://` URL. The adaptor delegates all wire transport to the ScratchBird Python driver (DB-API 2.0); the dialect layer handles connection argument normalization, type mapping, and schema reflection queries.

**Status:** beta_2 (release_candidate bucket). Deterministic contract tests run without a live server; full ORM transaction and migration coverage requires a running ScratchBird instance.

**Conformance profile:** `adaptor_sqlalchemy_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-sqlalchemy-dialect` |
| driver\_package\_uuid | `019e12a0-0021-7000-8000-000000000021` |
| api\_surface | `application_adapter` |
| ingress\_mode | `driver_embedded_python` |
| delegates to / pooling | `delegates_to_python` (driver:python) |
| type\_mapping\_profile | `python_dbapi_mapping` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| license | Apache-2.0 (pyproject.toml) |

---

## Installation

The dialect is distributed as a Python wheel (`scratchbird-sqlalchemy`). Install it alongside the ScratchBird Python driver.

**Package name** (from `pyproject.toml`):

```bash
pip install scratchbird-sqlalchemy
# or editable install from source:
pip install -e path/to/scratchbird-sqlalchemy-dialect
```

**Dependencies:**

- `scratchbird>=0.1.0` (the ScratchBird Python driver)
- `SQLAlchemy>=1.4,<3.0`
- Python 3.9 or later

**Dialect registration** is via the `sqlalchemy.dialects` entry point declared in `pyproject.toml`:

```
scratchbird = "scratchbird_sqlalchemy.dialect:ScratchBirdDialect"
```

After installation, SQLAlchemy resolves `scratchbird://` URLs to `ScratchBirdDialect` automatically. No manual registration is required.

**Dialect class** (from `scratchbird_sqlalchemy/dialect.py`):

```
scratchbird_sqlalchemy.dialect.ScratchBirdDialect
```

`ScratchBirdDialect` extends `sqlalchemy.engine.default.DefaultDialect`.

---

## Configuring a connection

Pass a `scratchbird://` URL to `create_engine`. From the source README:

```python
from sqlalchemy import create_engine, inspect

engine = create_engine(
    "scratchbird://user:pass@localhost:3092/mydb?sslmode=require&binaryTransfer=true"
)
inspector = inspect(engine)
print(inspector.get_schema_names())
```

**URL structure:**

```
scratchbird://<user>:<password>@<host>:<port>/<database>[?param=value&...]
```

Default host is `localhost`, default port is `3092`.

**Connection argument policy** (`create_connect_args` in source):

The dialect normalizes URL query parameters into the keyword arguments passed to the Python driver. Aliases are resolved (e.g. `binaryTransfer` → `binary_transfer`, `currentSchema` → `schema`). The following values are rejected at connection-arg construction time before any driver call is made:

| Parameter | Rejected value | Reason |
|---|---|---|
| `sslmode` (or `ssl`) | `disable` | TLS floor policy |
| `binary_transfer` | `false` | Protocol requirement |
| `compression` | `zstd` | Not supported |
| `front_door_mode` | any value other than `direct` or `manager_proxy` | Policy |
| `auth_method_id` | any value not starting with `scratchbird.auth.` | Policy |

All manager-proxy, token/assertion, channel-binding, and dormant-reattach options are accepted as canonical query parameters.

---

## Type and feature mapping

`ScratchBirdDialect` carries a built-in `_TYPE_MAP` (from `dialect.py`) covering the full ScratchBird type surface. Representative entries:

| ScratchBird type | SQLAlchemy type |
|---|---|
| BOOLEAN | Boolean |
| SMALLINT | SmallInteger |
| INTEGER / INT | Integer |
| BIGINT / INT8 | BigInteger |
| REAL / FLOAT / DOUBLE | Float |
| NUMERIC / DECIMAL | Numeric |
| VARCHAR / CHAR / TEXT | String / Text |
| DATE / TIME / TIMESTAMP | Date / Time / DateTime |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | DateTime(timezone=True) |
| UUID | Uuid |
| JSON / JSONB | JSON |
| BYTEA / BLOB | LargeBinary |
| ARRAY | ARRAY(String) |
| VECTOR | ARRAY(Float) |
| GEOMETRY / GEOGRAPHY | LargeBinary |
| COMPOSITE / RECORD / ROW / RANGE | String |
| TSVECTOR / TSQUERY | Text |
| INET / CIDR / MACADDR | String |
| XML / INTERVAL | Text / String |
| MONEY | Numeric |

Type names with precision/scale suffixes (e.g. `VARCHAR(255)`) are normalized by stripping the parenthetical before lookup. Array-suffix types (e.g. `TEXT[]`) map to `ARRAY(String)`.

For the full type mapping reference see [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- `create_engine("scratchbird://...")` URL handling and connection argument construction
- Inspector reflection:
  - `get_schema_names` — queries `sys.schemas`
  - `get_table_names` — queries `sys.tables` joined to `sys.schemas`, excludes views
  - `get_view_names` — queries `sys.tables` where `table_type = 'VIEW'`
  - `has_table` — existence check via `sys.tables`
  - `get_columns` — queries `sys.columns`; returns `name`, `type`, `nullable`, `default`, `autoincrement`
  - `get_pk_constraint` — queries `information_schema.table_constraints` / `key_column_usage`
  - `get_foreign_keys` — queries `information_schema.referential_constraints`
  - `get_indexes` — queries `sys.indexes` and `sys.index_columns`
- Schema-qualified reflection (pass `schema=` keyword to all reflection methods)
- Native boolean, decimal, and UUID support (`supports_native_boolean/decimal/uuid = True`)
- Named paramstyle (`paramstyle = "named"`)
- Statement cache support (`supports_statement_cache = True`)

**Not confirmed in source / deferred to driver or engine:**

- DDL generation (CREATE TABLE, ALTER TABLE, migrations) — not tested in contract suite
- ORM `Session` transaction lifecycle against a live server — deterministic tests are offline
- Connection pooling — managed by the underlying Python driver and SQLAlchemy pool; the adaptor itself has no pool

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/python.md](#ch-client-driver-guide-drivers-python-md) — ScratchBird Python driver (DB-API 2.0)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection parameter reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE and error mapping




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/prisma.md -->

<a id="ch-client-driver-guide-adaptors-prisma-md"></a>

# Prisma Adaptor

The Prisma adaptor (`scratchbird-prisma-adapter`) provides adapter-level scaffolding for using ScratchBird with Prisma. It handles datasource URL validation, ScratchBird-to-Prisma scalar/native type mapping, metadata-to-`schema.prisma` model generation, and migration/reflection workflow contracts. The adaptor delegates all wire transport to the ScratchBird Node.js driver.

**Status:** beta_2 (release_candidate bucket). This is not yet a full Prisma provider runtime. The source README explicitly states: "This is not yet a full Prisma provider runtime." The package provides adapter-level contracts and deterministic tests to de-risk datasource validation, scalar mapping, schema generation, and migration/reflection workflows. A live Prisma client query lifecycle is environment-gated.

**Conformance profile:** `adaptor_prisma_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-prisma-adapter` |
| driver\_package\_uuid | `019e12a0-0020-7000-8000-000000000020` |
| api\_surface | `application_adapter` |
| ingress\_mode | `driver_embedded_node` |
| delegates to / pooling | `delegates_to_node` (driver:node) |
| type\_mapping\_profile | `sbsql_core` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| license | MPL-2.0 |

---

## Installation

The adaptor is distributed as an npm package. Node.js 18 or later is required.

**Package name** (from `package.json`):

```
scratchbird-prisma-adapter
```

**Install:**

```bash
npm install scratchbird-prisma-adapter
```

The ScratchBird Node.js driver must also be available. The adaptor `main` entry point is `lib/index.js`.

---

## Configuring a connection

Connection URLs follow the `scratchbird://` scheme. From the source (`lib/connection-url.js`), the URL must use the `scratchbird:` protocol; any other scheme is rejected.

**URL structure:**

```
scratchbird://<user>:<password>@<host>:<port>/<database>[?param=value&...]
```

Default host is `localhost`, default port is `3092`.

**`schema.prisma` datasource block** (from `examples/schema.prisma`):

```prisma
datasource db {
  provider = "scratchbird"
  url      = env("DATABASE_URL")
}

generator client {
  provider = "prisma-client-js"
}
```

The adapter validates that the schema text contains a `datasource` block, a `generator` block, and that the datasource URL is set via `env("DATABASE_URL")`. Inline URL literals are not accepted by the schema validator.

**`DATABASE_URL` example:**

```
DATABASE_URL=scratchbird://sb_admin:change_me@127.0.0.1:3092/main?sslmode=require&binaryTransfer=true
```

**Connection parameter guardrails** (`normalizeScratchbirdQueryParams` in `lib/connection-url.js`):

The adaptor normalizes query parameter aliases and enforces the following policy before any driver call:

| Parameter | Rejected value | Error |
|---|---|---|
| `sslmode` (or `ssl`) | `disable` | sslmode=disable is not supported |
| `binary_transfer` | `false` | binary\_transfer=false is not supported |
| `compression` | `zstd` | compression=zstd is not supported |
| `front_door_mode` | other than `direct` / `manager_proxy` | must be direct or manager\_proxy |
| `auth_method_id` | not prefixed `scratchbird.auth.` | must start with scratchbird.auth. |

Manager-proxy, token/assertion, channel-binding, and dormant-reattach option families are all accepted as canonical query parameters.

---

## Type and feature mapping

`lib/type-map.js` provides `mapScratchBirdTypeToPrisma`. The mapping covers the Prisma scalar types. Representative entries:

| ScratchBird type | Prisma type | Native type annotation |
|---|---|---|
| BOOLEAN | Boolean | — |
| SMALLINT / INTEGER / INT | Int | — |
| BIGINT | BigInt | — |
| REAL / FLOAT / DOUBLE PRECISION | Float | — |
| NUMERIC / DECIMAL | Decimal | — |
| VARCHAR / CHARACTER VARYING / TEXT | String | — |
| UUID | String | Uuid |
| JSON / JSONB | Json | — |
| BYTEA | Bytes | — |
| DATE | DateTime | Date |
| TIMESTAMP | DateTime | Timestamp |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | DateTime | Timestamptz |

Types not in the map are reported as `String` with `unsupported: true`. Array-suffix types (e.g. `TEXT[]`) are flagged as array variants.

For the full type mapping reference see [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- Datasource URL parsing and validation (`parseScratchbirdConnectionUrl`)
- `schema.prisma` text validation (datasource, generator, `env("DATABASE_URL")` requirement)
- ScratchBird-to-Prisma scalar/native type mapping (`mapScratchBirdTypeToPrisma`)
- Metadata-to-`schema.prisma` model generation (schema generator utility in `lib/schema-generator.js`)
- Deterministic reflection round-trip contract helpers (`lib/workflow.js`)
- Migration plan builder for Prisma migration layout (`lib/workflow.js`)
- Full canonical option alias normalization (manager-proxy, token/assertion, channel-binding, dormant-reattach)

**Not yet available / environment-gated (per source README):**

- Full Prisma provider runtime — this package is not a complete Prisma provider
- Live Prisma Client query execution (`findMany`, `create`, etc.) against a running ScratchBird server
- `prisma migrate` CLI integration — migration plan builder provides contract scaffolding only

All connection-level authority is delegated to the ScratchBird Node.js driver and revalidated server-side. The adaptor has `driver_local_authority: advisory_only`.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/node.md](#ch-client-driver-guide-drivers-node-md) — ScratchBird Node.js driver (the underlying transport)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection parameter reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE and error mapping




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/typeorm.md -->

<a id="ch-client-driver-guide-adaptors-typeorm-md"></a>

# TypeORM Adaptor

The TypeORM adaptor (`@scratchbird/typeorm-adapter`) provides adapter-level contracts for using ScratchBird with TypeORM. It normalizes TypeORM `DataSource` options, maps ScratchBird types to TypeORM column types, generates TypeORM-style entity schemas from ScratchBird metadata catalogs, and builds transaction command plans for nested CRUD operations. The adaptor delegates all wire transport to the ScratchBird Node.js driver.

**Status:** beta_2 (release_candidate bucket). The source README explicitly states: "Full live TypeORM runtime execution against managed/listener endpoints remains environment-gated." The package provides deterministic adapter contracts; a full live TypeORM `DataSource` session against a running ScratchBird server is not yet covered in the contract suite.

**Conformance profile:** `adaptor_typeorm_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-typeorm-adapter` |
| driver\_package\_uuid | `019e12a0-0023-7000-8000-000000000023` |
| api\_surface | `application_adapter` |
| ingress\_mode | `driver_embedded_node` |
| delegates to / pooling | `delegates_to_node` (driver:node) |
| type\_mapping\_profile | `sbsql_core` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| license | MPL-2.0 |

---

## Installation

The adaptor is distributed as an npm package. Node.js 18 or later is required.

**Package name** (from `package.json`):

```
@scratchbird/typeorm-adapter
```

**Install:**

```bash
npm install @scratchbird/typeorm-adapter
```

The ScratchBird Node.js driver must also be available. The adaptor `main` entry point is `lib/index.js`.

---

## Configuring a connection

TypeORM is configured via a `DataSource` options object. The adaptor's `normalizeTypeOrmOptions` function (in `lib/options.js`) accepts either a URL string or individual host/port/database fields, merges any `extra` query parameters, and applies policy guardrails.

**Options object** (from `examples/sample-service.js`):

```javascript
const adapter = require("@scratchbird/typeorm-adapter");

const options = adapter.normalizeTypeOrmOptions({
  url: "scratchbird://sb_admin:change_me@127.0.0.1:3092/main?sslmode=require&binaryTransfer=true",
  extra: {
    connectTimeout: "30",
  },
});
// options.type === "scratchbird"
// options.host === "127.0.0.1"
// options.port === 3092
// options.database === "main"
```

**URL structure:**

```
scratchbird://<user>:<password>@<host>:<port>/<database>[?param=value&...]
```

Default host is `localhost`, default port is `3092`, default database is `main`. The URL protocol must be `scratchbird:`; any other protocol is rejected.

**Extra option guardrails** (`enforceGuardrails` in `lib/options.js`):

The adaptor normalizes connection option aliases and enforces these policy rules before any driver call:

| Parameter | Rejected value | Error |
|---|---|---|
| `sslmode` (or `ssl`) | `disable` | sslmode=disable is not supported |
| `binary_transfer` (or `binaryTransfer`) | `false` | binaryTransfer=false is not supported |
| `compression` | `zstd` | compression=zstd is not supported |
| `front_door_mode` | other than `direct` / `manager_proxy` | must be direct or manager\_proxy |
| `auth_method_id` | not prefixed `scratchbird.auth.` | must start with scratchbird.auth. |

---

## Type and feature mapping

`lib/type-map.js` provides `mapScratchBirdTypeToTypeOrm`. Representative entries from source:

| ScratchBird type | TypeORM column type |
|---|---|
| BOOLEAN | boolean |
| SMALLINT | smallint |
| INTEGER / INT | int |
| BIGINT | bigint |
| REAL / FLOAT | float |
| DOUBLE PRECISION | double precision |
| NUMERIC | numeric |
| DECIMAL | decimal |
| CHAR | char |
| VARCHAR / CHARACTER VARYING | varchar |
| TEXT | text |
| DATE | date |
| TIME | time |
| TIMESTAMP | timestamp |
| TIMESTAMPTZ | timestamptz |
| UUID | uuid |
| JSON | json |
| JSONB | jsonb |
| BYTEA / BLOB | bytea |
| VECTOR | text (unsupported flag) |
| GEOMETRY | bytea (unsupported flag) |

Types not in the map default to `varchar` with `unsupported: true`. Array-suffix types (e.g. `TEXT[]`) are flagged as array variants.

For the full type mapping reference see [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- `normalizeTypeOrmOptions` — merges URL and explicit options, applies guardrails
- ScratchBird-to-TypeORM column type mapping (`mapScratchBirdTypeToTypeOrm`)
- Metadata-to-TypeORM entity schema generation (`lib/entity-schema.js` / `generateEntitySchemas`)
- Relation mapping (many-to-one, join column resolution) via entity schema generator
- Transaction command plan builder for nested CRUD (`buildNestedCrudTransactionPlan`) — plans request engine-side finality without taking finality locally
- Full canonical option alias normalization (manager-proxy, token/assertion, channel-binding, dormant-reattach)

**Not yet available / environment-gated (per source README):**

- Full live TypeORM `DataSource` session against a running ScratchBird server
- TypeORM migration (`synchronize`, `runMigrations`) — transaction plan builder provides contract scaffolding only
- Repository/EntityManager query lifecycle verification

All connection-level authority is delegated to the ScratchBird Node.js driver and revalidated server-side. The adaptor has `driver_local_authority: advisory_only`. Transaction finality is engine-owned (MGA); the adapter plans transaction commands but does not own commit/rollback.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/node.md](#ch-client-driver-guide-drivers-node-md) — ScratchBird Node.js driver (the underlying transport)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection parameter reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE and error mapping




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/dbt.md -->

<a id="ch-client-driver-guide-adaptors-dbt-md"></a>

# dbt Adaptor

> **Draft stub.** The source directory (`project/drivers/adaptor/scratchbird-dbt-adapter/`) contains only `package_contract.json` — no README, no Python source, no `pyproject.toml`, no examples. The information on this page is derived exclusively from `package_contract.json` and the corresponding row in `DriverPackageManifest.csv`.

The dbt adaptor (`scratchbird-dbt-adapter`) integrates ScratchBird into the dbt data transformation framework. It provides a dbt adapter plugin that compiles dbt relation and materialization requests against ScratchBird and delegates all wire transport to the ScratchBird Python driver (DB-API 2.0).

**Status:** beta_2 (release_candidate bucket).

**Conformance profile:** `adaptor_dbt_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-dbt-adapter` |
| driver\_package\_uuid | `019e12a0-0031-7000-8000-000000000031` |
| api\_surface | `application_adapter` |
| ingress\_mode | `direct_listener`, `manager_proxy` |
| delegates to / pooling | `delegates_to_python` (driver:python) |
| type\_mapping\_profile | `python_dbapi_mapping` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| package\_type | `python_wheel` |
| license | MPL-2.0 |

---

## Installation

The artifact is a Python wheel, built with:

```bash
python3 -m build
```

Install smoke: `python3 -m pytest tests`.

No further install instructions are available from source at this time. The dbt adapter plugin mechanism (`AdapterPlugin`) and the `profiles.yml` connection profile format are standard dbt adapter conventions; specific profile key names for ScratchBird are not documented in the available source.

---

## API surface (from package_contract.json)

The contract declares the following API surface areas:

- `ConnectionManager` — manages connections to ScratchBird via the Python driver
- `AdapterPlugin` — dbt adapter plugin registration
- `relation` — dbt relation object (schema-qualified table/view references)
- `column` — dbt column type representation
- `materialization_table` — dbt table materialization
- `materialization_view` — dbt view materialization
- `incremental_strategy` — incremental model strategy

Schema metadata uses authorization-filtered `sys.information` views (not raw `information_schema`), consistent with the ScratchBird `sys_information_recursive` metadata profile.

---

## Authority boundaries

Per the `delegation_posture` in `package_contract.json`:

- The adapter may compile dbt relation and materialization requests
- The Python driver handles transport and protocol binding
- The server revalidates authentication, authorization, SBLR, UUID, cache, schema, and transaction claims
- MGA transaction finality remains engine-owned

---

## Type and feature mapping

Type mapping follows the `python_dbapi_mapping` profile. See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Capabilities and limitations

Source coverage is thin (package_contract.json only). The following is known from the contract:

- Supported materialization strategies: table, view, incremental
- Catalog metadata maps to ScratchBird `sys.information` with authorization-filtered visibility
- Comparison baseline is `dbt-postgres`; ScratchBird-specific deltas cover relation, column, materialization, incremental strategy, and adapter response surfaces

Runtime details, `profiles.yml` configuration keys, and incremental strategy semantics are not yet documented from source.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/python.md](#ch-client-driver-guide-drivers-python-md) — ScratchBird Python driver (DB-API 2.0)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection parameter reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE and error mapping




===== FILE SEPARATION =====

<!-- chapter source: Client_Driver_Guide/adaptors/airbyte.md -->

<a id="ch-client-driver-guide-adaptors-airbyte-md"></a>

# Airbyte Adaptor

> **Draft stub.** The source directory (`project/drivers/adaptor/scratchbird-airbyte/`) contains only `package_contract.json` — no README, no Python source, no connector configuration files, no examples. The information on this page is derived exclusively from `package_contract.json` and the corresponding row in `DriverPackageManifest.csv`.

The Airbyte adaptor (`scratchbird-airbyte`) integrates ScratchBird into the Airbyte data integration platform as a connector. It implements the Airbyte Python CDK connector interface and delegates all wire transport to the ScratchBird Python driver (DB-API 2.0).

**Status:** beta_2 (release_candidate bucket).

**Conformance profile:** `adaptor_airbyte_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-airbyte` |
| driver\_package\_uuid | `019e12a0-0030-7000-8000-000000000030` |
| api\_surface | `application_adapter` |
| ingress\_mode | `direct_listener`, `manager_proxy` |
| delegates to / pooling | `delegates_to_python` (driver:python) |
| type\_mapping\_profile | `python_dbapi_mapping` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| package\_type | `airbyte_connector_bundle` |
| license | MPL-2.0 |

---

## Installation

The artifact is an Airbyte connector bundle, built with:

```bash
python3 project/drivers/scripts/driver_component_runner.py --component adaptor:scratchbird-airbyte
```

Install smoke: Airbyte `spec`/`check`/`discover`/`read` against the staged connector bundle.

No further install instructions are available from source at this time. Connector registration in an Airbyte workspace (self-hosted or cloud) follows standard Airbyte custom connector procedures; specific connector image or package names are not documented in the available source.

---

## API surface (from package_contract.json)

The contract declares the following Airbyte CDK operation surfaces:

| Operation | Description |
|---|---|
| `spec` | Returns the connector configuration schema |
| `check` | Tests connectivity and credential validity |
| `discover` | Returns the Airbyte catalog (available streams) |
| `read` | Reads records from ScratchBird |
| `incremental_state` | Manages incremental sync state checkpointing |
| `destination_write` | Writes records to ScratchBird as a destination |

Catalog metadata maps to ScratchBird `sys.information` views with authorization-filtered visibility, consistent with the `sys_information_recursive` metadata profile.

---

## Authority boundaries

Per the `delegation_posture` in `package_contract.json`:

- The adapter may shape Airbyte catalog and state messages
- The Python driver handles transport and protocol binding
- The server revalidates authentication, authorization, SBLR, UUID, cache, schema, and transaction claims
- MGA transaction finality remains engine-owned

---

## Type and feature mapping

Type mapping follows the `python_dbapi_mapping` profile. See [../type_mapping.md](#ch-client-driver-guide-type-mapping-md).

---

## Capabilities and limitations

Source coverage is thin (package_contract.json only). The following is known from the contract:

- Both source (read) and destination (write) connector modes are in scope
- Full refresh and incremental sync strategies are in scope
- Comparison baseline is `airbyte-postgres-source`; ScratchBird-specific deltas cover spec, check, discover, full-refresh read, incremental state, and destination write surfaces
- Stream catalog metadata is authorization-filtered via `sys.information`

Connector configuration schema keys, stream selection options, and incremental cursor/bookmark semantics are not yet documented from source.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md).

---

## See also

- [../README.md](#ch-client-driver-guide-readme-md) — Client and Driver Guide overview
- [../drivers/python.md](#ch-client-driver-guide-drivers-python-md) — ScratchBird Python driver (DB-API 2.0)
- [../connection_and_dsn.md](#ch-client-driver-guide-connection-and-dsn-md) — DSN and connection parameter reference
- [../authentication.md](#ch-client-driver-guide-authentication-md) — Authentication methods
- [../type_mapping.md](#ch-client-driver-guide-type-mapping-md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](#ch-client-driver-guide-diagnostics-and-sqlstate-md) — SQLSTATE and error mapping




# AI Integration Guide




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/README.md -->

<a id="ch-ai-integration-guide-readme-md"></a>

# AI Integration Guide

**DRAFT — Early Beta documentation. Subject to revision.**

This manual describes the ScratchBird AI integration layer: an MCP-oriented
service component that connects AI workflows to ScratchBird's native
parser/compiler and execution paths.

---

## What ScratchBird AI Is

ScratchBird AI is the AI integration layer for ScratchBird. It provides:

- An MCP-oriented service and tool registration surface.
- A compile/execute split orchestration path that routes SQL through the
  ScratchBird parser and compiler before engine submission.
- An HTTP adapter and local HTTP bridge for connecting AI clients to a running
  ScratchBird server.
- Safe-by-default policy controls with read-only defaults and approval-gated
  mutation paths.
- Governance helpers: durable approval ledger, audit bundle attestation,
  in-process quotas, rate limits, and cost attribution.
- Remote MCP session lifecycle with a broad set of advertised authentication
  families.
- A machine-readable dialect capability matrix.

Current release track: **early beta / technical Beta 1** (`0.1.0`).
Status timestamp: April 20, 2026.

---

## Native-Only Support Boundary

ScratchBird AI supports **ScratchBird native engine workflows only**.

- Non-native dialect requests are rejected with explicit policy errors.
- Emulated external engines (PostgreSQL, MySQL, Firebird emulation lanes) are
  out of scope for this component's AI layer.
- The ScratchBird engine execution boundary is `ServerSession`. SQL must be
  compiled to SBLR (ScratchBird Bytecode and Logical Representation) before it
  is submitted to the engine.

This boundary is enforced in the router, capability matrix, and compatibility
negotiation path. It is not configurable.

For a detailed explanation, see [overview_and_support_boundary.md](#ch-ai-integration-guide-overview-and-support-boundary-md).

---

## Manual Map

| Chapter | Contents |
| --- | --- |
| [overview_and_support_boundary.md](#ch-ai-integration-guide-overview-and-support-boundary-md) | What the layer does and explicitly does not do; native-only enforcement; engine boundary |
| [architecture_adapter_and_bridge.md](#ch-ai-integration-guide-architecture-adapter-and-bridge-md) | Adapter modes (mock/http/hybrid), local HTTP bridge, compile/execute split, transport and front-door modes |
| [mcp_tools_and_control_surface.md](#ch-ai-integration-guide-mcp-tools-and-control-surface-md) | Full MCP tool inventory organized by family; native control surface families |
| [governance_quotas_and_audit.md](#ch-ai-integration-guide-governance-quotas-and-audit-md) | Safe-by-default policy, approval-gated mutation, approval ledger, audit bundles, quotas, circuit breaker |
| [remote_mcp_and_authentication.md](#ch-ai-integration-guide-remote-mcp-and-authentication-md) | Remote MCP session lifecycle; advertised auth families; transport modes |
| [runtime_configuration.md](#ch-ai-integration-guide-runtime-configuration-md) | All adapter and bridge environment variables; runtime profile model; secret provider |
| [getting_started.md](#ch-ai-integration-guide-getting-started-md) | Install, validate, run bridge, run MCP stack — distilled from Ubuntu and Windows install guides |
| [release_status_and_known_gaps.md](#ch-ai-integration-guide-release-status-and-known-gaps-md) | Current status, known gaps, support matrix, conformance gates — dated early-beta framing |
| [troubleshooting.md](#ch-ai-integration-guide-troubleshooting-md) | Diagnostics for bridge boot failures, auth errors, compatibility failures, approval errors |

---

## Reading Model

Read this manual in chapter order for a first-time bring-up. For an operator
who already has the stack running:

1. [runtime_configuration.md](#ch-ai-integration-guide-runtime-configuration-md) for environment
   variable reference.
2. [governance_quotas_and_audit.md](#ch-ai-integration-guide-governance-quotas-and-audit-md) for
   approval and audit controls.
3. [troubleshooting.md](#ch-ai-integration-guide-troubleshooting-md) for live failure diagnosis.

---

## Related Manual Cross-Links

- Getting started with ScratchBird as a whole: see `../Getting_Started/`
- Operations and administration topics: see `../Operations_Administration/`
- SBsql language reference: see `../Language_Reference/`

---

## Source Authority

This manual was assembled from the following source documents under
`project/ai/`:

- `README.md` — support policy, current surface, environment variables, repo layout
- `docs/guides/GETTING_STARTED_EARLY_BETA.md`
- `docs/guides/INSTALL_UBUNTU_BETA1.md`
- `docs/guides/INSTALL_WINDOWS_BETA1.md`
- `docs/guides/RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`
- `docs/guides/LIVE_BRIDGE_TROUBLESHOOTING.md`
- `docs/releases/INITIAL_EARLY_BETA_RELEASE_2026-02-18.md`
- `docs/releases/EARLY_BETA_CONFORMANCE_GATES.md`
- `docs/releases/BETA1_SUPPORT_MATRIX_2026-04-18.md`
- `docs/releases/BETA1_REMAINING_LIVE_TASKS_2026-04-18.md`
- `docs/status/EARLY_BETA_STATUS_2026-03-07.md` (content refreshed 2026-04-20)
- `docs/status/EARLY_BETA_KNOWN_GAPS_2026-03-07.md` (content refreshed 2026-04-20)
- `capability/capability-matrix.v0.json` and `capability-matrix.schema.json`
- `src/scratchbird_ai/mcp_server.py`
- `src/scratchbird_ai/remote_sessions.py`
- `src/scratchbird_ai/scratchbird_core_surface.py`
- `src/scratchbird_ai/http_bridge.py`
- `examples/http-bridge.env.example`




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/overview_and_support_boundary.md -->

<a id="ch-ai-integration-guide-overview-and-support-boundary-md"></a>

# Overview and Support Boundary

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter explains what the ScratchBird AI integration layer does, what it
explicitly does not do, and where the native-only boundary is drawn and
enforced.

---

## What the Layer Does

ScratchBird AI is the AI integration layer for the ScratchBird Convergent Data
Engine (CDE). It provides a managed path between AI clients and the ScratchBird
native engine without exposing raw database connections or bypassing
ScratchBird's compile/execute contract.

Key responsibilities:

- **MCP-oriented orchestration.** The layer exposes a set of canonical tools
  through the Model Context Protocol (MCP) surface. AI clients discover
  capabilities through the tool registry and invoke tools rather than issuing
  SQL directly.

- **Compile/execute split.** Query text is compiled to a ScratchBird artifact
  identifier before execution. The compile step validates syntax and produces a
  traceable artifact; the execute step runs the pre-compiled form. These are
  separate tool invocations.

- **Safe-by-default policy.** Read operations are available through the
  `execute_readonly_query` and `run_query` tool paths without additional
  approval. Write/mutation operations require an explicit approval token and
  pass through the durable approval ledger.

- **Dialect capability matrix.** The layer publishes a machine-readable matrix
  of supported dialects and their per-capability flags. As of the current
  release, only the `native` dialect appears in the matrix.

- **HTTP adapter and local bridge.** An HTTP adapter layer provides
  `mock`, `http`, and `hybrid` operation modes. In `http` and `hybrid` modes,
  a local HTTP bridge process forwards compile, execute, and metadata requests
  to a real ScratchBird server.

- **Governance helpers.** Durable approval ledger, audit bundle generation and
  replay, attestation (HMAC or external reference), in-process quotas and
  rate limits, cost attribution, and HTTP retry/circuit-breaker behavior.

- **Remote MCP sessions.** A session lifecycle for remote MCP clients with
  token negotiation, expiry, and a broad set of supported authentication
  families.

---

## What the Layer Does Not Do

The following are explicitly out of scope for this component:

- **Non-native dialect AI support.** The PostgreSQL, MySQL, and Firebird
  emulation lanes may be present in a ScratchBird deployment profile, but AI
  support for those lanes is not part of this component. Non-native dialect
  requests are rejected with explicit policy errors.

- **Direct raw SQL execution.** Queries pass through the compile/execute split.
  The layer does not provide a raw SQL pass-through path that bypasses
  compilation.

- **Production-grade authorization depth.** Fine-grained authorization and
  hard multi-tenant isolation are not complete in the current early-beta
  surface. The layer has token-based auth and a local approval ledger, but
  not a full enterprise IAM integration.

- **Third-party signing infrastructure.** The audit attestation path supports
  HMAC-SHA256 and external-reference modes. A PKI-backed or externally
  notarized signing infrastructure is not shipped in this release.

- **Automatic live certification for runtime modes not currently exposed.** The
  `manager_proxy`, `local_ipc`, and `embedded_local_only` runtime modes are
  admitted in ScratchBird core and implemented here, but live certification
  evidence for those modes depends on the test environment exposing them. The
  primary certified path is `listener_direct`.

- **Packaged public installer.** The current release ships source-first Beta 1
  instructions, not a packaged installer.

---

## The Native-Only Support Boundary

The native-only boundary is enforced in three places:

1. **Router.** The service router rejects requests that arrive with a
   non-native dialect identifier.

2. **Capability matrix.** The dialect capability matrix schema (`capability-matrix.schema.json`)
   constrains `propertyNames` to `["native"]`. A matrix with any other dialect
   key will not validate.

3. **Compatibility negotiation.** The compatibility negotiation path fails
   closed for any declared server, parser, or driver/runtime version that is
   outside the configured supported window.

The enforcement is not advisory: rejected requests return explicit policy error
codes, not silently degraded results.

---

## The Engine Boundary

The ScratchBird engine execution boundary is `ServerSession`. SQL must be
compiled to SBLR (ScratchBird Bytecode and Logical Representation) before it
is submitted to the engine. The AI layer enforces this by separating the
`compile_query` step from the `execute_compiled` step. The compile step
produces a `compile_artifact_id`; the execute step accepts that identifier,
not raw query text.

Practical implications:

- Query text that cannot be compiled is rejected at the compile step with a
  structured error. Bounded compile-repair can strip common wrapper noise
  (markdown code fences, `sql:` / `query:` prefixes), but it does not rewrite
  semantics.

- The engine's own retrieval metadata is discoverable through the
  `opensearch_meta.*` catalog namespace, which the AI layer treats as a
  first-class introspection surface.

- The runtime mode used for a given connection (`listener_direct`,
  `manager_proxy`, `local_ipc`, `embedded_local_only`) determines the
  transport path. The AI layer maps the `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP`
  setting to the appropriate transport family.

---

## Capability Matrix (Current State)

The capability matrix at `capability/capability-matrix.v0.json` is the
machine-readable source of truth for dialect capabilities. As of version
`2026-04-20.1`:

| Dialect | Status | read_select | write_dml | ddl | transactions | prepare_bind | metadata_introspection | vector_ops | graph_ops |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `native` | baseline | true | true | true | true | true | true | true | true |

The `status` values the schema admits are: `unavailable`, `experimental`,
`partial`, `baseline`, `full`. The `native` dialect is at `baseline` as of
the current release.

The matrix is validated by `tools/validate_capability_matrix.py` as part of
the standard local validation sequence.




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/architecture_adapter_and_bridge.md -->

<a id="ch-ai-integration-guide-architecture-adapter-and-bridge-md"></a>

# Architecture: Adapter and Bridge

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter describes how the ScratchBird AI layer connects to a running
ScratchBird server. It covers the three adapter modes, the local HTTP bridge,
the compile/execute split, and the transport/front-door/IPC mode options.

---

## Layered Architecture Overview

```
AI Client (MCP tool calls)
        |
        v
  MCP Server  (scratchbird_ai.mcp_server)
        |
        v
  AI Service  (scratchbird_ai.service / ScratchBirdAIService)
        |
        v
  Adapter Layer  (scratchbird_ai.adapters)
        |
     [mode]
   mock | http | hybrid
        |
        v (http / hybrid only)
  Local HTTP Bridge  (scratchbird_ai.http_bridge)
  port 3095 (default)
        |
        v
  ScratchBird Server
  (native listener, manager proxy, IPC, or embedded)
```

The adapter layer is the seam between the service and the ScratchBird server.
Which adapter mode is active determines whether requests go to a real server,
a stub, or a combination.

---

## Adapter Modes

The adapter mode is set by `SCRATCHBIRD_AI_ADAPTER_MODE`. Three values are
supported:

| Mode | Behavior |
| --- | --- |
| `mock` | All adapter calls return deterministic stub responses. No bridge or real server is needed. This is the default. Used for local testing and development without a live ScratchBird target. |
| `http` | All adapter calls are forwarded to the local HTTP bridge over `SCRATCHBIRD_AI_HTTP_BASE_URL`. A running bridge is required. Used for live integration and certification runs. |
| `hybrid` | Dialects listed in `SCRATCHBIRD_AI_HTTP_DIALECTS` are forwarded to the bridge; others fall back to mock. The default dialect CSV is `native`. |

The default is `mock`. Enabling `http` or `hybrid` requires a running bridge
instance (see below).

---

## The Local HTTP Bridge

The local HTTP bridge (`scratchbird_ai.http_bridge`) is a lightweight HTTP
server that exposes compile, execute, and metadata endpoints that the adapter
calls. It holds the actual ScratchBird Python driver connection and handles the
transport-mode details.

**Default bind address and port:**

- Host: `127.0.0.1` (controlled by `SCRATCHBIRD_AI_BRIDGE_HOST`)
- Port: `3095` (controlled by `SCRATCHBIRD_AI_BRIDGE_PORT`)

**Bridge endpoints exposed:**

- compile
- execute
- metadata (schemas, tables, describe)
- health/dialect checks

**Bridge auth:** The bridge optionally requires a Bearer token set by
`SCRATCHBIRD_AI_BRIDGE_API_TOKEN`. The adapter presents the matching token via
`SCRATCHBIRD_AI_HTTP_API_TOKEN`.

**Starting the bridge:**

```bash
# POSIX (Ubuntu/Linux)
PYTHONPATH=src tools/run_local_bridge.sh

# Direct Python module
python3 -m scratchbird_ai.http_bridge

# Windows PowerShell
python -m scratchbird_ai.http_bridge
# or: scratchbird-ai-http-bridge (if installed as console script)
```

---

## The Compile/Execute Split

The AI layer enforces a two-step contract for query execution:

1. **Compile** — `compile_query(dialect, query_text)` submits query text to
   the ScratchBird parser/compiler and returns a `compile_artifact_id`.
   The artifact identifier is stable and traceable.

2. **Execute** — `execute_compiled(compile_artifact_id, options, mode)` runs
   the pre-compiled form. It does not accept raw query text.

This split means:

- The compiler validates syntax before any execution attempt.
- Compiled artifacts carry trace IDs and feed into audit bundles.
- The execution mode (`ai_analysis` by default) can be changed at the execute
  step without recompiling.
- Bounded compile-repair can attempt to strip common wrapper noise from query
  text before compilation (see [governance_quotas_and_audit.md](#ch-ai-integration-guide-governance-quotas-and-audit-md)).

The `run_query` and `execute_readonly_query` tools combine compile and execute
in one call for convenience, but they still go through the same split
internally.

---

## Server Setup Profiles

The `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` variable selects the ScratchBird
server connection mode the bridge will use. Valid values:

| Value | Maps to | Description |
| --- | --- | --- |
| `listener-only` | `listener_direct` transport | Direct TCP connection to the native ScratchBird listener. Default and primary Beta 1 certified path. |
| `managed` | `manager_proxy` transport | Connection through the ScratchBird manager proxy. Requires `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN`. |
| `ipc-only` | `local_ipc` transport | Local IPC socket/pipe connection. Requires the Python driver to support IPC transport. |
| `embedded` | `embedded_local_only` transport | In-process embedded mode. Single-connection; non-shared. Requires driver/runtime support. |

Several aliases are accepted (for example, `listener`, `tcp`, `inet_listener`
all map to `listener-only`; `manager_proxy`, `mcp` map to `managed`).

**For the current Beta 1, use `listener-only` as the primary path.** The
`managed`, `ipc-only`, and `embedded` modes are admitted in ScratchBird core
and implemented, but live certification evidence for those modes is still
environment-dependent.

---

## Transport, Front-Door, and IPC Overrides

For advanced deployments, individual aspects of the connection can be overridden
independently of the server setup profile:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_TRANSPORT_MODE` | Explicit transport override: `inet_listener`, `managed`, `local_ipc`, `embedded` |
| `SCRATCHBIRD_AI_BRIDGE_FRONT_DOOR_MODE` | Explicit front-door override: `direct` or `manager_proxy` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_METHOD` | IPC method override: `auto`, `unix`, `pipe`, `tcp` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_PATH` | IPC socket or pipe path for `ipc-only` mode |

These overrides are intended for environment-specific deployment needs. For
most installations, setting `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` is sufficient.

**Note:** `ipc-only` and `embedded` require a Python driver/runtime that
supports those transport modes. Do not configure them until the target
environment and driver are confirmed to support them.

---

## DSN Configuration

The bridge uses a DSN (Data Source Name) to locate the ScratchBird server:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` | Fallback DSN for all enabled dialects |
| `SCRATCHBIRD_AI_BRIDGE_DSN_<DIALECT>` | Per-dialect DSN override (for example, `SCRATCHBIRD_AI_BRIDGE_DSN_NATIVE`) |

The DSN format used in the shipped example is:

```
scratchbird://user:password@127.0.0.1:3092/mydb
```

Endpoint state (host, port, database, user) is deployment-specific and must
be supplied via the configured profile or environment. These values are never
committed in the repository.

---

## Dialect Filtering

The bridge only serves dialects listed in `SCRATCHBIRD_AI_BRIDGE_DIALECTS`
(default `native`). Requests for a dialect not in the list return
`404 Dialect not enabled`. The adapter side mirrors this with
`SCRATCHBIRD_AI_HTTP_DIALECTS`.

For the current release, include `native` in both variables.

---

## Strict Compile Mode

When `SCRATCHBIRD_AI_BRIDGE_STRICT_COMPILE=1`, a compile probe failure at the
bridge endpoint returns HTTP 400 instead of a warning. In the default `0` state,
compile probe failures are logged but do not abort the bridge startup. Use
strict mode in environments where a failed compile probe should be treated as
a hard error.




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/mcp_tools_and_control_surface.md -->

<a id="ch-ai-integration-guide-mcp-tools-and-control-surface-md"></a>

# MCP Tools and Control Surface

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter lists the MCP tools exposed by the ScratchBird AI layer and
describes the native control surface families. All tools are registered through
`scratchbird_ai.mcp_server` using the `FastMCP` server scaffold under the
server name `scratchbird-ai`.

---

## How Tools Are Invoked

Tools are registered with the MCP runtime at startup. An AI client connects to
the MCP server and issues tool calls by name. Each tool call goes through an
error envelope path: if the underlying service raises an exception, the tool
returns a structured error object rather than propagating a raw exception.

The MCP server is installed by including the `mcp` optional dependency:

```
pip install -e ".[mcp]"
```

Starting the server:

```bash
# POSIX
PYTHONPATH=src tools/run_local_stack.sh

# Direct
python3 -m scratchbird_ai.mcp_server

# Windows console script (if installed)
scratchbird-ai-mcp
```

---

## Tool Inventory by Family

### Discovery and Compatibility

| Tool | Purpose |
| --- | --- |
| `get_capabilities` | Return the current capability matrix and supported dialect list |
| `get_tool_descriptors` | Return canonical tool declarations for all registered tools |
| `get_provider_profiles` | Return the direct provider tool-calling compatibility profiles |
| `get_compatibility_manifest` | Return the current compatibility manifest including version pins |
| `export_certification_manifest` | Export the release certification manifest |
| `negotiate_compatibility` | Fail-closed compatibility negotiation against a declared server/parser/driver context |
| `list_dialects` | List currently enabled dialects |

### Schema and Metadata Introspection

| Tool | Purpose |
| --- | --- |
| `list_schemas` | List schemas for a dialect and optional database |
| `list_tables` | List tables in a schema |
| `describe_table` | Describe a table's columns and types |

### Query Compilation and Execution

| Tool | Purpose |
| --- | --- |
| `compile_query` | Compile query text for a dialect; returns a `compile_artifact_id` |
| `execute_compiled` | Execute a previously compiled artifact by ID |
| `execute_readonly_query` | Compile and execute a read-only query in one call; requires `security_context` |
| `execute_mutation` | Compile and execute a mutation; requires `security_context` and `approval_evidence` |
| `run_query` | Convenience combine of compile + execute; accepts an `approval_token` for mutations |
| `run_mutation` | Mutation convenience wrapper; requires an `approval_token` |
| `explain_query` | Return the execution plan for a query without running it |

### Vector and Hybrid Retrieval

| Tool | Purpose |
| --- | --- |
| `create_vector_index` | Create a new vector index |
| `list_vector_indexes` | List vector indexes |
| `describe_vector_index` | Describe a vector index |
| `add_embeddings` | Add pre-computed embeddings to an index |
| `add_generated_embeddings` | Add embeddings that the tool generates from a provider config |
| `delete_embeddings` | Delete embeddings by vector ID |
| `reindex_vector_index` | Reindex a vector index |
| `delete_vector_index` | Delete a vector index |
| `vector_search` | K-nearest-neighbor vector search |
| `hybrid_search` | Combined vector + SQL hybrid search with configurable weights |

### Audit and Governance

| Tool | Purpose |
| --- | --- |
| `replay_audit_bundle` | Replay an audit bundle and verify tamper or policy mismatch |
| `list_audit_bundles` | List stored audit bundles |
| `validate_approval_evidence` | Validate an approval evidence object against the ledger |
| `list_approval_records` | List approval records from the durable ledger |
| `revoke_approval_record` | Administratively revoke an approval record |
| `create_audit_attestation` | Create an audit attestation (HMAC or external reference) |
| `verify_audit_attestation` | Verify a previously created attestation |

### Runtime Diagnostics and Operations

| Tool | Purpose |
| --- | --- |
| `get_runtime_diagnostics` | Return event summary counts, error rates, latency, and recent failures |
| `generate_operator_runbook_bundle` | Write a runbook bundle to the operator bundle output directory |

### Remote MCP Session Lifecycle

| Tool | Purpose |
| --- | --- |
| `open_remote_session` | Open a remote MCP session with auth negotiation |
| `invoke_remote_tool` | Invoke a tool within an established remote session |
| `close_remote_session` | Close a remote session |
| `poll_remote_operation` | Poll for the result of an async/streaming remote operation |
| `cancel_remote_operation` | Cancel an in-progress remote operation |

### Registry and Routing

| Tool | Purpose |
| --- | --- |
| `get_server_registry` | Return the current server registry |
| `register_remote_server` | Register a remote server in the registry |
| `update_remote_server_lifecycle` | Change the lifecycle state of a registered server |
| `report_remote_server_health` | Submit a health report for a registered server |
| `resolve_gateway_route` | Resolve the gateway route for a tool name and interface profile |

---

## Native Control Surface Families

The ScratchBird AI layer publishes a bounded, machine-readable native control
surface packet (`scratchbird_core_surface.py`). The packet is versioned at
`2026-04-18` and describes the following families:

### Graph Operations

Graph operation capability is declared in the `native` dialect capability
matrix (`graph_ops: true`). Graph tools operate over the native ScratchBird
execution path.

### Remote MCP

The remote MCP family covers the session lifecycle tools
(`open_remote_session`, `invoke_remote_tool`, `close_remote_session`,
`poll_remote_operation`, `cancel_remote_operation`). The default interface
profile is `mcp_remote_v0`. For details on authentication families, see
[remote_mcp_and_authentication.md](#ch-ai-integration-guide-remote-mcp-and-authentication-md).

### Registry and Routing Controls

The registry/routing family covers server registration, lifecycle management,
health reporting, and gateway route resolution. These tools allow AI
orchestration layers to track and route to multiple ScratchBird AI service
instances.

### Bridge and Runtime Management

The bridge and runtime management family covers the adapter mode, compatibility
negotiation, certification manifest export, and runtime diagnostics. These are
the tools an operator uses to inspect and manage the state of the AI layer
itself.

---

## Engine-Owned Retrieval Families

The ScratchBird core surface packet declares three engine-owned retrieval
families:

| Family ID | Semantic contract | Support state |
| --- | --- | --- |
| `vector_distance` | Vector similarity retrieval | implemented |
| `ann_hnsw` | k-NN/ANN retrieval | implemented |
| `full_text_inverted` | Full-text retrieval | implemented |

Retrieval metadata discovery is available through the `opensearch_meta.*`
catalog namespace with these relations:

| Relation | Support state |
| --- | --- |
| `opensearch_meta.index_metadata` | implemented |
| `opensearch_meta.mapping_fields` | implemented |
| `opensearch_meta.analyzer_settings` | implemented |
| `opensearch_meta.knn_index_metadata` | implemented |
| `opensearch_meta.aliases` | implemented |

---

## Framework Adapter Compatibility

The service layer also supports framework adapter wrappers. These are not MCP
tools but allow AI frameworks to interact with the service through familiar
interfaces:

| Framework | Profile ID | Status |
| --- | --- | --- |
| LangChain | `langchain_v0` | Supported |
| LlamaIndex | `llamaindex_v0` | Supported |
| Semantic Kernel | `semantic_kernel_v0` | Supported |

Provider tool-calling normalization is available through the
`provider_tool_calling_v0` profile, which normalizes OpenAI-style,
Anthropic-style, and Gemini-style provider payloads to the same canonical
execution results.




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/governance_quotas_and_audit.md -->

<a id="ch-ai-integration-guide-governance-quotas-and-audit-md"></a>

# Governance, Quotas, and Audit

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter describes the governance controls that are implemented in the
current early-beta ScratchBird AI component. These controls are real and
active. They do not imply third-party approval workflow orchestration,
PKI-backed signing infrastructure, or automatic certification for runtime
modes that are not currently exposed.

Topics covered:

- Safe-by-default read-only mode versus approval-gated mutation
- Bounded compile-repair
- Durable approval evidence ledger
- Audit bundles and attestation
- In-process quotas, rate limits, and cost attribution
- HTTP retry and circuit-breaker behavior
- Fail-closed compatibility pinning

---

## Safe-by-Default and Approval-Gated Mutation

The layer operates in read-only mode by default. Read operations (`run_query`,
`execute_readonly_query`, `explain_query`, retrieval tools) proceed without an
approval token.

Mutation operations (`execute_mutation`, `run_mutation`) require:

1. An `approval_token` or `approval_evidence` object.
2. The token to be valid in the durable approval ledger: not expired, not
   revoked, matching the correct tenant, actor, and statement hash.

If these conditions are not met, the mutation call returns `E_APPROVAL_INVALID`
and does not execute.

---

## Bounded Compile-Repair

Environment variable: `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS`

Before giving up on a compile failure, the layer can attempt a bounded set of
deterministic repairs on query text:

1. Original query text (no repair)
2. Trimmed whitespace
3. Stripped markdown code fence
4. Stripped leading `sql:` / `query:` label
5. Stripped markdown code fence plus leading label

Compile-repair is for recoverable wrapper noise only. It does not rewrite
query semantics. If the query itself is malformed, repair will not help and
the compile failure is returned to the caller.

---

## Durable Approval Evidence Ledger

Environment variable: `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`

Default path: `${SCRATCHBIRD_AI_RUNTIME_ROOT}/approval_ledger.json`, or the
platform user state runtime directory when `SCRATCHBIRD_AI_RUNTIME_ROOT` is
not set.

The approval ledger is a file-backed, append-only store of approval records.
Each approval record contains:

| Field | Purpose |
| --- | --- |
| `approval_id` | Stable, deterministic record identifier |
| `approval_token_hash` | Hash of the approval token |
| `tenant_id` | Tenant scope for the approval |
| `actor_id` | Identity of the approving actor |
| `statement_hash` | Hash of the approved statement |
| `approved_by` | Identity of the approver |
| `approved_at` | Approval timestamp |
| `expires_at` | Expiry timestamp |
| `revoked_at` | Revocation timestamp (null if not revoked) |
| `revoked_by` | Revoking identity (null if not revoked) |
| `revocation_reason` | Free-text revocation reason |
| `last_used_at` | Timestamp of most recent valid use |
| `use_count` | Count of successful reuse events |

Reuse validation checks all of: approval token hash, tenant identity, actor
identity, and statement hash. Expired or revoked approvals fail closed.
Successful reuse updates `last_used_at` and `use_count`.

Operator tools for the approval ledger:

| Tool | Purpose |
| --- | --- |
| `list_approval_records` | List records, filterable by tenant and actor |
| `revoke_approval_record` | Administratively revoke a record by ID |
| `validate_approval_evidence` | Validate an evidence object without executing |

---

## Structured Event Logging and Runbook Packaging

Environment variables:

- `SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH`
- `SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR`

Default paths: `${SCRATCHBIRD_AI_RUNTIME_ROOT}/structured_events.jsonl` and
`${SCRATCHBIRD_AI_RUNTIME_ROOT}/operator_bundle/` respectively.

Tool invocations and direct query execution outcomes emit structured JSONL
events to the event log. Event summaries include counts, error rates, latency
statistics, and recent failures.

Operator runbook bundles write the following files to the bundle output
directory:

| File | Contents |
| --- | --- |
| `summary.json` | High-level summary of current state |
| `runtime_diagnostics.json` | Detailed runtime diagnostics |
| `slo_report.json` | SLO/error budget report |
| `recent_errors.json` | Recent error events |
| `approval_summary.json` | Approval ledger summary |
| `certification_manifest.json` | Current certification manifest |

Use `generate_operator_runbook_bundle` to produce a bundle, and
`get_runtime_diagnostics` for an immediate in-memory summary.

---

## Audit Bundles and Attestation

Environment variables:

- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE` — `hmac_sha256` or `external_reference`
- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET` — shared secret for HMAC mode
- `SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID` — attestor identity for the attestation record

Audit bundles are deterministic, replayable records of a tool invocation's
policy decision, plan hash, execution result, and security context. Key
properties:

- Equivalent audit bundles hash identically.
- Replay detects tamper (payload modification) and policy mismatch
  (re-evaluation against the current policy produces a different decision).

Attestation modes:

| Mode | Behavior |
| --- | --- |
| `hmac_sha256` | A HMAC-SHA256 over the bundle payload using the configured shared secret. Default when a shared secret is configured. |
| `external_reference` | An external governance system owns the final attestation record. The field carries an external URI or identifier. |

Audit tools:

| Tool | Purpose |
| --- | --- |
| `list_audit_bundles` | List stored bundles |
| `replay_audit_bundle` | Replay a bundle; verify expected policy decision, rule ID, and plan hash |
| `create_audit_attestation` | Issue a new attestation for a bundle |
| `verify_audit_attestation` | Verify a previously issued attestation |

---

## In-Process Quotas, Rate Limits, and Cost Attribution

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` | Rolling window size in seconds |
| `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW` | Maximum requests per window |
| `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW` | Maximum mutation operations per window |
| `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW` | Maximum cost units per window |

Enforcement is per `tenant_id` and per `actor_id` within each active time
window.

When a budget is exceeded, the operation returns `E_LIMIT_EXCEEDED` with a
policy rule ID:

| Limit | Error code | Rule ID |
| --- | --- | --- |
| Request budget | `E_LIMIT_EXCEEDED` | `OPS-RATE-001` |
| Mutation budget | `E_LIMIT_EXCEEDED` | `OPS-MUTATION-001` |
| Cost budget | `E_LIMIT_EXCEEDED` | `OPS-COST-001` |

Cost is estimated from the bounded request options plus a mutation uplift. The
cost model is intentionally conservative in the current release; it is not a
production billing system.

---

## HTTP Retry and Circuit Breaker

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC` | Timeout for HTTP adapter requests |
| `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS` | Number of retry attempts for retryable requests |
| `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS` | Backoff between retries in milliseconds |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD` | Consecutive failures before the circuit opens |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC` | Cooldown before the circuit closes again |

Retry policy:

- Retries apply only to retryable bridge requests.
- Retryable requests: `GET` and compile endpoints.
- Execute and mutation endpoints are not retried automatically.

Circuit-breaker policy:

- Consecutive failures open the circuit.
- The circuit remains open until the cooldown period elapses.
- Open-circuit requests fail closed immediately instead of sending further
  requests to the bridge.

---

## Fail-Closed Compatibility Pinning

Environment variables:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS` | Accepted ScratchBird server version strings |
| `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS` | Accepted parser/compiler version strings |
| `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS` | Accepted local/bridge driver runtime version strings |

When these variables are set, the compatibility negotiation path rejects any
declared version that is outside the configured set. Unknown interface profiles
and unsupported transports also fail closed.

Error codes:

| Condition | Error code |
| --- | --- |
| Unsupported server version | `E_SERVER_RUNTIME_UNSUPPORTED` |
| Unsupported component version | `E_COMPONENT_VERSION_UNSUPPORTED` |
| Unsupported driver runtime | `E_DRIVER_RUNTIME_UNSUPPORTED` |
| Unsupported interface profile | `E_INTERFACE_PROFILE_UNSUPPORTED` |
| Unsupported transport | `E_TRANSPORT_PROFILE_UNSUPPORTED` |

Use these pins when you want the AI layer to reject environments outside the
validated release window rather than silently accepting them. Do not suppress
these checks unless the environment is genuinely inside the supported window.

---

## What These Controls Do Not Provide

- Third-party approval workflow orchestration.
- PKI-backed or externally notarized signing infrastructure. The shipped
  attestation path uses HMAC-SHA256 or an external reference pointer.
- Automatic certification for runtime modes that are not currently exposed in
  the active test environment.
- A production billing or metering system. Cost attribution is an in-process
  estimate for governance guardrails, not an external billing record.




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/remote_mcp_and_authentication.md -->

<a id="ch-ai-integration-guide-remote-mcp-and-authentication-md"></a>

# Remote MCP and Authentication

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter describes the remote MCP session lifecycle and the authentication
families that the ScratchBird AI layer advertises for remote clients. It also
covers the transport modes available for remote sessions.

---

## Remote MCP Session Lifecycle

A remote MCP session is an authenticated, time-bounded connection between a
remote AI client and the ScratchBird AI service. The session lifecycle has four
phases:

1. **Open** — `open_remote_session` accepts an authentication envelope and
   negotiates the protocol version and transport. On success it returns a
   `session_id`, `session_expires_at`, and `heartbeat_interval_sec`.

2. **Use** — `invoke_remote_tool` submits tool calls within the session. Long-
   running or streaming operations return an `operation_id` that can be polled
   with `poll_remote_operation`. In-progress operations can be cancelled with
   `cancel_remote_operation`.

3. **Poll/Cancel** — `poll_remote_operation` accepts an optional
   `continuation_token` for paged streaming results. `cancel_remote_operation`
   accepts a `reason` and terminates the operation.

4. **Close** — `close_remote_session` terminates the session. Calling close on
   an already-closed session returns `status: already_closed` without error.

Sessions expire automatically at `session_expires_at` (default TTL: 900
seconds). The default heartbeat interval is 30 seconds. Expired sessions
return `E_SESSION_REQUIRED` with policy rule ID `REMOTE-SESSION-006`.

---

## Interface Profile

The only currently supported remote interface profile is `mcp_remote_v0`.
Requests for any other profile return:

```
E_INTERFACE_PROFILE_UNSUPPORTED / REMOTE-SESSION-001
```

The default is applied automatically when `interface_profile_id` is omitted.

---

## Protocol Version

The current supported remote protocol version is `v0`. Other values return:

```
E_COMPONENT_VERSION_UNSUPPORTED / REMOTE-SESSION-002
```

---

## Transport Modes

The following transport modes are advertised as supported for remote sessions:

| Transport | Description |
| --- | --- |
| `https_json_request_response` | Standard HTTPS request-response (default) |
| `https_sse_server_stream` | HTTPS with server-sent events for streaming responses |
| `websocket_bidirectional` | WebSocket bidirectional transport |

Requesting an unsupported transport returns:

```
E_TRANSPORT_PROFILE_UNSUPPORTED / REMOTE-SESSION-003
```

---

## Authentication Families

The `open_remote_session` call requires an `auth_envelope` object. The
`RemoteSessionManager` advertises the following `supported_auth_types`:

| Auth type | Description |
| --- | --- |
| `bearer` | Static bearer token. Validated against `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` when that variable is set. Inferred automatically if `token`, `access_token`, or `jwt` fields are present without an explicit `auth_type`. |
| `oauth2_access_token` | OAuth2 access token. Validated against the bridge token when set. |
| `jwt_bearer` | JWT bearer token. Validated against the bridge token when set. |
| `workload_identity` | Workload identity credential (e.g., cloud service account). Provide `workload_identity` field in the auth envelope. |
| `proxy_principal` | A principal forwarded by a trusted proxy. Provide `proxy_principal` or `principal` field. |
| `ldap_bind` | LDAP bind credential. Provide `bind_dn` field in the auth envelope. |
| `kerberos_gssapi` | Kerberos/GSSAPI credential. Provide `kerberos_principal` field. |
| `radius_pap` | RADIUS PAP credential. Provide `radius_username` field. |
| `pam_conversation` | PAM conversation credential. Provide `pam_service` field. |
| `preauthenticated_context` | The caller asserts pre-authentication; provide `security_context` in the envelope. Inferred automatically when no token fields are present. |

**Token validation note:** When `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` is
configured, auth types `bearer`, `oauth2_access_token`, and `jwt_bearer` are
validated against that token value. Other auth types are admitted based on the
presence of the expected envelope field, not against a server-side credential
store in the current early-beta surface.

**Auth material requirements:** Each auth type requires a specific envelope
field to be non-empty. If the required field is missing, the call returns
`E_POLICY_DENY / REMOTE-AUTH-004`. The `preauthenticated_context` type
requires a `security_context` object.

---

## Auth Envelope Format

The auth envelope is a JSON object passed as the `auth_envelope` argument to
`open_remote_session`. Minimal examples:

```json
{
  "auth_type": "bearer",
  "token": "<your-token>",
  "security_context": { "tenant_id": "t1", "actor_id": "user1" }
}
```

```json
{
  "auth_type": "preauthenticated_context",
  "security_context": { "tenant_id": "t1", "actor_id": "user1" }
}
```

```json
{
  "auth_type": "ldap_bind",
  "bind_dn": "cn=user,dc=example,dc=com",
  "security_context": { "tenant_id": "t1", "actor_id": "user1" }
}
```

The `security_context` object within the envelope is used to populate the
session's security context for downstream policy and audit checks. It should
include at minimum `tenant_id` and `actor_id`.

---

## Security Context Propagation

The `security_context` derived from the auth envelope is carried through the
session and used for:

- Approval record matching (tenant and actor identity)
- Audit bundle subject fields
- Operational control enforcement (quota per tenant and actor)
- Registry and routing security checks

---

## Error Codes Reference

| Error code | Policy rule | Condition |
| --- | --- | --- |
| `E_INTERFACE_PROFILE_UNSUPPORTED` | `REMOTE-SESSION-001` | Unsupported interface profile requested |
| `E_COMPONENT_VERSION_UNSUPPORTED` | `REMOTE-SESSION-002` | Unsupported protocol version requested |
| `E_TRANSPORT_PROFILE_UNSUPPORTED` | `REMOTE-SESSION-003` | Unsupported transport requested |
| `E_TOOL_INPUT_INVALID` | `REMOTE-SESSION-004` | `client_capabilities` is not an object |
| `E_SESSION_REQUIRED` | `REMOTE-SESSION-005` | Unknown session ID |
| `E_SESSION_REQUIRED` | `REMOTE-SESSION-006` | Expired session ID |
| `E_POLICY_DENY` | `REMOTE-AUTH-001` | `auth_envelope` is not an object |
| `E_PROVIDER_CONTRACT_UNSUPPORTED` | `REMOTE-AUTH-002` | Unsupported auth type |
| `E_POLICY_DENY` | `REMOTE-AUTH-003` | Token validation failure (bearer/oauth2/jwt) |
| `E_POLICY_DENY` | `REMOTE-AUTH-004` | Missing auth material for declared auth type |




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/runtime_configuration.md -->

<a id="ch-ai-integration-guide-runtime-configuration-md"></a>

# Runtime Configuration

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter is the complete reference for environment variables that configure
the ScratchBird AI adapter, bridge, and governance controls. It also describes
the runtime profile model and how secrets are supplied.

The canonical source for bridge variable names is
`project/ai/examples/http-bridge.env.example`.

---

## Runtime Profile Model

Connection metadata is policy-supplied at runtime, not hardcoded in the
repository. Two environment variables point to the active profile:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH` | Path to the runtime environment file (`runtime.env`) that describes the active ScratchBird server and its published listener addresses |
| `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH` | Path to a `connections.json` profile file that maps profile names to connection parameters |

Endpoint state (host, port, database, user, secrets) is read from the
configured profile. These values are deployment-specific and must never be
committed in repository files.

Native ScratchBird lanes are in scope. Reference emulation lanes
(PostgreSQL/MySQL/Firebird) may appear in a deployment profile, but AI support
for non-native modes remains out of scope for this component.

Secrets are supplied by the configured secret provider or a test fixture. The
secret reference format used in direct native client probes is
`--password-ref <secret-ref>`.

---

## Adapter Environment Variables

These variables configure the adapter layer that sits between the AI service
and the bridge (or mock):

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_ADAPTER_MODE` | `mock` | Adapter operation mode: `mock`, `http`, or `hybrid` |
| `SCRATCHBIRD_AI_HTTP_BASE_URL` | _(none)_ | Base URL for HTTP adapter calls (required for `http` and `hybrid` modes) |
| `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC` | _(unset)_ | Timeout in seconds for HTTP adapter requests |
| `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS` | _(unset)_ | Retry count for retryable bridge requests (`GET` and `compile`) |
| `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS` | _(unset)_ | Retry backoff in milliseconds |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD` | _(unset)_ | Consecutive bridge failures before the circuit opens |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC` | _(unset)_ | Cooldown in seconds before the bridge circuit closes again |
| `SCRATCHBIRD_AI_HTTP_API_TOKEN` | _(none)_ | Optional Bearer token presented to the bridge |
| `SCRATCHBIRD_AI_HTTP_DIALECTS` | `native` | Dialect CSV used for `hybrid` mode |
| `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH` | _platform user state dir_ | Path to the durable approval evidence ledger |
| `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS` | _(unset)_ | Maximum number of compile-repair attempts for recoverable wrapper noise |
| `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` | _(unset)_ | Rolling window size for quota enforcement (seconds) |
| `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW` | _(unset)_ | Maximum requests per quota window |
| `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW` | _(unset)_ | Maximum mutation operations per quota window |
| `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW` | _(unset)_ | Maximum cost units per quota window |
| `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS` | _(unset)_ | Comma-separated accepted ScratchBird server versions (fail-closed if set) |
| `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS` | _(unset)_ | Accepted parser/compiler versions |
| `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS` | _(unset)_ | Accepted driver runtime versions |

---

## Bridge Environment Variables

These variables configure the local HTTP bridge process:

### Core Bridge Settings

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_HOST` | `127.0.0.1` | Bridge bind host |
| `SCRATCHBIRD_AI_BRIDGE_PORT` | `3095` | Bridge bind port |
| `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` | _(none)_ | Optional Bearer token that clients must present |
| `SCRATCHBIRD_AI_BRIDGE_DIALECTS` | `native` | Comma-separated list of enabled dialects |
| `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` | _(none)_ | Fallback DSN for all enabled dialects |
| `SCRATCHBIRD_AI_BRIDGE_DSN_<DIALECT>` | _(none)_ | Per-dialect DSN override; for example, `SCRATCHBIRD_AI_BRIDGE_DSN_NATIVE` |

### Server Setup and Transport

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` | `listener-only` | Server connection profile: `listener-only`, `managed`, `ipc-only`, or `embedded` |
| `SCRATCHBIRD_AI_BRIDGE_TRANSPORT_MODE` | _(derived)_ | Explicit transport override: `inet_listener`, `managed`, `local_ipc`, `embedded` |
| `SCRATCHBIRD_AI_BRIDGE_FRONT_DOOR_MODE` | _(derived)_ | Explicit front-door override: `direct` or `manager_proxy` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_METHOD` | _(auto)_ | IPC method: `auto`, `unix`, `pipe`, `tcp` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_PATH` | _(none)_ | IPC socket or pipe path for `ipc-only` mode |

### Managed (Manager Proxy) Settings

These variables are required when `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=managed`.
The `MANAGER_` prefix and the `MCP_` prefix are aliases for the same setting:

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` / `SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_TOKEN` | _(none)_ | Manager signon token (required for managed mode) |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_USERNAME` / `SCRATCHBIRD_AI_BRIDGE_MCP_USERNAME` | _(none)_ | Manager username override |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_DATABASE` / `SCRATCHBIRD_AI_BRIDGE_MCP_DATABASE` | _(none)_ | Managed database override |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_CONNECTION_PROFILE` / `SCRATCHBIRD_AI_BRIDGE_MCP_CONNECTION_PROFILE` | `native_v3` | Managed connection profile |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_INTENT` / `SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_INTENT` | `native_v3` | Managed client intent |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_FLAGS` / `SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_FLAGS` | `0` | Managed client flags (0–65535) |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_FAST_PATH` / `SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_FAST_PATH` | `true` | Managed fast-path auth toggle |

### Driver and Compile Settings

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` | _(none)_ | Path to the ScratchBird Python driver `src/` directory when the driver is not pip-installed |
| `SCRATCHBIRD_AI_BRIDGE_STRICT_COMPILE` | `0` | When `1`, compile probe failure returns HTTP 400 instead of a warning |

---

## Governance and Logging Variables

These variables configure audit attestation and structured logging paths:

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH` | _platform user state dir_ | Path for JSONL structured event log |
| `SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR` | _platform user state dir_ | Directory for operator runbook bundle output |
| `SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE` | _(unset)_ | Attestation mode: `hmac_sha256` or `external_reference` |
| `SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET` | _(none)_ | Shared secret for HMAC attestation mode |
| `SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID` | _(none)_ | Attestor identity included in attestation records |
| `SCRATCHBIRD_AI_RUNTIME_ROOT` | _(platform user state)_ | Override for the runtime root directory used for default ledger, log, and bundle paths |

---

## Live Certification Variables

These variables are used by the live certification harness
(`tools/run_live_native_conformance.py`):

| Variable | Description |
| --- | --- |
| `SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED` | Set to `1` to enable live native certification runs |
| `SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE` | Set to `1` to have the harness launch the bridge automatically |
| `SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION` | Declared server version for the certification run |
| `SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION` | Declared parser/compiler version |
| `SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION` | Declared test dataset version |
| `SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION` | Declared seed/fixture version |

---

## Recommended Development Baseline

The following baseline is drawn from
`docs/guides/RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`. Use it as a
starting point for a local development or CI environment:

```bash
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL=http://127.0.0.1:3095
export SCRATCHBIRD_AI_HTTP_DIALECTS=native
export SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH=artifacts/runtime/approval_ledger.json
export SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH=artifacts/runtime/structured_events.jsonl
export SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR=artifacts/runtime/operator_bundle
export SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE=hmac_sha256
export SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET=replace-with-local-attestation-secret
export SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS=3
export SCRATCHBIRD_AI_OPERATION_WINDOW_SEC=60
export SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW=100
export SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW=20
export SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW=1000
export SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS=1
export SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS=100
export SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD=3
export SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC=30
```

---

## Example Bridge Configuration File

The file `project/ai/examples/http-bridge.env.example` contains a fully
commented template for bridge variables. Copy and edit it for local use:

```bash
cp examples/http-bridge.env.example .env.bridge
```

Source it before launching the bridge:

```bash
set -a
source .env.bridge
set +a
PYTHONPATH=src tools/run_local_bridge.sh
```




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/getting_started.md -->

<a id="ch-ai-integration-guide-getting-started-md"></a>

# Getting Started

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter guides you through the initial bring-up of ScratchBird AI for
Beta 1 evaluation: install the Python environment, run local validation,
configure the bridge, start the bridge and MCP server, and optionally run the
live certification harness.

This guide covers the general flow. For platform-specific detail, the source
install guides are at:

- Ubuntu/Linux: `project/ai/docs/guides/INSTALL_UBUNTU_BETA1.md`
- Windows: `project/ai/docs/guides/INSTALL_WINDOWS_BETA1.md`

---

## Support Scope

ScratchBird AI supports **ScratchBird native** workflows only. The current
primary certified path is `listener_direct` (direct TCP connection to the
native ScratchBird listener). The `manager_proxy`, `local_ipc`, and
`embedded_local_only` runtime modes are implemented and admitted in ScratchBird
core, but live certification evidence for those modes depends on the test
environment exposing them.

---

## Prerequisites

### All Platforms

- Python 3.11 or later
- A ScratchBird server reachable from the host when using live bridge mode
- The ScratchBird Python driver, either:
  - installed in the active Python environment, or
  - available as source with `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` pointing
    to the driver `src/` directory

### Ubuntu / Linux Additional

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip git
```

### Windows Additional

- Windows PowerShell
- Optional: Windows Terminal

---

## Step 1: Install the Repository

**Ubuntu/Linux:**

```bash
cd ScratchBird/project/ai
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -U pip
python3 -m pip install -e ".[mcp]"
```

**Windows (PowerShell):**

```powershell
cd ScratchBird\project\ai
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -U pip
python -m pip install -e ".[mcp]"
```

If PowerShell blocks activation:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.venv\Scripts\Activate.ps1
```

---

## Step 2: Validate the Local Baseline

Run these validation steps from the `project/ai` directory. All should pass
before attempting live bridge mode.

**Ubuntu/Linux:**

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py \
  --repo-root . --artifact-root build/ai/artifacts
PYTHONPATH=src python3 tools/validate_evidence_gates.py \
  --repo-root . --artifact-root build/ai/artifacts \
  --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
python -m unittest discover -s tests -p 'test_*.py'
python tools\validate_capability_matrix.py
python tools\smoke_http_contract.py --mode selftest
python tools\generate_ai_conformance_artifacts.py `
  --repo-root . --artifact-root build\ai\artifacts
python tools\validate_evidence_gates.py `
  --repo-root . --artifact-root build\ai\artifacts `
  --spec docs\releases\EARLY_BETA_CONFORMANCE_GATES.md
```

Expected outcomes:

- Test suite passes
- Capability matrix validator exits `0`
- Selftest smoke prints `[smoke] PASS`
- Conformance artifacts regenerate successfully
- Evidence gate validator prints `OK: evidence gates valid ...`

---

## Step 3: Configure the Bridge for Live Mode

Copy the example configuration:

```bash
cp examples/http-bridge.env.example .env.bridge
```

Edit at minimum:

| Variable | Description |
| --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` | DSN pointing to your ScratchBird server (e.g. `scratchbird://user:password@127.0.0.1:3092/mydb`) |
| `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` | Connection mode; use `listener-only` for first live bring-up |
| `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` | Path to the driver `src/` if not pip-installed |

Recommended first live bring-up settings:

```bash
SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only
SCRATCHBIRD_AI_ADAPTER_MODE=http
SCRATCHBIRD_AI_HTTP_DIALECTS=native
```

Optional local guardrails to add:

```bash
SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH=artifacts/runtime/approval_ledger.json
SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS=3
SCRATCHBIRD_AI_OPERATION_WINDOW_SEC=60
SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW=100
SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW=20
SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW=1000
SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS=1
SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS=100
SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD=3
SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC=30
```

For managed mode (`SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=managed`), also set:

```bash
SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN=<token>
```

For `ipc-only` and `embedded` modes: confirm that the ScratchBird Python driver
and runtime support those transport modes before configuring them.

**Windows:** Set the same variables as PowerShell environment variables:

```powershell
$env:SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN = 'scratchbird://user:password@127.0.0.1:3092/mydb'
$env:SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP = 'listener-only'
$env:SCRATCHBIRD_AI_ADAPTER_MODE = 'http'
$env:SCRATCHBIRD_AI_HTTP_DIALECTS = 'native'
```

---

## Step 4: Start the HTTP Bridge

**Ubuntu/Linux (POSIX helper):**

```bash
set -a
source .env.bridge
set +a
PYTHONPATH=src tools/run_local_bridge.sh
```

**Ubuntu/Linux (direct module):**

```bash
set -a
source .env.bridge
set +a
export PYTHONPATH=src
python3 -m scratchbird_ai.http_bridge
```

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
python -m scratchbird_ai.http_bridge
# or: scratchbird-ai-http-bridge
```

The bridge binds to `127.0.0.1:3095` by default. Confirm it is listening
before proceeding.

---

## Step 5: Start the MCP Server with the Bridge

In a second terminal (the bridge must remain running):

**Ubuntu/Linux (POSIX helper):**

```bash
set -a
source .env.bridge
set +a
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://${SCRATCHBIRD_AI_BRIDGE_HOST:-127.0.0.1}:${SCRATCHBIRD_AI_BRIDGE_PORT:-3095}"
export SCRATCHBIRD_AI_HTTP_API_TOKEN="${SCRATCHBIRD_AI_BRIDGE_API_TOKEN:-}"
PYTHONPATH=src tools/run_local_stack.sh
```

**Ubuntu/Linux (direct module):**

```bash
export PYTHONPATH=src
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://127.0.0.1:3095"
python3 -m scratchbird_ai.mcp_server
```

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
$env:SCRATCHBIRD_AI_ADAPTER_MODE = 'http'
$env:SCRATCHBIRD_AI_HTTP_BASE_URL = 'http://127.0.0.1:3095'
python -m scratchbird_ai.mcp_server
# or: scratchbird-ai-mcp
```

---

## Step 6: Run the Live Contract Smoke Test (Optional)

When the bridge is running against a real ScratchBird server:

```bash
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode live --dialect native
```

The current shared static-example environment publishes the native ScratchBird
listener by default. If you have not started reference emulation listeners
separately, expect only the native lane to answer.

---

## Step 7: Run the Live Certification Harness (Optional)

The direct-listener certification sequence was exercised successfully on
2026-04-20 against the shared ScratchBird node. Re-run it whenever the
environment is refreshed or when capturing new release evidence:

```bash
set -a
source .env.bridge
set +a
export SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED=1
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE=1
export SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH="${SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH:-$HOME/.scratchbird/static-example/profiles/runtime.env}"
export SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION="current-shared-node-2026-04-20"
export SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION="current-v3-prebuild"
export SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION="shared-main"
export SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION="shared-node"
PYTHONPATH=src python3 tools/run_live_native_conformance.py \
  --covered-profile mcp_local_v0 \
  --covered-profile mcp_remote_v0 \
  --covered-profile streaming_async_v0 \
  --covered-profile retrieval_ingest_v0
```

After the harness run, regenerate and validate release artifacts:

```bash
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root .
PYTHONPATH=src python3 tools/validate_evidence_gates.py \
  --repo-root . --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

The harness writes `summary.json`, `environment_manifest.json`, `run_log.json`,
and `test_report.junit.xml` under `artifacts/live_native_conformance/`.

---

## Step 8: Validate Release-Candidate Claims (Optional)

```bash
PYTHONPATH=src python3 tools/validate_release_candidate.py \
  --claim-profile provider_tool_calling_v0 \
  --release-time-utc 2026-04-20T00:00:00Z
```

For live-native profile claims such as `service_internal_v0`, run the
live certification harness first so `artifacts/live_native_conformance/`
exists on the same commit.

---

## Governed Mutation Note

Mutation execution requires prior approval. Current behavior:

- Approval evidence is validated and persisted in the local approval ledger
- Approval reuse is checked against tenant, actor, and statement hash
- Expired or revoked approvals fail closed
- Request, mutation, and cost windows are enforced before execution

For operator-facing controls, see [governance_quotas_and_audit.md](#ch-ai-integration-guide-governance-quotas-and-audit-md).

---

## Common Startup Failures

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `ImportError: scratchbird` | Driver not installed or not on path | Set `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` to the driver `src/` directory |
| `401 Unauthorized` | Token mismatch between adapter and bridge | Match `SCRATCHBIRD_AI_HTTP_API_TOKEN` and `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` |
| `404 Dialect not enabled` | `native` not in bridge dialect list | Set `SCRATCHBIRD_AI_BRIDGE_DIALECTS=native` |
| `400 Managed setup requires manager_auth_token` | Managed mode without token | Set `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` |
| `503 Connection failed` | Bad DSN or server unreachable | Verify `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` and server reachability |

For detailed troubleshooting, see [troubleshooting.md](#ch-ai-integration-guide-troubleshooting-md).




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/release_status_and_known_gaps.md -->

<a id="ch-ai-integration-guide-release-status-and-known-gaps-md"></a>

# Release Status and Known Gaps

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter consolidates the status snapshots, known gap reports, support
matrix, conformance gates, and remaining live tasks for the ScratchBird AI
component as of the current early-beta release. All dates and framing reflect
the early-beta state; this content does not claim production readiness.

---

## Release Identity

| Item | Value |
| --- | --- |
| Release version | `0.1.0` |
| Release track | Early beta / technical Beta 1 |
| Original release date | 2026-02-18 |
| Status timestamp | 2026-04-21 |
| Overall status | **Green-Yellow** |

**Green-Yellow** means: the current ScratchBird-only baseline is implemented
and test-green. Direct-listener live evidence was refreshed successfully on
2026-04-20. Live evidence for the `manager_proxy`, `local_ipc`, and
`embedded_local_only` runtime modes is still environment-dependent.

---

## What Was Shipped in the Initial Release (2026-02-18)

The initial `0.1.0` release delivered:

- MCP server scaffold (`scratchbird-ai-mcp`)
- Core orchestration service with dialect routing, capability gating,
  compile/execute split, and read-only policy defaults
- HTTP adapter implementations and contract handling
- Local HTTP bridge (`scratchbird-ai-http-bridge`) with compile, execute,
  metadata, and auth endpoints
- Capability matrix schema and baseline payload
- Local utility scripts: `run_local_bridge.sh`, `run_local_stack.sh`,
  `smoke_http_contract.py`
- `native` dialect as the only supported AI dialect scope

Initial validation: 21 tests passing, HTTP bridge contract smoke selftest
passed, static checks passed.

---

## Implemented Surface Since Initial Release

By April 2026 the implemented surface expanded to include:

- Durable approval ledger with persisted identifiers, expiry, revocation, and
  replay-safe reuse checks
- Bounded compile-repair for fenced/labeled recoverable query wrappers
- In-process quotas, rate limits, cost attribution, and HTTP retry/circuit-
  breaker controls
- Fail-closed compatibility checks for declared ScratchBird server, parser,
  and driver/runtime versions
- ScratchBird-native registry publication, route resolution, graph capability
  publication, and expanded remote-auth normalization
- Remote MCP session lifecycle with bearer, OAuth2/JWT, workload identity,
  proxy principal, LDAP, Kerberos/GSSAPI, RADIUS PAP, PAM, and
  preauthenticated context auth families
- Structured JSONL runtime event logging and operator runbook/SLO bundle
  generation
- Audit attestation (HMAC-SHA256 and external reference) issue/verify
- Live-native certification harness with machine-readable artifact capture
- Ubuntu and Windows Beta 1 install guides

---

## Current Readiness by Area (April 2026)

| Area | Status | Notes |
| --- | --- | --- |
| Core service orchestration | Green | `ScratchBirdAIService` covers compile, execute, read-only query, mutation gating, explain, and retrieval |
| Native-only routing and capability matrix | Green | Router and matrix enforced fail-closed for non-native dialects |
| HTTP adapter and bridge runtime | Green | Adapter, bridge endpoints, auth checks, and service round-trip tests pass |
| Tool schema and policy guardrails | Green | Strict payload validation, error envelopes, mode normalization |
| Retrieval helpers | Green | Engine-free vector and hybrid retrieval with deterministic ranking and tenant isolation |
| Deterministic plan/audit/routing helpers | Green | Plan hashing, audit replay, cluster-routing covered by tests and evidence |
| Native live-workload hardening | Yellow | Direct-listener live path certified; broader real-workload and additional runtime-mode coverage still limited |
| Mutation governance maturity | Green | Durable ledger validation with expiry, revocation, replay-safe checks |
| Operations hardening | Green | Quotas, rate limits, cost attribution, HTTP retry/circuit-breaker implemented and tested |
| ScratchBird core AI surface truth | Green | Core packet published in capabilities and manifests |
| ScratchBird-native control/auth publication | Green | Graph ops, registry/routing controls, remote MCP, auth-family advertisement implemented |

---

## Platform and Install Support Matrix

| Area | Status | Notes |
| --- | --- | --- |
| Ubuntu install | Supported | Primary Beta 1 reviewer path |
| Windows install | Partial | Repo-local install documented; live bridge depends on Windows driver/runtime |
| Local offline validation | Supported | Tests, capability validation, artifact generation, evidence gates |
| ScratchBird Python driver from source path | Supported | Use `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` |
| Packaged public installer | Out of scope | Source-first Beta 1 only |

---

## Runtime Mode Support Matrix

| Runtime mode | Status | Notes |
| --- | --- | --- |
| `listener_direct` | Supported | Primary live certification target; direct-listener evidence refreshed 2026-04-20; native listener at `127.0.0.1:13092` in the shared environment |
| `manager_proxy` | Supported after environment-specific live refresh | Admitted in ScratchBird core; needs live environment that exposes this mode |
| `local_ipc` | Supported after environment-specific live refresh | Requires driver/runtime IPC support |
| `embedded_local_only` | Supported after environment-specific live refresh | Requires driver/runtime support; single-connection semantics |
| HTTP bridge selftest | Supported | Covered by repo validation |
| MCP local server | Supported | Available via `mcp` optional dependency |

---

## Interface and Profile Matrix

| Interface/profile | Status | Notes |
| --- | --- | --- |
| `service_internal_v0` | Supported | Covered by refreshed direct-listener live certification |
| `mcp_local_v0` | Supported | Covered by refreshed direct-listener live certification |
| `mcp_remote_v0` | Supported | Remote session/auth/streaming surface with live certification |
| `langchain_v0` | Supported | Framework adapter with parity artifacts |
| `llamaindex_v0` | Supported | Framework adapter and retrieval wrappers with parity artifacts |
| `semantic_kernel_v0` | Supported | Framework adapter with parity artifacts |
| `provider_tool_calling_v0` | Supported | OpenAI/Anthropic/Gemini payload normalization with parity artifacts |
| `streaming_async_v0` | Supported | Polling, continuation, cancellation, SSE surface with live certification |
| `retrieval_ingest_v0` | Supported | Retrieval lifecycle with live certification for baseline behavior |
| `governance_certification_v0` | Supported | Approval validation, listing/revocation, audit replay, attestation issue/verify |

---

## Conformance Gates

The release gate is evaluated by running:

```bash
python tools/generate_ai_conformance_artifacts.py \
  --repo-root . --artifact-root ./build/ai/artifacts
python tools/validate_evidence_gates.py \
  --repo-root . --artifact-root ./build/ai/artifacts \
  --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

Summary of the 13 evidence IDs (`EVID-01` through `EVID-13`):

| Evidence ID | Bound area |
| --- | --- |
| `EVID-01` | Baseline repository readiness |
| `EVID-02` | HTTP adapter and bridge runtime |
| `EVID-03` | Service orchestration surface |
| `EVID-04` | Vector retrieval API |
| `EVID-05` | Hybrid retrieval |
| `EVID-06` | Tool contract and compatibility |
| `EVID-07` | Plan introspection determinism |
| `EVID-08` | Execution mode and policy |
| `EVID-09` | Audit bundle determinism |
| `EVID-10` | Cluster-aware routing |
| `EVID-11` | Release integrity and doc alignment |
| `EVID-12` | Framework adapter parity |
| `EVID-13` | Direct provider tool-calling parity |

Rules: all artifacts must share the same `git_commit`; artifacts older than 14
days from release evaluation time fail validation; template artifacts are not
allowed.

Passing these gates means the current early-beta implementation is internally
consistent and reproducible for its shipped feature set. It does not imply
production-grade authorization depth, non-native dialect support, or live
certification for runtime modes not exposed in the active harness.

---

## Known Gaps (April 2026)

### Functional Gaps

- Explain/trace data exists at the helper and service level, but broader live
  bridge-backed explain validation is still limited.
- Native live-workload coverage is narrower than in-process and fake-backend
  contract coverage beyond the refreshed direct-listener path.
- Engine-managed retrieval depth is incomplete beyond the baseline
  live-certified lifecycle/search path.

### Governance and Security Gaps

- Fine-grained authorization and tenant boundary policy are stronger than the
  February baseline but still not production-complete.
- Third-party signing and externally hosted approval products are not finished.
  The shipped surface stops at durable local approval evidence plus HMAC or
  external-reference attestation issue/verify.

### Operational Gaps

- Operator bundle generation, runtime diagnostics, and SLO summary generation
  are implemented, but the repository does not ship a pre-generated
  target-specific production dashboard/runbook package for every environment.
- Environment-specific live evidence for `manager_proxy`, `local_ipc`, and
  `embedded_local_only` still depends on the active test harness exposing those
  runtime modes.

### Documentation and Release Gaps

- Release readiness depends on the conformance gates contract remaining aligned
  with the actual code surface.
- The remaining documentation work is current completion scope rather than later
  cleanup.

---

## What Is Intentionally Outside Beta 1

- Non-ScratchBird AI/database support
- Third-party signing infrastructure beyond local HMAC attestation
- Public packaged installers
- Production-grade authorization depth
- Automatic live certification for runtime modes not currently exposed

---

## Exit Criteria Toward the Next Milestone

1. Certify additional runtime modes on environments that expose `manager_proxy`,
   `local_ipc`, or `embedded_local_only`.
2. Expand live workload, explain/trace, and retrieval-scale certification beyond
   the bounded current claim surface.
3. Finish third-party signing and externally hosted approval productization.
4. Keep release/status materials synchronized with generated evidence.
5. Publish target-specific operator dashboard/runbook packages for supported
   environments.




===== FILE SEPARATION =====

<!-- chapter source: AI_Integration_Guide/troubleshooting.md -->

<a id="ch-ai-integration-guide-troubleshooting-md"></a>

# Troubleshooting

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter provides practical troubleshooting guidance for the ScratchBird AI
local HTTP bridge and the direct-listener validation path. It covers the
currently supportable early-beta surface: ScratchBird native workflows, the
local HTTP bridge, and direct-listener validation.

The primary certified path is `listener_direct`. `manager_proxy`, `local_ipc`,
and `embedded_local_only` are admitted in ScratchBird core and notes for those
modes appear where relevant, but the detailed steps below assume
`listener-only` as the server setup profile.

---

## First Checks

Before diagnosing live bridge or server issues, verify the local baseline:

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
```

If any of these fail, fix the local repository or runtime before investigating
the live target.

### Confirm the Runtime Profile

The active runtime profile is supplied by policy and normally points at:

- `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH`
- `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH`

Check that the configured native ScratchBird listener is actually published and
reachable:

```bash
ss -ltn
```

Confirm the configured native listener address appears in `LISTEN` before
concluding that the bridge or driver is broken.

---

## Bridge Boot Failures

### `ImportError: scratchbird`

**Cause:** The bridge cannot import the ScratchBird Python driver.

**Fix:**

```bash
export SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC=/path/to/driver/src
```

Or install the driver in the active Python environment.

---

### `503 Connection failed`

**Cause:** Bad DSN, unreachable ScratchBird listener, or transport mode
mismatch.

**Checks:**

1. Verify `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` is correct.
2. Verify the ScratchBird listener is reachable from this host.
3. Verify the configured runtime profile is actually publishing the native
   listener address.
4. Use `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only` for the current
   release-truth path.
5. Do not expect reference emulation ports to answer unless those listeners were
   explicitly started and AI support for that lane is in scope.

---

### `08001 connect() failed` with engine endpoint diagnostics

**Example messages:**

```
08001 connect() failed: No such file or directory | engine endpoint diagnostics:
  engine_endpoint=...; base_socket=missing; parser_socket=stale_or_not_listening
```

```
08001 connect() failed: Connection refused | engine endpoint diagnostics:
  engine_endpoint=...; base_socket=missing; parser_socket=stale_or_not_listening
```

**Cause:** The ScratchBird listener/parser path is up far enough to accept the
client, but the engine IPC endpoint is missing, stale, or not actually
listening.

**Checks:**

1. Inspect `artifacts/live_native_conformance/run_log.json`.
2. Read `native_preflight.runtime_diagnostics`.
3. Verify the engine endpoint published in `profiles/runtime_ownership.json`
   is a live Unix socket, not just a leftover path entry.
4. If `127.0.0.1:13092` is listening but the engine endpoint diagnostics still
   report a missing or stale socket, treat it as a ScratchBird runtime issue,
   not an AI bridge issue.
5. If the base socket is missing while only a `.parser_v1` sibling exists or
   is stale, record it as a ScratchBird runtime issue.

---

### `404 Dialect not enabled`

**Cause:** `native` is not present in the bridge dialect list.

**Fix:**

```bash
export SCRATCHBIRD_AI_BRIDGE_DIALECTS=native
```

---

## Authentication and Token Failures

### `401 Unauthorized`

**Cause:** Bridge token mismatch between the adapter and the bridge.

**Fix:** Make `SCRATCHBIRD_AI_HTTP_API_TOKEN` and
`SCRATCHBIRD_AI_BRIDGE_API_TOKEN` match.

---

### `400 Managed setup requires manager_auth_token`

**Cause:** `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=managed` was selected without
the required manager token.

**Fix:**

```bash
export SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN=<token>
```

Note: `managed` mode maps to the bounded ScratchBird core `manager_proxy` lane.
Using it requires matching manager credentials, driver support, and a test
environment that exposes this mode so live evidence can be rerun.

---

## Compatibility Failures

These errors indicate a version or transport mismatch between the declared
compatibility policy and the live environment:

| Error code | Condition |
| --- | --- |
| `E_SERVER_RUNTIME_UNSUPPORTED` | Declared server version not in supported list |
| `E_COMPONENT_VERSION_UNSUPPORTED` | Declared component version not in supported list |
| `E_DRIVER_RUNTIME_UNSUPPORTED` | Declared driver runtime version not in supported list |
| `E_INTERFACE_PROFILE_UNSUPPORTED` | Requested interface profile not supported |
| `E_TRANSPORT_PROFILE_UNSUPPORTED` | Requested transport not supported |

**Checks:**

- `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS`

The fail-closed behavior is by design. Do not suppress these checks unless the
environment is genuinely inside the supported window.

---

## Approval and Mutation Failures

### `E_APPROVAL_INVALID`

**Cause:** Missing approval token, mismatched tenant/actor/statement hash,
expired approval, or revoked approval.

**Checks:**

1. Inspect the approval ledger at `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`.
2. Verify the mutation was replayed with the same statement hash.
3. Verify `expires_at` is in the future.
4. Verify `revoked_at` is null.

---

### `E_LIMIT_EXCEEDED`

**Cause:** Request, mutation, or cost window exceeded.

**Checks:**

| Rule ID | Variable to inspect |
| --- | --- |
| `OPS-RATE-001` | `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW`, `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` |
| `OPS-MUTATION-001` | `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW`, `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` |
| `OPS-COST-001` | `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW`, `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` |

Increase the window budget only if the higher budget is justified for the
current environment.

---

## Compile and Bridge Resilience Failures

### Compile failures

If wrapped query text causes a compile failure:

1. Reduce prompt formatting noise (avoid markdown fences and `sql:`/`query:`
   prefixes in the query text passed to the tool).
2. Check `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS`. The repair strategies
   are deterministic and bounded: only whitespace trimming, markdown fence
   stripping, and leading label stripping are attempted.
3. Remember that compile-repair does not rewrite query semantics.

### Bridge flapping

If the bridge becomes unstable:

1. Inspect `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS`
2. Inspect `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS`
3. Inspect `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD`
4. Inspect `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC`

Retry applies only to `GET` and compile endpoints; execute and mutation
endpoints are not retried automatically.

---

## Live Recertification

When you need to regenerate live evidence:

```bash
export SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED=1
export SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE=1
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH="${SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH:-./profiles/runtime.env}"
PYTHONPATH=src python3 tools/run_live_native_conformance.py \
  --scratchbird-server-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION:-current-shared-node-2026-04-20}" \
  --parser-compiler-version "${SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION:-current-v3-prebuild}" \
  --test-dataset-version "${SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION:-shared-main}" \
  --seed-or-fixture-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION:-shared-node}" \
  --covered-profile mcp_local_v0 \
  --covered-profile mcp_remote_v0 \
  --covered-profile streaming_async_v0 \
  --covered-profile retrieval_ingest_v0
```

Then regenerate and validate release artifacts:

```bash
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root .
PYTHONPATH=src python3 tools/validate_evidence_gates.py \
  --repo-root . --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

If the live harness reports `no table available for describe endpoint`:

- Leave `SCRATCHBIRD_AI_SMOKE_SCHEMA` and `SCRATCHBIRD_AI_SMOKE_TABLE` unset.
  The harness walks discovered schemas until it finds a describable native table.
- Only pin `schema` or `table` explicitly when validating a known object.

---

## Escalation Boundary

The following conditions are not missing core release contracts. Treat them as
live environment, driver, or runtime-packaging issues:

- Live evidence regeneration needed for a refreshed ScratchBird target
- Driver/runtime support for `local_ipc` or embedded execution
- Environment-specific `manager_proxy` reachability or authentication
- A published engine endpoint that is missing, stale, or not actually listening

Record these against the updated test environment, not as AI-layer defects.




<a id="ch-glossary"></a>

# Glossary

## Purpose

This glossary defines terms used across the ScratchBird documentation set. The
definitions are written for end users, evaluators, operators, and developers.
They are intentionally concise and cautious: a term appearing here does not mean
the related feature is complete, enabled, or available in every build.

## ScratchBird Product Names

| Term | Meaning |
| --- | --- |
| ScratchBird | The project and product line described by this documentation. |
| ScratchBird Convergent Data Engine | The full product concept: engine, parsers, tools, resources, and operational surfaces. |
| SB | Short brand form used in names and examples. |
| SBcore | ScratchBird Engine. The embedded engine library that owns durable catalog identity, transactions, storage, security admission, recovery decisions, and engine diagnostics. |
| SBsql | ScratchBird SQL. The native ScratchBird command language and script runner surface. |
| SBParser | ScratchBird Core Parser. The native SBsql parser package that lowers SBsql requests to SBLR. |
| SBsrv | ScratchBird IPC Server. A local multi-user server process for same-machine clients. |
| SBgate | ScratchBird Listener. The listener and parser-facing entry point used for network-facing client traffic. |
| SBmgr | ScratchBird Single Node Manager. A single-node front door that can proxy authenticated connections to internal listener routes in managed deployments. |
| SBadm | ScratchBird Administrator. Administrative utility name for configuration, time zone, character set, collation, and policy management where present. |
| SBbak | ScratchBird Backup Manager. Utility name for backup and backup-set operations where present. |
| SBsec | ScratchBird Security. Utility name for security provider, user, role, group, and policy management where present. |
| SBdoc | ScratchBird Doctor. Utility name for analysis, diagnosis, and repair-oriented workflows where present and admitted. |
| SBcop | ScratchBird Conformance Officer. Utility name for conformance and comparison checks where present. |

## Architecture Terms

| Term | Meaning |
| --- | --- |
| Convergent Data Engine | An engine design that attempts to bring multiple data shapes, parser surfaces, transaction rules, security rules, and diagnostics under one shared engine authority model. |
| CDE | Abbreviation for Convergent Data Engine. |
| Engine authority | The rule that durable behavior belongs to SBcore: object identity, descriptors, transactions, security admission, storage, recovery, and diagnostics. |
| Parser boundary | The separation between a client language or wire protocol and engine execution authority. |
| Parser package | A component that accepts a specific language or protocol surface and lowers accepted work to ScratchBird execution requests. |
| Compatibility parser | A standalone parser package for one reference-system client family. It should not silently accept unrelated dialects. |
| SBsql language profile | A parser resource profile that can change user-facing SBsql spellings, phrase order, diagnostics, completion hints, and source rendering without changing SBLR, UUID identity, descriptors, security, storage, or MGA transaction authority. |
| Canonical element stream | The normalized parser output created before UUID binding. It records canonical token and surface identities rather than treating localized words as engine authority. |
| Standard SBsql fallback | A policy-controlled input fallback that lets a non-English session accept canonical English SBsql when the preferred language profile does not parse the statement. |
| Parser route | The configured path that determines which parser handles a client request. |
| SBLR | ScratchBird's bound engine-facing request representation. Parsers emit SBLR after parsing and binding accepted work. |
| Bound request | A structured request whose names, values, parameters, and types have been resolved enough to submit toward engine authority. |
| Raw text | The command text received from a client before parsing. Raw text is not durable engine authority. |
| Catalog projection | A view or metadata surface that presents engine catalog information in a particular shape for a parser, tool, or user. |
| Workarea | A schema-root area presented to a parser or user as its operating root. |
| Compatibility surface | The subset of behavior a parser or tool is designed and proven to accept, execute, or refuse clearly. |
| Refusal | A controlled response that says a request is unsupported, denied, unavailable, unsafe, or otherwise not admitted. |

## Database And Catalog Terms

| Term | Meaning |
| --- | --- |
| Database | A managed durable store of data, metadata, identity, transactions, security rules, diagnostics, and recovery behavior. |
| Metadata | Information that describes data, such as schemas, tables, columns, types, constraints, indexes, views, routines, grants, policies, and catalog rows. |
| Catalog | Engine-owned metadata that describes durable database objects and their relationships. |
| Catalog identity | The durable identity of a catalog object, separate from the user-facing name used to spell it. |
| UUID identity | Durable object identity based on UUIDs rather than only text names. |
| Object descriptor | Engine metadata describing an object shape, type, storage behavior, dependency, or operational capability. |
| Type descriptor | Metadata describing a datatype, its value behavior, binary representation, capabilities, and related rules. |
| Domain | A reusable constrained type definition. |
| Constraint | A rule attached to a table, column, domain, or related object. |
| Index | A search structure maintained for faster lookup, ordering, constraint enforcement, or query planning where implemented. |
| View | A named query projection. |
| Materialized view | A stored projection whose refresh and dependency behavior must be defined by the implementation. |
| Procedure | A stored routine that can perform controlled work and may return output parameters or result sets where supported. |
| Function | A routine that returns a value or result. |
| Package | A named grouping of routine definitions where supported. |
| Trigger | Routine behavior tied to table, database, transaction, or event-style actions where implemented. |
| Sequence | A database object that generates ordered values according to its definition. |
| Comment | Descriptive metadata attached to an object. Comments do not grant authority and should not contain secrets. |

## Schema And Name Terms

| Term | Meaning |
| --- | --- |
| Schema | A namespace branch that can contain objects and, where supported, child schemas. |
| Recursive schema tree | A schema model where schemas can contain child schemas, creating a tree rather than one flat namespace. |
| Database root | The top of the durable database tree. Not every session can see it directly. |
| Parser-visible root | The root of the namespace presented to the selected parser route. |
| Home schema | The schema associated with a user, identity, or configured workarea. |
| Current schema | The default schema used for unqualified names in a session. |
| Search path | An ordered lookup path used by commands that allow path-based name resolution. |
| Qualified name | A name that includes schema or path information, such as `app.notes`. |
| Unqualified name | A name without schema qualification, such as `notes`. |
| Name resolution | The process of turning a user-visible name into engine object identity. |
| Sandbox | The visible boundary that limits what a session or parser route can name, inspect, or access. |
| Schema branch | A subtree of the database namespace. |
| Object lifecycle | The create, alter, rename, comment, describe, use, refresh, validate, or drop actions that apply to an object type. |

## Transaction And Recovery Terms

| Term | Meaning |
| --- | --- |
| Transaction | A boundary around work that can commit, roll back, and participate in visibility rules. |
| Commit | Make a transaction's admitted changes final according to engine visibility rules. |
| Rollback | Discard uncommitted transaction changes. |
| Savepoint | A named point inside a transaction that can be rolled back without ending the whole transaction where supported. |
| Autocommit | A mode where each statement may be committed automatically according to session and parser rules. |
| MGA | ScratchBird's transaction and visibility authority model. In this documentation, the key rule is that transaction finality belongs to the engine. |
| Visibility | The rule that determines which transaction versions a session can see. |
| Cleanup | Engine-controlled work that reclaims or resolves old transaction state when it is safe. |
| Recovery | The process of reopening or refusing a database after shutdown, interruption, or uncertain durable state. |
| Recovery-required state | A state where the engine requires recovery handling before normal writes can proceed. |
| Fail closed | Refuse work when the safe outcome is uncertain instead of silently accepting it. |
| Reopen proof | A test that closes and reopens a database to verify committed state is still present. |

## Security Terms

| Term | Meaning |
| --- | --- |
| Identity | The authenticated user, service, or agent identity attached to a session or operation. |
| Principal | A user, role, group, service, or other authority-bearing identity. |
| Authentication | Establishing who the session or agent is. |
| Authorization | Deciding what an authenticated identity is allowed to do. |
| Grant | A permission given to a principal or object. |
| Revoke | Removal of a previously granted permission. |
| Role | A named set of privileges that can be granted and activated according to policy. |
| Policy | A rule that controls access, masking, row visibility, external access, operational admission, or protected material use. |
| Row-level security | Policy behavior that limits which rows a session can see or change. |
| Mask | A policy-controlled transformation that hides or changes protected values in query output. |
| Protected material | Secrets or sensitive values that require controlled storage, reference, redaction, and use. |
| Secret reference | A reference to protected material without placing the raw secret in a parser packet, script, or diagnostic. |
| Materialized authorization | Authorization information loaded into an engine-admissible form before work is executed. |
| Denied | A refusal because the authenticated identity, policy, or sandbox does not admit the operation. |

## Data And Type Terms

| Term | Meaning |
| --- | --- |
| Datatype | A named value category with storage, comparison, conversion, and validation behavior. |
| Scalar value | A single value such as an integer, timestamp, boolean, UUID, or text value. |
| Numeric type | A datatype for integer, unsigned integer, decimal, fixed-point, or floating-point values. |
| Text type | A datatype for character data governed by character set and collation rules. |
| Character set | The encoding rules for text values. |
| Collation | The comparison and ordering rules for text values. |
| Temporal type | A datatype for dates, times, timestamps, intervals, or time-zone-aware values. |
| UUID | A fixed-size identifier value commonly used for durable identity. |
| Binary value | A sequence of bytes with type-specific interpretation. |
| Protected value | A value governed by protected-material policy. |
| Document value | A structured value such as JSON-like data where implemented. |
| Graph value | A relationship-oriented value or model surface where implemented. |
| Vector value | A numeric vector used for similarity or embedding-style operations where implemented. |
| Time-series value | A value or record organized around time-oriented measurement behavior where implemented. |
| Coercion | An implicit or explicit conversion between compatible types. |
| Cast | An explicit type conversion requested by the user. |
| Null | A marker for absence of a value, distinct from zero, empty string, or false. |

## Query Terms

| Term | Meaning |
| --- | --- |
| DDL | Data definition language: commands that create, alter, describe, comment on, rename, or drop database objects. |
| DML | Data manipulation language: commands that read or change rows and values. |
| Query | A request that reads data and returns a result set or scalar result. |
| Result set | Rows and columns returned by a query or routine. |
| Projection | The selected output columns or expressions of a query. |
| Predicate | A condition used to filter rows or control logic. |
| Join | A query operation that combines rows from more than one source. |
| Grouping | A query operation that forms groups of rows for aggregate calculations. |
| Aggregate | A calculation over multiple rows, such as count or sum where implemented. |
| Window function | A calculation over a window of rows related to the current row where implemented. |
| CTE | Common table expression. A named temporary query expression inside a statement. |
| Recursive CTE | A CTE that refers to itself according to the rules of the language surface. |
| Ordering | The explicit sort order requested for result rows. |
| Limit | A request to return only a bounded number of rows. |
| Offset | A request to skip a number of rows before returning results. |
| Upsert | Insert-or-update behavior according to a conflict rule where supported. |
| Merge | A statement that conditionally inserts, updates, or deletes based on a source relation where supported. |
| Copy | A large or streaming data input or output surface where implemented and admitted. |

## Procedural Terms

| Term | Meaning |
| --- | --- |
| Procedural SQL | Stored routine language constructs such as blocks, variables, control flow, cursors, exceptions, and triggers where implemented. |
| Block | A procedural unit containing declarations and executable statements. |
| Variable | A named procedural value local to a routine or block. |
| Cursor | A controlled handle over a result set. |
| Result-set cursor | A cursor passed or returned as a routine-controlled result where supported. |
| Exception handler | Procedural logic that handles a diagnostic or error condition. |
| Event trigger | Trigger-style behavior tied to database, transaction, or event actions where implemented. |
| UDR | User-defined routine or parser-support routine package, depending on context. In parser documentation, it commonly means the package that supports bridge or extension behavior for that parser. |
| Bridge | A controlled connection or interface used by a parser-support routine to reach another database surface where configured and admitted. |

## Operations And Data Movement Terms

| Term | Meaning |
| --- | --- |
| Configuration | Settings that control startup, resource locations, parser registration, security providers, policy defaults, and runtime behavior. |
| Resource file | A staged file needed by the product, such as character set, collation, time zone, policy, or configuration data. |
| Health check | A diagnostic check that reports whether a component appears alive and able to answer. |
| Readiness check | A diagnostic check that reports whether a component is ready to accept intended work. |
| Liveness check | A diagnostic check that reports whether a component is still running. |
| Support bundle | A redacted package of diagnostic evidence for review or support. |
| Redaction | Removing or masking protected material before diagnostics are shown or bundled. |
| Message vector | Structured diagnostic output used for errors, refusals, and operational status. |
| Logical stream | Data movement represented as statements, rows, records, or events rather than physical page files. |
| Logical backup | A backup stream that represents database content as logical metadata and data operations. |
| Logical restore | Replaying a logical stream as admitted database operations. |
| Physical backup | A page-copy or file-copy backup shape. Compatibility parser routes should not treat physical page-copy formats as normal logical restore input. |
| Import | Bring external logical data into a database through an admitted parser or tool route. |
| Export | Write logical data from a database to an external stream or file according to policy. |
| CDC | Change data capture. A stream or record of changes suitable for replication, ETL, or integration where implemented. |
| Replication | Copying changes between systems according to an ordering and identity model where implemented. |
| ETL | Extract, transform, load. A data movement workflow that reads from one source, transforms, and writes to another target. |
| Migration | Moving schema, data, routines, security, or operational behavior from one database shape to another. |
| Quarantine | Holding questionable incoming records or events aside for review instead of applying them silently. |
| Cutover | The controlled switch from one active source or route to another. |
| Idempotency key | A value used to detect repeated events or operations so replay can be handled safely. |

## Build And Release Terms

| Term | Meaning |
| --- | --- |
| Build output | The generated binaries, libraries, parser packages, resources, and configuration artifacts for a target platform. |
| Output tree | The staged directory layout intended for testing or release packaging. |
| Target platform | The operating system and architecture being built or tested. |
| Proof gate | A test or validation step intended to prove that a behavior remains implemented and has not regressed. |
| CTest | The test runner integration used by many project tests. |
| Conformance test | A test that compares behavior against a declared specification, parser expectation, or compatibility target. |
| Smoke test | A small test proving that a basic workflow starts, runs, and stops. |
| Regression test | A test intended to prevent a previously handled behavior from breaking again. |
| Draft documentation | Documentation under active review. Draft status means users should verify commands and claims against the current build and tests. |



# About This Documentation

This book is part of the ScratchBird documentation set. ScratchBird is a
Convergent Data Engine (CDE).

**Draft status.** This is draft documentation. It describes the architecture and
intended behavior of the source tree. A topic appearing here does not by itself
guarantee that a feature is complete, enabled, performant, certified, or
available in any particular build. Always confirm against the current build,
configuration, tests, and release notes.

**License.** The ScratchBird engine is distributed under the Mozilla Public
License 2.0 (MPL-2.0). This documentation describes that open-source engine.

**No certification claim.** Nothing in this documentation constitutes a security
certification, performance benchmark, or compatibility guarantee.
