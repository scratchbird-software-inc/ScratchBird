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
