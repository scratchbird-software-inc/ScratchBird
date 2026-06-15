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
