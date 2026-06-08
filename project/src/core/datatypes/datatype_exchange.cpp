// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_exchange.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::Subsystem;

inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetTypeId = 8;
inline constexpr u32 kOffsetFamily = 12;
inline constexpr u32 kOffsetWidthClass = 14;
inline constexpr u32 kOffsetBitWidth = 16;
inline constexpr u32 kOffsetPrecision = 20;
inline constexpr u32 kOffsetScale = 24;
inline constexpr u32 kOffsetFlags = 28;
inline constexpr u32 kOffsetStableName = 32;
inline constexpr u32 kStableNameBytes = 64;

namespace DescriptorFlag {
inline constexpr u32 nullable_allowed = 1u << 0;
inline constexpr u32 descriptor_authoritative = 1u << 1;
inline constexpr u32 donor_name_is_alias_only = 1u << 2;
inline constexpr u32 requires_mandatory_library = 1u << 3;
}  // namespace DescriptorFlag

Status DatatypeExchangeOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status DatatypeExchangeWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::datatypes};
}

Status DatatypeExchangeErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

std::string NormalizeDonorLabel(const std::string& label) {
  std::string normalized;
  normalized.reserve(label.size());
  for (char value : label) {
    if (value >= 'a' && value <= 'z') {
      normalized.push_back(static_cast<char>(value - 'a' + 'A'));
    } else {
      normalized.push_back(value);
    }
  }
  return normalized;
}

void StoreStableName(SerializedDatatypeDescriptor& serialized, const std::string& stable_name) {
  const u32 limit = stable_name.size() < (kStableNameBytes - 1) ? static_cast<u32>(stable_name.size())
                                                               : (kStableNameBytes - 1);
  for (u32 index = 0; index < limit; ++index) {
    serialized[kOffsetStableName + index] = static_cast<byte>(stable_name[index]);
  }
  serialized[kOffsetStableName + limit] = 0;
}

std::string LoadStableName(const SerializedDatatypeDescriptor& serialized) {
  u32 length = 0;
  while (length < kStableNameBytes && serialized[kOffsetStableName + length] != 0) {
    ++length;
  }
  return std::string(reinterpret_cast<const char*>(serialized.data() + kOffsetStableName), length);
}

u32 DescriptorFlags(const DatatypeDescriptor& descriptor) {
  u32 flags = 0;
  if (descriptor.nullable_allowed) {
    flags |= DescriptorFlag::nullable_allowed;
  }
  if (descriptor.descriptor_authoritative) {
    flags |= DescriptorFlag::descriptor_authoritative;
  }
  if (descriptor.donor_name_is_alias_only) {
    flags |= DescriptorFlag::donor_name_is_alias_only;
  }
  if (descriptor.requires_mandatory_library) {
    flags |= DescriptorFlag::requires_mandatory_library;
  }
  return flags;
}

DiagnosticRecord MakeDatatypeExchangeDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.exchange");
}

DonorTypeLabelMapping Mapping(DonorDialectId dialect, std::string donor_label, CanonicalTypeId target_type_id) {
  DonorTypeLabelMapping mapping;
  mapping.dialect = dialect;
  mapping.donor_label = std::move(donor_label);
  mapping.target_type_id = target_type_id;
  return mapping;
}

bool IsIntegerFamily(TypeFamily family) {
  return family == TypeFamily::signed_integer || family == TypeFamily::unsigned_integer;
}

}  // namespace

const char* DonorDialectName(DonorDialectId dialect) {
  switch (dialect) {
    case DonorDialectId::native_sbsql_v3: return "native_sbsql_v3";
    case DonorDialectId::firebird: return "firebird";
    case DonorDialectId::interbase: return "interbase";
    case DonorDialectId::postgresql: return "postgresql";
    case DonorDialectId::mysql: return "mysql";
    case DonorDialectId::mariadb: return "mariadb";
    case DonorDialectId::sqlite: return "sqlite";
    case DonorDialectId::oracle: return "oracle";
    case DonorDialectId::sql_server: return "sql_server";
    case DonorDialectId::db2: return "db2";
    case DonorDialectId::unknown: return "unknown";
  }
  return "unknown";
}

const char* ConversionDiagnosticKindName(ConversionDiagnosticKind kind) {
  switch (kind) {
    case ConversionDiagnosticKind::exact: return "exact";
    case ConversionDiagnosticKind::widening: return "widening";
    case ConversionDiagnosticKind::narrowing: return "narrowing";
    case ConversionDiagnosticKind::precision_loss_possible: return "precision_loss_possible";
    case ConversionDiagnosticKind::incompatible: return "incompatible";
    case ConversionDiagnosticKind::donor_label_unmapped: return "donor_label_unmapped";
    case ConversionDiagnosticKind::unsupported: return "unsupported";
  }
  return "unsupported";
}

