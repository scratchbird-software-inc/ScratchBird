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

These symbolic codes appear as `symbolic_code` strings in `sb_engine_diagnostic_view_t.symbolic_code` and as diagnostic codes in `SblrDecodedEnvelope.diagnostic_code` (see [SBLR Envelope](sblr_envelope.md)).

## Admission-Only Operations

In the current Core Beta surface, all operation families in the Priority D registry are marked `admission_only`. This means the engine accepts and validates envelopes for these operation families but does not execute them; the engine returns `SBLR.EXECUTION.ADMISSION_ONLY` as the diagnostic code. This is a documented beta behavior, not an error in the caller's usage. See [SBLR Envelope](sblr_envelope.md) for the full Priority D registry.

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
