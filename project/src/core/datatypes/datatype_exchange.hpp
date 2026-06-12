// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_descriptor.hpp"

#include <array>
#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

inline constexpr u32 kSerializedDatatypeDescriptorBytes = 128;
inline constexpr std::array<byte, 8> kDatatypeDescriptorMagic = {'S', 'B', 'D', 'T', 'V', '0', '0', '1'};

using SerializedDatatypeDescriptor = std::array<byte, kSerializedDatatypeDescriptorBytes>;

enum class ReferenceDialectId : u16 {
  native_sbsql_v3,
  firebird,
  interbase,
  postgresql,
  mysql,
  mariadb,
  sqlite,
  oracle,
  sql_server,
  db2,
  unknown
};

enum class ConversionDiagnosticKind : u16 {
  exact,
  widening,
  narrowing,
  precision_loss_possible,
  incompatible,
  reference_label_unmapped,
  unsupported
};

struct DatatypeDescriptorSerializationResult {
  Status status;
  SerializedDatatypeDescriptor serialized{};
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ReferenceTypeLabelMapping {
  ReferenceDialectId dialect = ReferenceDialectId::unknown;
  std::string reference_label;
  CanonicalTypeId target_type_id = CanonicalTypeId::unknown;
  bool placeholder_only = true;
  bool parser_must_confirm_descriptor = true;
};

struct DatatypeConversionDiagnosticResult {
  Status status;
  ConversionDiagnosticKind kind = ConversionDiagnosticKind::unsupported;
  CanonicalTypeId source_type_id = CanonicalTypeId::unknown;
  CanonicalTypeId target_type_id = CanonicalTypeId::unknown;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* ReferenceDialectName(ReferenceDialectId dialect);
const char* ConversionDiagnosticKindName(ConversionDiagnosticKind kind);
DatatypeDescriptorSerializationResult SerializeDatatypeDescriptor(const DatatypeDescriptor& descriptor);
DatatypeDescriptorResult ParseDatatypeDescriptor(const SerializedDatatypeDescriptor& serialized);
const std::vector<ReferenceTypeLabelMapping>& BuiltinReferenceTypeLabelPlaceholders();
std::vector<ReferenceTypeLabelMapping> ReferenceTypeLabelPlaceholdersFor(ReferenceDialectId dialect);
DatatypeDescriptorResult ResolveReferenceTypeLabelPlaceholder(ReferenceDialectId dialect, const std::string& reference_label);
DatatypeConversionDiagnosticResult DescribeDatatypeConversion(CanonicalTypeId source_type_id,
                                                              CanonicalTypeId target_type_id);

}  // namespace scratchbird::core::datatypes