DatatypeDescriptorSerializationResult SerializeDatatypeDescriptor(const DatatypeDescriptor& descriptor) {
  DatatypeDescriptorSerializationResult result;
  DatatypeDescriptorResult validation = ValidateDatatypeDescriptor(descriptor);
  if (!validation.ok()) {
    result.status = validation.status;
    result.diagnostic = validation.diagnostic;
    return result;
  }

  result.status = DatatypeExchangeOkStatus();
  std::memcpy(result.serialized.data() + kOffsetMagic, kDatatypeDescriptorMagic.data(), kDatatypeDescriptorMagic.size());
  StoreLittle32(result.serialized.data() + kOffsetTypeId, static_cast<u32>(descriptor.type_id));
  StoreLittle16(result.serialized.data() + kOffsetFamily, static_cast<u16>(descriptor.family));
  StoreLittle16(result.serialized.data() + kOffsetWidthClass, static_cast<u16>(descriptor.width_class));
  StoreLittle32(result.serialized.data() + kOffsetBitWidth, descriptor.bit_width);
  StoreLittle32(result.serialized.data() + kOffsetPrecision, descriptor.default_precision);
  StoreLittle32(result.serialized.data() + kOffsetScale, descriptor.default_scale);
  StoreLittle32(result.serialized.data() + kOffsetFlags, DescriptorFlags(descriptor));
  StoreStableName(result.serialized, descriptor.stable_name);
  return result;
}

DatatypeDescriptorResult ParseDatatypeDescriptor(const SerializedDatatypeDescriptor& serialized) {
  DatatypeDescriptorResult result;
  result.status = DatatypeExchangeOkStatus();

  if (std::memcmp(serialized.data() + kOffsetMagic, kDatatypeDescriptorMagic.data(), kDatatypeDescriptorMagic.size()) != 0) {
    result.status = DatatypeExchangeErrorStatus();
    result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                       "SB-DATATYPE-SERIALIZED-BAD-MAGIC",
                                                       "datatype.serialized.bad_magic");
    return result;
  }

  const auto type_id = static_cast<CanonicalTypeId>(LoadLittle32(serialized.data() + kOffsetTypeId));
  DatatypeDescriptorResult lookup = LookupDatatypeDescriptor(type_id);
  if (!lookup.ok()) {
    return lookup;
  }

  const auto family = static_cast<TypeFamily>(LoadLittle16(serialized.data() + kOffsetFamily));
  const auto width_class = static_cast<TypeWidthClass>(LoadLittle16(serialized.data() + kOffsetWidthClass));
  const u32 bit_width = LoadLittle32(serialized.data() + kOffsetBitWidth);
  const u32 precision = LoadLittle32(serialized.data() + kOffsetPrecision);
  const u32 scale = LoadLittle32(serialized.data() + kOffsetScale);
  const u32 flags = LoadLittle32(serialized.data() + kOffsetFlags);
  const std::string stable_name = LoadStableName(serialized);

  const DatatypeDescriptor& descriptor = lookup.descriptor;
  const u32 expected_flags = DescriptorFlags(descriptor);
  if (family != descriptor.family || width_class != descriptor.width_class || bit_width != descriptor.bit_width ||
      precision != descriptor.default_precision || scale != descriptor.default_scale || flags != expected_flags ||
      stable_name != descriptor.stable_name) {
    result.status = DatatypeExchangeErrorStatus();
    result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                       "SB-DATATYPE-SERIALIZED-DESCRIPTOR-MISMATCH",
                                                       "datatype.serialized.descriptor_mismatch",
                                                       CanonicalTypeName(type_id));
    return result;
  }

  result.descriptor = descriptor;
  return result;
}

const std::vector<DonorTypeLabelMapping>& BuiltinDonorTypeLabelPlaceholders() {
  static const std::vector<DonorTypeLabelMapping> mappings = {
      Mapping(DonorDialectId::native_sbsql_v3, "BOOLEAN", CanonicalTypeId::boolean),
      Mapping(DonorDialectId::native_sbsql_v3, "INT128", CanonicalTypeId::int128),
      Mapping(DonorDialectId::native_sbsql_v3, "UINT128", CanonicalTypeId::uint128),
      Mapping(DonorDialectId::native_sbsql_v3, "REAL128", CanonicalTypeId::real128),
      Mapping(DonorDialectId::native_sbsql_v3, "UUID", CanonicalTypeId::uuid),
      Mapping(DonorDialectId::firebird, "SMALLINT", CanonicalTypeId::int16),
      Mapping(DonorDialectId::firebird, "INTEGER", CanonicalTypeId::int32),
      Mapping(DonorDialectId::firebird, "BIGINT", CanonicalTypeId::int64),
      Mapping(DonorDialectId::firebird, "DECFLOAT", CanonicalTypeId::real128),
      Mapping(DonorDialectId::postgresql, "SMALLINT", CanonicalTypeId::int16),
      Mapping(DonorDialectId::postgresql, "INTEGER", CanonicalTypeId::int32),
      Mapping(DonorDialectId::postgresql, "BIGINT", CanonicalTypeId::int64),
      Mapping(DonorDialectId::postgresql, "UUID", CanonicalTypeId::uuid),
      Mapping(DonorDialectId::mysql, "TINYINT", CanonicalTypeId::int8),
      Mapping(DonorDialectId::mysql, "SMALLINT", CanonicalTypeId::int16),
      Mapping(DonorDialectId::mysql, "INT", CanonicalTypeId::int32),
      Mapping(DonorDialectId::mysql, "BIGINT", CanonicalTypeId::int64),
      Mapping(DonorDialectId::sqlite, "INTEGER", CanonicalTypeId::int64),
      Mapping(DonorDialectId::oracle, "NUMBER", CanonicalTypeId::decimal),
      Mapping(DonorDialectId::sql_server, "UNIQUEIDENTIFIER", CanonicalTypeId::uuid),
      Mapping(DonorDialectId::db2, "BIGINT", CanonicalTypeId::int64),
  };
  return mappings;
}

