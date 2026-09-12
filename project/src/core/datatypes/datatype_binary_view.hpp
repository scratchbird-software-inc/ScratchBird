// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_binary.hpp"

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

struct DatatypeBinaryViewResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::size_t bytes_written = 0;
  bool ok() const { return status.ok(); }
};

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
