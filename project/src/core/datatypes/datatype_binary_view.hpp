// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_binary.hpp"

#include <array>
#include <string_view>
#include <type_traits>

namespace scratchbird::core::datatypes {

// Borrowed canonical payload. The caller retains storage through validation or
// encoding. A nonempty view requires a readable payload pointer. No payload
// ownership is transferred and no payload-sized temporary is materialized.
struct DatatypeBinaryValueView {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  bool is_null = false;
  bool payload_is_toast_reference = false;
  const byte* payload_data = nullptr;
  std::size_t payload_bytes = 0;
};

// Allocation-free diagnostic projection for borrowed validation and decode.
// Every string_view refers to immutable datatype-owned storage. UUID and
// integer values are native fields, never text shadows. The result and all
// argument backing are fixed-size and remain valid independently of the input
// buffer; only a successful payload view borrows the caller's source bytes.
// These V1 structs are a process-local compiled ABI. They are never persisted,
// hashed, transported, or used across unlike build targets.
enum class DatatypeBinaryDiagnosticArgumentKind : std::uint8_t {
  none = 0,
  text = 1,
  unsigned_integer = 2,
  uuid = 3,
  descriptor_reference = 4,
};

struct DatatypeBinaryDiagnosticArgumentView {
  std::string_view key;
  DatatypeBinaryDiagnosticArgumentKind kind =
      DatatypeBinaryDiagnosticArgumentKind::none;
  std::string_view text;
  u64 unsigned_integer = 0;
  scratchbird::core::platform::Uuid uuid{};
  u32 generation = 0;
};

inline constexpr std::size_t kDatatypeBinaryDiagnosticArgumentCapacity = 4;

struct DatatypeBinaryDiagnosticView {
  u16 abi_version = 1;
  Status status;
  std::string_view diagnostic_code;
  std::string_view message_key;
  std::array<DatatypeBinaryDiagnosticArgumentView,
             kDatatypeBinaryDiagnosticArgumentCapacity> arguments{};
  std::uint8_t argument_count = 0;
  std::string_view origin;
};

struct DatatypeBinaryAllocationFreeViewResult {
  u16 abi_version = 1;
  Status status;
  DatatypeBinaryDiagnosticView diagnostic;
  DatatypeBinaryValueView value;
  bool ok() const noexcept {
    return abi_version == 1 && diagnostic.abi_version == 1 && status.ok() &&
           diagnostic.status.ok() && diagnostic.diagnostic_code.empty();
  }
};

// Reporting context supplied only after the containing owner has validated the
// expected descriptor. The decoder copies this identity into diagnostics; it
// does not validate or certify a descriptor receipt.
struct DatatypeBinaryDiagnosticContextV1 {
  scratchbird::core::platform::Uuid descriptor_uuid{};
  u32 descriptor_generation = 0;
};

// Validates the exact direct, non-NULL base.binary component state without
// allocation, exception, receipt inference, or payload publication on failure.
// This is the datatype-owned value validator used by allocation-free
// containing readers.
DatatypeBinaryAllocationFreeViewResult
ValidateCanonicalBinaryValueViewNoAlloc(
    const DatatypeBinaryValueView& value,
    const DatatypeBinaryDiagnosticContextV1& context) noexcept;

// Validates a direct, non-NULL SBDVAL01 structural component whose canonical
// type must be base.binary and returns an exact borrowed payload view. This does
// not certify an outer descriptor receipt, page admission, authorization, or
// commit. The caller must retain the source bytes for every successful use of
// value.
DatatypeBinaryAllocationFreeViewResult
DecodeCanonicalBinaryValueViewNoAlloc(
    const byte* encoded, std::size_t encoded_bytes,
    const DatatypeBinaryDiagnosticContextV1& context) noexcept;

static_assert(std::is_standard_layout_v<DatatypeBinaryDiagnosticArgumentView>);
static_assert(std::is_trivially_copyable_v<DatatypeBinaryDiagnosticArgumentView>);
static_assert(std::is_standard_layout_v<DatatypeBinaryDiagnosticContextV1>);
static_assert(std::is_trivially_copyable_v<DatatypeBinaryDiagnosticContextV1>);
static_assert(std::is_standard_layout_v<DatatypeBinaryDiagnosticView>);
static_assert(std::is_trivially_copyable_v<DatatypeBinaryDiagnosticView>);
static_assert(std::is_standard_layout_v<DatatypeBinaryAllocationFreeViewResult>);
static_assert(std::is_trivially_copyable_v<DatatypeBinaryAllocationFreeViewResult>);

struct DatatypeBinaryViewResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::size_t bytes_written = 0;
  bool ok() const { return status.ok(); }
};

// Owning diagnostic materialization may throw std::bad_alloc. It occurs only
// after the fixed result is final and publishes no value on materialization
// failure; allocation-free readers call the V1 API above directly.
DatatypeBinaryViewResult ValidateDatatypeBinaryValueView(
    const DatatypeBinaryValueView& value);

// Writes the unchanged SBDVAL01 envelope to caller-owned capacity. Validation
// and capacity failure leave destination bytes unchanged. Successful writes
// affect exactly bytes_written bytes; payload/destination overlap is allowed.
DatatypeBinaryViewResult EncodeDatatypeBinaryValueInto(
    const DatatypeBinaryValueView& value, byte* destination,
    std::size_t destination_bytes);

struct DatatypeBinaryDecodedViewResult {
  Status status;
  DiagnosticRecord diagnostic;
  DatatypeBinaryValueView value;
  bool ok() const { return status.ok(); }
};

// Validates the complete canonical envelope and returns a borrowed payload.
// The source must remain alive and unchanged through every use of the view.
// Unknown flags, nonzero reserved fields and noncanonical headers are refused.
// Known TOAST-reference values retain their general datatype role; consumers
// such as typed result packets must separately forbid that role when required.
// Refusal exposes no payload view. No payload-sized temporary is allocated.
DatatypeBinaryDecodedViewResult DecodeDatatypeBinaryValueView(
    const byte* encoded, std::size_t encoded_bytes);

}  // namespace scratchbird::core::datatypes