std::vector<DonorTypeLabelMapping> DonorTypeLabelPlaceholdersFor(DonorDialectId dialect) {
  std::vector<DonorTypeLabelMapping> matches;
  for (const DonorTypeLabelMapping& mapping : BuiltinDonorTypeLabelPlaceholders()) {
    if (mapping.dialect == dialect) {
      matches.push_back(mapping);
    }
  }
  return matches;
}

DatatypeDescriptorResult ResolveDonorTypeLabelPlaceholder(DonorDialectId dialect, const std::string& donor_label) {
  const std::string normalized = NormalizeDonorLabel(donor_label);
  for (const DonorTypeLabelMapping& mapping : BuiltinDonorTypeLabelPlaceholders()) {
    if (mapping.dialect == dialect && mapping.donor_label == normalized) {
      return LookupDatatypeDescriptor(mapping.target_type_id);
    }
  }

  DatatypeDescriptorResult result;
  result.status = DatatypeExchangeWarningStatus();
  result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                     "SB-DATATYPE-DONOR-LABEL-UNMAPPED",
                                                     "datatype.donor_label_unmapped",
                                                     DonorDialectName(dialect));
  return result;
}

DatatypeConversionDiagnosticResult DescribeDatatypeConversion(CanonicalTypeId source_type_id,
                                                              CanonicalTypeId target_type_id) {
  DatatypeConversionDiagnosticResult result;
  result.status = DatatypeExchangeOkStatus();
  result.source_type_id = source_type_id;
  result.target_type_id = target_type_id;

  DatatypeDescriptorResult source = LookupDatatypeDescriptor(source_type_id);
  DatatypeDescriptorResult target = LookupDatatypeDescriptor(target_type_id);
  if (!source.ok() || !target.ok()) {
    result.status = DatatypeExchangeErrorStatus();
    result.kind = ConversionDiagnosticKind::unsupported;
    result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                       "SB-DATATYPE-CONVERSION-UNKNOWN-TYPE",
                                                       "datatype.conversion.unknown_type");
    return result;
  }

  if (source_type_id == target_type_id) {
    result.kind = ConversionDiagnosticKind::exact;
    return result;
  }

  if (source.descriptor.family == target.descriptor.family &&
      (IsIntegerFamily(source.descriptor.family) || source.descriptor.family == TypeFamily::real)) {
    result.kind = source.descriptor.bit_width <= target.descriptor.bit_width ? ConversionDiagnosticKind::widening
                                                                             : ConversionDiagnosticKind::narrowing;
    if (result.kind == ConversionDiagnosticKind::narrowing) {
      result.status = DatatypeExchangeWarningStatus();
      result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                         "SB-DATATYPE-CONVERSION-NARROWING",
                                                         "datatype.conversion.narrowing",
                                                         CanonicalTypeName(source_type_id));
    }
    return result;
  }

  if (IsIntegerFamily(source.descriptor.family) && target.descriptor.family == TypeFamily::real) {
    result.kind = ConversionDiagnosticKind::precision_loss_possible;
    result.status = DatatypeExchangeWarningStatus();
    result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                       "SB-DATATYPE-CONVERSION-PRECISION-LOSS-POSSIBLE",
                                                       "datatype.conversion.precision_loss_possible",
                                                       CanonicalTypeName(source_type_id));
    return result;
  }

  result.status = DatatypeExchangeErrorStatus();
  result.kind = ConversionDiagnosticKind::incompatible;
  result.diagnostic = MakeDatatypeExchangeDiagnostic(result.status,
                                                     "SB-DATATYPE-CONVERSION-INCOMPATIBLE",
                                                     "datatype.conversion.incompatible",
                                                     CanonicalTypeName(source_type_id));
  return result;
}

}  // namespace scratchbird::core::datatypes
