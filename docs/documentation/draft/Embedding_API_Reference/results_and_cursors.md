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

See [Diagnostics and Errors](diagnostics_and_errors.md) for the `sb_engine_diagnostic_set_view_t` structure. The `out_view` is valid for the lifetime of the result handle.

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

Note: `sb_engine_row_batch_view_v1_t` provides row count and stream state, but individual row/column values are not directly accessible through public C ABI functions exposed in `engine.h`. Row value access through the public headers is handled via the C++ `ExecutionValue` / `PlainValuePayload` encoding described in [Types, Descriptors, and Values](types_descriptors_and_values.md). The precise mechanism by which batched rows are accessed from `out_batch` is not fully enumerated in the frozen public C headers; the batch view provides count and stream metadata.

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
