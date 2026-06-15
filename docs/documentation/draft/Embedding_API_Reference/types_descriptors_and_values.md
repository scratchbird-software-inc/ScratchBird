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
