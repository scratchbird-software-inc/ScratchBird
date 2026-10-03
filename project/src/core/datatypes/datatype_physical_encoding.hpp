// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_layout.hpp"
#include "datatype_binary_view.hpp"

#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;

inline constexpr const char* kDatatypePhysicalEncodingKey =
    "MDF-013-CURRENT-CORE-DATATYPE-PHYSICAL-ENCODING";

enum class DatatypePhysicalValueState : u16 {
  sql_null = 0,
  value = 1,
  overflow_root = 2,
  overflow_chunk = 3,
  locator_handle = 4,
  opaque_handle = 5,
  protected_chunk_root = 6,
  unknown = 0xffff
};

struct DatatypePhysicalValue {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  DatatypePhysicalValueState state = DatatypePhysicalValueState::unknown;
  std::vector<byte> payload;
};

struct DatatypePhysicalValueView {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  DatatypePhysicalValueState state = DatatypePhysicalValueState::unknown;
  const byte* payload_data = nullptr;
  std::size_t payload_bytes = 0;
};

struct DatatypePhysicalAllocationFreeViewResult {
  u16 abi_version = 1;
  Status status;
  DatatypeBinaryDiagnosticView diagnostic;
  DatatypePhysicalValueView value;
  std::size_t bytes_written = 0;
  bool ok() const noexcept {
    return abi_version == 1 && diagnostic.abi_version == 1 && status.ok() &&
           diagnostic.status.ok() && diagnostic.diagnostic_code.empty();
  }
};

struct DatatypePhysicalEncodingResult {
  Status status;
  std::vector<byte> bytes;
  DatatypePhysicalValue value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* DatatypePhysicalValueStateName(DatatypePhysicalValueState state);
DatatypePhysicalValue SampleDatatypePhysicalValueForLayout(
    const DatatypeStorageLayout& layout);
DatatypePhysicalEncodingResult EncodeDatatypePhysicalValue(
    const DatatypePhysicalValue& value);
DatatypePhysicalEncodingResult DecodeDatatypePhysicalValue(
    const byte* data,
    u64 size);

// Structural-only SBDPV001 access for a containing profile-aware adapter.
// These allocation-free functions confer no descriptor receipt, datatype
// profile, canonical payload, page, overflow, or MGA authority.
DatatypePhysicalAllocationFreeViewResult
ValidateDatatypePhysicalStructuralValueViewNoAlloc(
    const DatatypePhysicalValueView& value) noexcept;
DatatypePhysicalAllocationFreeViewResult
EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
    const DatatypePhysicalValueView& value, byte* destination,
    std::size_t destination_bytes) noexcept;
DatatypePhysicalAllocationFreeViewResult
DecodeDatatypePhysicalStructuralValueViewNoAlloc(
    const byte* data, std::size_t size) noexcept;

}  // namespace scratchbird::core::datatypes
