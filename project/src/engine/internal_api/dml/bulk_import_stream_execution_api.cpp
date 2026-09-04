// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/bulk_import_stream_execution_api.hpp"

#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"
#include "datatype_operations.hpp"
#include "dml/dml_executable_trigger_runtime.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_executor_availability_registry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using BulkUuid = engine::sblr::BulkImportUuid;
using BulkSha = engine::sblr::BulkImportSha;

constexpr std::string_view kConverterUuid =
    "019d0000-0000-7000-8000-00000000b775";
constexpr std::uint64_t kConverterGeneration = 1;

EngineExecuteBulkImportStreamResultV1 Failure(std::string code,
                                               std::string key,
                                               std::string detail) {
  EngineExecuteBulkImportStreamResultV1 result;
  result.diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(key), std::move(detail), true);
  return result;
}

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic SuccessDiagnostic() {
  EngineApiDiagnostic diagnostic;
  diagnostic.error = false;
  return diagnostic;
}

bool Nonzero(std::span<const std::uint8_t> bytes) {
  return std::any_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t value) { return value != 0; });
}

std::uint32_t ReadU32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
  }
  return value;
}

std::uint64_t ReadU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void AppendU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void StoreU64(std::array<std::uint8_t, 136>* out, std::size_t offset,
              std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    (*out)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8));
  }
}

BulkSha Hash(std::string_view domain,
             std::span<const std::uint8_t> material) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(domain.size() + material.size());
  bytes.insert(bytes.end(), domain.begin(), domain.end());
  bytes.insert(bytes.end(), material.begin(), material.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
  return digest.ok() ? digest.digest : BulkSha{};
}

BulkSha HashMaterial(const std::vector<std::uint8_t>& material) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? digest.digest : BulkSha{};
}

bool ParseUuidText(std::string_view text, BulkUuid* output) {
  if (output == nullptr) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            output->begin());
  return true;
}

std::string UuidText(const BulkUuid& uuid) {
  scratchbird::core::platform::Uuid value;
  std::copy(uuid.begin(), uuid.end(), value.bytes.begin());
  return scratchbird::core::uuid::UuidToString(value);
}

void AppendUuid(std::vector<std::uint8_t>* out, const BulkUuid& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

bool AppendUuidText(std::vector<std::uint8_t>* out, std::string_view text) {
  BulkUuid uuid{};
  if (!ParseUuidText(text, &uuid)) return false;
  AppendUuid(out, uuid);
  return true;
}

bool CanonicalUtf8(std::string_view value, bool allow_nul = false) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<std::uint8_t>(value[offset++]);
    if (first == 0 && !allow_nul) return false;
    if (first < 0x80) continue;
    unsigned continuation = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (continuation > value.size() - offset) return false;
    for (unsigned index = 0; index < continuation; ++index) {
      const auto next = static_cast<std::uint8_t>(value[offset++]);
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff) {
      return false;
    }
  }
  return true;
}

scratchbird::core::datatypes::CanonicalTypeId
CanonicalBulkColumnType(std::string_view type_name) {
  return scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
      std::string(type_name));
}

bool AdmittedBulkColumnType(std::string_view type_name) {
  const auto type = CanonicalBulkColumnType(type_name);
  return type == scratchbird::core::datatypes::CanonicalTypeId::int32 ||
         type == scratchbird::core::datatypes::CanonicalTypeId::character;
}

bool AppendLp16(std::vector<std::uint8_t>* out, std::string_view value,
                bool allow_empty = false) {
  if ((!allow_empty && value.empty()) || value.size() > 65535 ||
      !CanonicalUtf8(value)) {
    return false;
  }
  AppendU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

BulkUuid DerivedUuid(const BulkUuid& stream_uuid, std::string_view domain,
                     std::span<const std::uint8_t> material) {
  const BulkSha digest = Hash(domain, material);
  BulkUuid result{};
  std::copy_n(stream_uuid.begin(), 6, result.begin());
  std::copy_n(digest.begin(), 10, result.begin() + 6);
  result[6] = static_cast<std::uint8_t>((result[6] & 0x0f) | 0x70);
  result[8] = static_cast<std::uint8_t>((result[8] & 0x3f) | 0x80);
  return result;
}

struct DecodedAuthority {
  engine::sblr::SblrBulkImportStreamDescriptorV1 descriptor;
  BulkUuid stream_uuid{};
  BulkImportStreamEntry entry;
};

bool ExactBody(const engine::sblr::SblrBulkImportStreamDescriptorV1& value,
               const BulkImportStreamAllocation& allocation) {
  const auto& body = value.canonical_body;
  auto exact = [&](std::size_t offset, const auto& expected) {
    return std::equal(expected.begin(), expected.end(), body.begin() + offset);
  };
  return exact(0, allocation.authenticated_receipt_uuid) &&
         ReadU64(body.data() + 16) == allocation.structural_occurrence &&
         ReadU32(body.data() + 24) == allocation.import_occurrence &&
         ReadU32(body.data() + 28) == (allocation.cluster_bound ? 1u : 0u) &&
         exact(32, allocation.stream_uuid) &&
         ReadU64(body.data() + 48) == allocation.stream_generation &&
         exact(56, allocation.target_relation_uuid) &&
         ReadU64(body.data() + 72) == allocation.target_relation_generation &&
         exact(80, allocation.owning_transaction_uuid) &&
         ReadU64(body.data() + 96) == allocation.owning_local_transaction_id &&
         exact(104, allocation.statement_snapshot_uuid) &&
         exact(120, allocation.catalog_epoch_uuid) &&
         ReadU64(body.data() + 136) == allocation.catalog_generation &&
         exact(144, allocation.security_context_uuid) &&
         ReadU64(body.data() + 160) == allocation.security_epoch &&
         exact(168, allocation.policy_snapshot_uuid) &&
         ReadU64(body.data() + 184) == allocation.policy_generation &&
         exact(192, allocation.route_snapshot_uuid) &&
         ReadU64(body.data() + 208) == allocation.route_generation &&
         exact(216, allocation.recovery_operation_uuid) &&
         ReadU64(body.data() + 232) == allocation.recovery_generation &&
         exact(240, allocation.row_shape_uuid) &&
         ReadU64(body.data() + 256) == allocation.row_shape_generation &&
         exact(264, allocation.column_descriptor_set_sha256) &&
         exact(296, allocation.import_policy_bundle_sha256) &&
         exact(328, allocation.resource_grant_uuid) &&
         ReadU64(body.data() + 344) == allocation.resource_grant_generation &&
         exact(352, allocation.cluster_fence_uuid) &&
         value.evidence == allocation.descriptor_evidence &&
         value.availability_generation ==
             allocation.executor_availability_generation;
}

bool DecodeAuthority(const EngineExecuteBulkImportStreamRequestV1& request,
                     DecodedAuthority* output,
                     EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr || request.registry == nullptr ||
      request.canonical_biro.size() !=
          engine::sblr::BulkImportWireLayout::descriptor_size) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                               "sblr.bulk_import_stream.execution_operand_invalid",
                               "one exact BIRO and database registry are required");
    }
    return false;
  }
  engine::sblr::SblrBulkImportStreamDescriptorV1 descriptor;
  std::string detail;
  if (!engine::sblr::DecodeSblrBulkImportStreamDescriptorV1(
          request.canonical_biro.data(), request.canonical_biro.size(),
          &descriptor, &detail, true) ||
      !Nonzero(descriptor.evidence)) {
    *diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                             "sblr.bulk_import_stream.execution_operand_invalid",
                             detail.empty() ? "BIRO failed canonical decoding"
                                            : detail);
    return false;
  }
  BulkUuid stream{};
  std::copy_n(descriptor.canonical_body.begin() + 32, 16, stream.begin());
  BulkImportStreamEntry entry;
  const auto recovered = request.registry->Recover(stream, &entry);
  if (!recovered.ok) {
    *diagnostic = Diagnostic("BULK.IMPORT.RECOVERY_CONFLICT",
                             "sblr.bulk_import_stream.registry_recovery_failed",
                             recovered.error);
    return false;
  }
  if (!ExactBody(descriptor, entry.allocation)) {
    *diagnostic = Diagnostic("MGA.AUTHORITY_MISMATCH",
                             "sblr.bulk_import_stream.descriptor_authority_mismatch",
                             "BIRO differs from the durable engine allocation");
    return false;
  }
  output->descriptor = descriptor;
  output->stream_uuid = stream;
  output->entry = std::move(entry);
  return true;
}

bool ExactContext(const EngineRequestContext& context,
                  const BulkImportStreamAllocation& allocation,
                  EngineApiDiagnostic* diagnostic) {
  BulkUuid receipt{}, transaction{}, snapshot{}, catalog{}, security{}, resource{};
  const bool parsed =
      ParseUuidText(context.statement_receipt_uuid.canonical, &receipt) &&
      ParseUuidText(context.transaction_uuid.canonical, &transaction) &&
      ParseUuidText(context.statement_snapshot_uuid.canonical, &snapshot) &&
      ParseUuidText(context.catalog_epoch_uuid.canonical, &catalog) &&
      ParseUuidText(context.authorization_context.authority_uuid.canonical,
                    &security) &&
      ParseUuidText(context.resource_admission_uuid.canonical, &resource);
  if (!parsed || context.local_transaction_id == 0) {
    *diagnostic = Diagnostic("MGA.TRANSACTION_INVALID",
                             "sblr.bulk_import_stream.execution_context_invalid",
                             "live transaction and receipt authority are required");
    return false;
  }
  if (receipt != allocation.authenticated_receipt_uuid ||
      transaction != allocation.owning_transaction_uuid ||
      context.local_transaction_id != allocation.owning_local_transaction_id ||
      snapshot != allocation.statement_snapshot_uuid ||
      catalog != allocation.catalog_epoch_uuid ||
      context.catalog_generation_id != allocation.catalog_generation ||
      security != allocation.security_context_uuid ||
      context.authorization_context.security_epoch != allocation.security_epoch ||
      resource != allocation.resource_grant_uuid ||
      context.resource_epoch != allocation.resource_grant_generation) {
    *diagnostic = Diagnostic("MGA.AUTHORITY_MISMATCH",
                             "sblr.bulk_import_stream.execution_context_stale",
                             "live receipt, MGA, catalog, security, or resource authority changed");
    return false;
  }
  return true;
}

bool ExactExecutorAvailability(
    const EngineRequestContext& context,
    const BulkImportStreamAllocation& allocation,
    EngineApiDiagnostic* diagnostic) {
  if (diagnostic == nullptr) return false;
  SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = kSblrBulkImportStreamExecutorId;
  identity.opcode_code = kSblrBulkImportStreamOpcodeCode;
  identity.opcode_version = kSblrBulkImportStreamOpcodeVersion;
  identity.operand_descriptor_id =
      kSblrBulkImportStreamOperandDescriptorId;
  identity.result_descriptor_id =
      kSblrBulkImportStreamResultDescriptorId;
  identity.result_descriptor_version =
      kSblrBulkImportStreamResultDescriptorVersion;
  const auto availability =
      LoadSblrExecutorAvailabilitySnapshot(context, identity);
  if (!availability.ok || !availability.snapshot.installed ||
      availability.snapshot.generation == 0 ||
      availability.snapshot.generation !=
          allocation.executor_availability_generation) {
    *diagnostic = Diagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.bulk_import_stream.executor_availability_stale",
        "the exact opcode-775 executor availability row is absent or changed");
    return false;
  }
  return true;
}

bool ColumnDigest(const MgaRelationStorageDescriptor& descriptor,
                  BulkSha* output) {
  if (output == nullptr || descriptor.columns.empty() ||
      descriptor.columns.size() > 65535 ||
      descriptor.relation_generation == 0 ||
      descriptor.descriptor_generation == 0) {
    return false;
  }
  std::vector<const MgaRelationColumnStorageDescriptor*> columns;
  columns.reserve(descriptor.columns.size());
  for (const auto& column : descriptor.columns) columns.push_back(&column);
  std::sort(columns.begin(), columns.end(), [](const auto* left,
                                               const auto* right) {
    return left->ordinal < right->ordinal;
  });
  std::set<std::uint32_t> ordinals;
  std::set<std::string> column_uuids;
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamColumnDescriptorSet.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  if (!AppendUuidText(&material, descriptor.relation_uuid.canonical))
    return false;
  AppendU64(&material, descriptor.relation_generation);
  if (!AppendUuidText(&material, descriptor.descriptor_uuid.canonical))
    return false;
  AppendU64(&material, descriptor.descriptor_generation);
  AppendU32(&material, static_cast<std::uint32_t>(columns.size()));
  for (const auto* column : columns) {
    if (!ordinals.insert(column->ordinal).second ||
        !column_uuids.insert(column->column_uuid.canonical).second ||
        column->column_generation == 0) {
      return false;
    }
    AppendU32(&material, column->ordinal);
    if (!AppendUuidText(&material, column->column_uuid.canonical)) return false;
    AppendU64(&material, column->column_generation);
    if (!AppendLp16(&material, column->canonical_name_key) ||
        !AppendUuidText(&material,
                        column->value_descriptor.descriptor_uuid.canonical) ||
        !AppendLp16(&material, column->value_descriptor.descriptor_kind) ||
        !AppendLp16(&material,
                    column->value_descriptor.canonical_type_name) ||
        !CanonicalUtf8(column->value_descriptor.encoded_descriptor)) {
      return false;
    }
    const std::vector<std::uint8_t> encoded(
        column->value_descriptor.encoded_descriptor.begin(),
        column->value_descriptor.encoded_descriptor.end());
    const auto encoded_hash = HashMaterial(encoded);
    material.insert(material.end(), encoded_hash.begin(), encoded_hash.end());
    material.push_back(column->nullable ? 1 : 0);
    material.push_back(column->generated ? 1 : 0);
    material.push_back(column->identity_column ? 1 : 0);
    if (!AppendLp16(&material, column->storage_class) ||
        !AppendLp16(&material, column->charset_uuid, true) ||
        !AppendLp16(&material, column->collation_uuid, true)) {
      return false;
    }
    AppendU32(&material, column->character_length);
    AppendU64(&material, column->max_inline_bytes);
    if (!AppendLp16(&material, column->overflow_policy)) return false;
  }
  *output = HashMaterial(material);
  return Nonzero(*output);
}

bool DescriptorHasForbiddenDefaultOrConstraint(std::string_view encoded) {
  std::string lower(encoded);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
    return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
  });
  constexpr std::array<std::string_view, 12> forbidden{{
      "default=", "default_uuid=", "generated=", "identity=",
      "primary_key", "unique=", "foreign_key", "references=",
      "check=", "constraint_uuid=", "constraint_kind=", "constraint="}};
  return std::any_of(forbidden.begin(), forbidden.end(), [&](auto token) {
    return lower.find(token) != std::string::npos;
  });
}

struct TargetAuthority {
  MgaRelationStorageDescriptor descriptor;
  std::vector<const MgaRelationColumnStorageDescriptor*> columns;
};

bool LoadTargetAuthority(const EngineRequestContext& context,
                         const BulkImportStreamAllocation& allocation,
                         TargetAuthority* output,
                         EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr) return false;
  const std::string relation_uuid = UuidText(allocation.target_relation_uuid);
  const auto loaded = LoadMgaRelationStorageDescriptor(context, relation_uuid);
  if (!loaded.ok) {
    *diagnostic = loaded.diagnostic.error
                      ? loaded.diagnostic
                      : Diagnostic("BULK.IMPORT.TARGET_NOT_ELIGIBLE",
                                   "sblr.bulk_import_stream.target_missing",
                                   "the frozen target descriptor is unavailable");
    return false;
  }
  const auto& descriptor = loaded.descriptor;
  BulkUuid relation{}, row_shape{};
  if (!ParseUuidText(descriptor.relation_uuid.canonical, &relation) ||
      !ParseUuidText(descriptor.descriptor_uuid.canonical, &row_shape) ||
      relation != allocation.target_relation_uuid ||
      descriptor.relation_generation != allocation.target_relation_generation ||
      row_shape != allocation.row_shape_uuid ||
      descriptor.descriptor_generation != allocation.row_shape_generation ||
      descriptor.relation_kind != "table" || descriptor.columns.empty() ||
      descriptor.columns.size() > allocation.effective_maximum_target_columns ||
      !descriptor.indexes.empty() ||
      dml_trigger_runtime::HasActiveTableTriggerDescriptors(
          context, descriptor.relation_uuid.canonical)) {
    *diagnostic = Diagnostic(
        "BULK.IMPORT.TARGET_NOT_ELIGIBLE",
        "sblr.bulk_import_stream.target_authority_changed",
        "target identity, generation, row shape, indexes, or triggers are not v1 eligible");
    return false;
  }
  BulkSha columns_digest{};
  if (!ColumnDigest(descriptor, &columns_digest) ||
      columns_digest != allocation.column_descriptor_set_sha256) {
    *diagnostic = Diagnostic("BULK.IMPORT.GENERATION_CONFLICT",
                             "sblr.bulk_import_stream.column_authority_changed",
                             "the ordered target column set changed after bind");
    return false;
  }
  output->descriptor = descriptor;
  output->columns.reserve(output->descriptor.columns.size());
  for (const auto& column : output->descriptor.columns) {
    if (column.generated || column.identity_column ||
        !AdmittedBulkColumnType(
            column.value_descriptor.canonical_type_name) ||
        DescriptorHasForbiddenDefaultOrConstraint(
            column.value_descriptor.encoded_descriptor)) {
      *diagnostic = Diagnostic(
          "BULK.IMPORT.TARGET_NOT_ELIGIBLE",
          "sblr.bulk_import_stream.column_converter_unavailable",
          "v1 requires unconstrained int32 or character columns with no defaults");
      return false;
    }
    output->columns.push_back(&column);
  }
  std::sort(output->columns.begin(), output->columns.end(),
            [](const auto* left, const auto* right) {
              return left->ordinal < right->ordinal;
            });
  return true;
}

BulkSha ConverterEvidence(const MgaRelationColumnStorageDescriptor& column) {
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamTextConverter.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  if (!AppendUuidText(&material, kConverterUuid)) {
    return {};
  }
  AppendU64(&material, kConverterGeneration);
  if (!AppendUuidText(&material, column.column_uuid.canonical)) {
    return {};
  }
  AppendU64(&material, column.column_generation);
  if (!AppendUuidText(&material,
                      column.value_descriptor.descriptor_uuid.canonical) ||
      !AppendLp16(&material,
                  column.value_descriptor.canonical_type_name)) {
    return {};
  }
  const std::vector<std::uint8_t> encoded(
      column.value_descriptor.encoded_descriptor.begin(),
      column.value_descriptor.encoded_descriptor.end());
  const auto encoded_hash = HashMaterial(encoded);
  material.insert(material.end(), encoded_hash.begin(), encoded_hash.end());
  return HashMaterial(material);
}

BulkSha EmptySetHash(std::string_view domain);

BulkSha PolicyBundleDigest(const BulkImportStreamAllocation& allocation,
                           const TargetAuthority& target) {
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamPolicyBundle.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  material.insert(material.end(), allocation.syntax_demand_sha256.begin(),
                  allocation.syntax_demand_sha256.end());
  AppendUuid(&material, allocation.target_relation_uuid);
  AppendU64(&material, allocation.target_relation_generation);
  AppendUuid(&material, allocation.row_shape_uuid);
  AppendU64(&material, allocation.row_shape_generation);
  material.insert(material.end(),
                  allocation.column_descriptor_set_sha256.begin(),
                  allocation.column_descriptor_set_sha256.end());
  AppendUuid(&material, allocation.catalog_epoch_uuid);
  AppendU64(&material, allocation.catalog_generation);
  AppendUuid(&material, allocation.security_context_uuid);
  AppendU64(&material, allocation.security_epoch);
  AppendUuid(&material, allocation.resource_grant_uuid);
  AppendU64(&material, allocation.resource_grant_generation);
  constexpr std::array<std::string_view, 4> empty_set_domains{{
      "ScratchBird.BulkImportStreamDefaultDescriptorSet.V1",
      "ScratchBird.BulkImportStreamConstraintSet.V1",
      "ScratchBird.BulkImportStreamTriggerSet.V1",
      "ScratchBird.BulkImportStreamIndexSet.V1",
  }};
  for (const auto empty_domain : empty_set_domains) {
    const auto empty_hash = EmptySetHash(empty_domain);
    if (!Nonzero(empty_hash)) return {};
    material.insert(material.end(), empty_hash.begin(), empty_hash.end());
  }
  if (target.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  AppendU32(&material, static_cast<std::uint32_t>(target.columns.size()));
  for (const auto* column : target.columns) {
    const auto converter_evidence = ConverterEvidence(*column);
    if (!Nonzero(converter_evidence) ||
        !AppendUuidText(&material, column->column_uuid.canonical)) {
      return {};
    }
    AppendU64(&material, column->column_generation);
    if (!AppendUuidText(
            &material, column->value_descriptor.descriptor_uuid.canonical) ||
        !AppendUuidText(&material, kConverterUuid)) {
      return {};
    }
    AppendU64(&material, kConverterGeneration);
    material.insert(material.end(), converter_evidence.begin(),
                    converter_evidence.end());
  }
  return HashMaterial(material);
}

struct ParsedField {
  bool sql_null = false;
  std::string text;
};

struct ParsedExecution {
  std::vector<EngineRowValue> rows;
  std::vector<std::string> shared_field_order;
  std::vector<BulkSha> row_value_hashes;
};

bool ParseCanonicalInt32(std::string_view text, std::int32_t* output) {
  if (output == nullptr || text.empty() || text.front() == '+' ||
      text.front() == ' ' || text.back() == ' ' || text == "-0" ||
      (text.size() > 1 && text.front() == '0') ||
      (text.size() > 2 && text.front() == '-' && text[1] == '0')) {
    return false;
  }
  std::int32_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text) {
    return false;
  }
  *output = value;
  return true;
}

BulkSha TypedFieldVectorHash(
    const std::vector<const MgaRelationColumnStorageDescriptor*>& columns,
    const EngineRowValue& row) {
  if (columns.size() != row.fields.size() ||
      columns.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamTypedFieldVector.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  AppendU32(&material, static_cast<std::uint32_t>(columns.size()));
  for (std::size_t index = 0; index < columns.size(); ++index) {
    const auto& column = *columns[index];
    const auto& typed = row.fields[index].second;
    AppendU32(&material, column.ordinal);
    if (!AppendUuidText(&material, column.column_uuid.canonical) ||
        !AppendUuidText(&material,
                        column.value_descriptor.descriptor_uuid.canonical)) {
      return {};
    }
    material.push_back(typed.isSqlNull() ? 1 : 0);
    if (typed.isSqlNull()) {
      AppendU64(&material, 0);
    } else {
      AppendU64(&material,
                static_cast<std::uint64_t>(typed.binary_value.size()));
      material.insert(material.end(), typed.binary_value.begin(),
                      typed.binary_value.end());
    }
  }
  return HashMaterial(material);
}

std::size_t Utf8ScalarCount(std::string_view text) {
  std::size_t count = 0;
  for (const unsigned char value : text) {
    if ((value & 0xc0u) != 0x80u) ++count;
  }
  return count;
}

class CanonicalCsvDecoder final {
 public:
  CanonicalCsvDecoder(
      const std::vector<const MgaRelationColumnStorageDescriptor*>& columns,
      const BulkImportStreamAllocation& allocation,
      const BulkSha& recovery_key)
      : columns_(columns), allocation_(allocation), recovery_key_(recovery_key) {
    output_.shared_field_order.reserve(columns.size());
    for (const auto* column : columns) {
      output_.shared_field_order.push_back(column->canonical_name_key);
    }
  }

  bool Consume(const std::uint8_t* bytes, std::size_t size,
               std::uint64_t offset) {
    if (failed_ || bytes == nullptr || offset != consumed_) {
      return Fail("stream_offset_discontinuity");
    }
    for (std::size_t index = 0; index < size; ++index) {
      const std::uint8_t byte = bytes[index];
      if (prefix_.size() < 3) {
        prefix_.push_back(byte);
        if (prefix_.size() == 3 && prefix_[0] == 0xef &&
            prefix_[1] == 0xbb && prefix_[2] == 0xbf) {
          return Fail("leading_utf8_bom_forbidden");
        }
      }
      ++consumed_;
      if (byte == 0) return Fail("nul_byte_forbidden");
      last_was_lf_ = false;
      switch (state_) {
        case State::start:
          quoted_field_ = false;
          if (byte == '"') {
            quoted_field_ = true;
            state_ = State::quoted;
          } else if (byte == ',') {
            if (!FinishField()) return false;
          } else if (byte == '\n') {
            if (!FinishField() || !FinishRow()) return false;
            last_was_lf_ = true;
          } else if (byte == '\r') {
            return Fail("unquoted_cr_forbidden");
          } else {
            field_.push_back(static_cast<char>(byte));
            state_ = State::unquoted;
          }
          break;
        case State::unquoted:
          if (byte == ',') {
            if (!FinishField()) return false;
          } else if (byte == '\n') {
            if (!FinishField() || !FinishRow()) return false;
            last_was_lf_ = true;
          } else if (byte == '"' || byte == '\r') {
            return Fail("unquoted_quote_or_cr_forbidden");
          } else {
            field_.push_back(static_cast<char>(byte));
          }
          break;
        case State::quoted:
          if (byte == '"') {
            state_ = State::quote_closed;
          } else {
            field_.push_back(static_cast<char>(byte));
          }
          break;
        case State::quote_closed:
          if (byte == '"') {
            field_.push_back('"');
            state_ = State::quoted;
          } else if (byte == ',') {
            if (!FinishField()) return false;
          } else if (byte == '\n') {
            if (!FinishField() || !FinishRow()) return false;
            last_was_lf_ = true;
          } else {
            return Fail("bytes_after_closing_quote");
          }
          break;
      }
      if (field_.size() > kBulkImportStreamMaximumChunkBytesV1 ||
          current_fields_.size() > columns_.size()) {
        return Fail("field_or_column_limit_exceeded");
      }
    }
    return true;
  }

  bool Finish(ParsedExecution* output) {
    if (output == nullptr || failed_ || consumed_ == 0 || !last_was_lf_ ||
        state_ != State::start || !field_.empty() ||
        !current_fields_.empty() || output_.rows.empty()) {
      return Fail("stream_requires_complete_nonempty_lf_terminated_records");
    }
    *output = std::move(output_);
    return true;
  }

  const std::string& error() const { return error_; }

 private:
  enum class State : std::uint8_t { start, unquoted, quoted, quote_closed };

  bool Fail(std::string reason) {
    failed_ = true;
    if (error_.empty()) error_ = std::move(reason);
    return false;
  }

  bool FinishField() {
    if (!CanonicalUtf8(field_)) return Fail("field_utf8_invalid");
    const bool requires_quotes =
        field_.find_first_of(",\"\r\n") != std::string::npos ||
        field_ == "\\N";
    if ((quoted_field_ && (field_.empty() || !requires_quotes)) ||
        (!quoted_field_ && requires_quotes && field_ != "\\N")) {
      return Fail("field_not_minimally_quoted");
    }
    ParsedField parsed;
    if (!quoted_field_ && field_ == "\\N") {
      parsed.sql_null = true;
    } else {
      parsed.text = std::move(field_);
    }
    current_fields_.push_back(std::move(parsed));
    field_.clear();
    quoted_field_ = false;
    state_ = State::start;
    return true;
  }

  bool FinishRow() {
    if (current_fields_.size() != columns_.size() ||
        output_.rows.size() >= allocation_.effective_maximum_rows) {
      return Fail("row_field_or_count_limit_invalid");
    }
    const std::uint64_t ordinal =
        static_cast<std::uint64_t>(output_.rows.size()) + 1;
    std::vector<std::uint8_t> row_identity_material;
    row_identity_material.insert(row_identity_material.end(),
                                 recovery_key_.begin(), recovery_key_.end());
    AppendU64(&row_identity_material, ordinal);
    AppendUuid(&row_identity_material, allocation_.target_relation_uuid);
    AppendU64(&row_identity_material, allocation_.target_relation_generation);
    const BulkUuid row_uuid = DerivedUuid(
        allocation_.stream_uuid,
        "ScratchBird.BulkImportStreamRowIdentity.V1",
        row_identity_material);
    EngineRowValue row;
    row.requested_row_uuid.canonical = UuidText(row_uuid);
    row.fields.reserve(columns_.size());
    for (std::size_t index = 0; index < columns_.size(); ++index) {
      const auto& column = *columns_[index];
      const auto& parsed = current_fields_[index];
      if (!Nonzero(ConverterEvidence(column))) {
        return Fail("converter_evidence_invalid");
      }
      EngineTypedValue typed;
      typed.descriptor = column.value_descriptor;
      if (parsed.sql_null) {
        if (!column.nullable) return Fail("null_not_admitted");
        typed.setState(EngineValueState::sql_null);
      } else if (CanonicalBulkColumnType(
                     column.value_descriptor.canonical_type_name) ==
                 scratchbird::core::datatypes::CanonicalTypeId::int32) {
        std::int32_t value = 0;
        if (!ParseCanonicalInt32(parsed.text, &value)) {
          return Fail("int32_conversion_invalid");
        }
        typed.encoded_value = parsed.text;
        const std::uint32_t bits = static_cast<std::uint32_t>(value);
        typed.binary_value.resize(4);
        for (unsigned byte = 0; byte < 4; ++byte) {
          typed.binary_value[byte] =
              static_cast<std::uint8_t>(bits >> (byte * 8));
        }
      } else if (CanonicalBulkColumnType(
                     column.value_descriptor.canonical_type_name) ==
                 scratchbird::core::datatypes::CanonicalTypeId::character) {
        if ((column.character_length != 0 &&
             Utf8ScalarCount(parsed.text) > column.character_length) ||
            parsed.text.size() > column.max_inline_bytes) {
          return Fail("character_constraint_invalid");
        }
        typed.encoded_value = parsed.text;
        typed.binary_value.assign(parsed.text.begin(), parsed.text.end());
      } else {
        return Fail("converter_not_admitted");
      }
      row.fields.push_back({column.canonical_name_key, std::move(typed)});
    }
    const auto row_hash = TypedFieldVectorHash(columns_, row);
    if (!Nonzero(row_hash)) return Fail("typed_field_hash_failed");
    output_.row_value_hashes.push_back(row_hash);
    output_.rows.push_back(std::move(row));
    current_fields_.clear();
    return true;
  }

  const std::vector<const MgaRelationColumnStorageDescriptor*>& columns_;
  const BulkImportStreamAllocation& allocation_;
  const BulkSha& recovery_key_;
  State state_ = State::start;
  bool quoted_field_ = false;
  bool failed_ = false;
  bool last_was_lf_ = false;
  std::uint64_t consumed_ = 0;
  std::vector<std::uint8_t> prefix_;
  std::string field_;
  std::vector<ParsedField> current_fields_;
  ParsedExecution output_;
  std::string error_;
};

bool ParseSealedStream(const EngineExecuteBulkImportStreamRequestV1& request,
                       const BulkImportStreamEntry& entry,
                       const TargetAuthority& target,
                       ParsedExecution* output,
                       EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr || request.registry == nullptr ||
      !Nonzero(entry.recovery_key_sha256)) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic("BULK.IMPORT.RECOVERY_CONFLICT",
                               "sblr.bulk_import_stream.execution_record_missing",
                               "durable execution identity is unavailable");
    }
    return false;
  }
  CanonicalCsvDecoder decoder(target.columns, entry.allocation,
                              entry.recovery_key_sha256);
  BulkImportSealedSpoolSnapshot snapshot;
  const auto read = request.registry->ReadSealedSpool(
      entry.allocation.stream_uuid, entry.allocation.stream_generation,
      entry.allocation.authenticated_receipt_uuid,
      entry.allocation.descriptor_evidence,
      [&](const std::uint8_t* bytes, std::size_t size, std::uint64_t offset) {
        return decoder.Consume(bytes, size, offset);
      },
      &snapshot);
  if (!read.ok || snapshot.total_stream_bytes != entry.seal.total_stream_bytes ||
      snapshot.final_chunk_count != entry.seal.final_chunk_count ||
      snapshot.content_sha != entry.seal.content_sha ||
      !decoder.Finish(output)) {
    *diagnostic = Diagnostic(
        "BULK.IMPORT.ABORTED",
        "sblr.bulk_import_stream.canonical_csv_invalid",
        !decoder.error().empty() ? decoder.error() : read.error);
    return false;
  }
  return true;
}

BulkSha EmptySetHash(std::string_view domain) {
  std::array<std::uint8_t, 4> zero{};
  return Hash(domain, zero);
}

BulkSha ImportedRowEventEvidence(
    const MgaBulkImportImportedRowEventV1& event) {
  std::vector<std::uint8_t> material;
  material.insert(material.end(), {1, 0, 0, 0});
  BulkUuid value{};
  auto append = [&](std::string_view text) {
    if (!ParseUuidText(text, &value)) return false;
    AppendUuid(&material, value);
    return true;
  };
  if (!append(event.durable_publication_uuid)) return {};
  AppendU64(&material, event.durable_publication_generation);
  material.insert(material.end(), event.recovery_idempotency_key.begin(),
                  event.recovery_idempotency_key.end());
  if (!append(event.mutation_uuid) || !append(event.bulk_batch_uuid) ||
      !append(event.owning_transaction_uuid)) {
    return {};
  }
  AppendU64(&material, event.owning_local_transaction_id);
  if (!append(event.statement_uuid)) return {};
  AppendU64(&material, event.savepoint_ordinal);
  if (!append(event.target_relation_uuid)) return {};
  AppendU64(&material, event.target_relation_generation);
  AppendU64(&material, event.import_ordinal);
  if (!append(event.row_uuid) || !append(event.row_version_uuid) ||
      !append(event.row_image_uuid)) {
    return {};
  }
  AppendU64(&material, event.row_image_metadata_generation);
  material.insert(material.end(), event.row_image_domain_hash.begin(),
                  event.row_image_domain_hash.end());
  material.insert(material.end(), event.row_image_value_hash.begin(),
                  event.row_image_value_hash.end());
  material.insert(material.end(), event.column_descriptor_set_sha256.begin(),
                  event.column_descriptor_set_sha256.end());
  material.insert(
      material.end(), event.canonical_typed_field_vector_sha256.begin(),
      event.canonical_typed_field_vector_sha256.end());
  return Hash("ScratchBird.BulkImportStreamImportedRowEvent.V1", material);
}

BulkSha ImportedRowsHash(
    const BulkUuid& publication_uuid,
    const std::vector<MgaBulkImportImportedRowEventV1>& events) {
  std::vector<std::uint8_t> material;
  AppendUuid(&material, publication_uuid);
  AppendU64(&material, static_cast<std::uint64_t>(events.size()));
  for (const auto& event : events) {
    AppendU64(&material, event.import_ordinal);
    material.insert(material.end(), event.event_evidence_sha256.begin(),
                    event.event_evidence_sha256.end());
  }
  return Hash("ScratchBird.BulkImportStreamImportedRows.V1", material);
}

BulkSha StatementEffectsHash(
    const BulkSha& imported_rows, const BulkImportStreamAllocation& allocation,
    const BulkSha& default_set, const BulkSha& constraint_set,
    const BulkSha& trigger_set, const BulkSha& index_set) {
  std::vector<std::uint8_t> material;
  material.insert(material.end(), imported_rows.begin(), imported_rows.end());
  material.insert(material.end(),
                  allocation.column_descriptor_set_sha256.begin(),
                  allocation.column_descriptor_set_sha256.end());
  material.insert(material.end(), allocation.import_policy_bundle_sha256.begin(),
                  allocation.import_policy_bundle_sha256.end());
  material.insert(material.end(), default_set.begin(), default_set.end());
  material.insert(material.end(), constraint_set.begin(), constraint_set.end());
  material.insert(material.end(), trigger_set.begin(), trigger_set.end());
  material.insert(material.end(), index_set.begin(), index_set.end());
  return Hash("ScratchBird.BulkImportStreamStatementEffects.V1", material);
}

BulkSha PostconditionEvidence(
    const MgaBulkImportPublicationRecordV1& record) {
  std::vector<std::uint8_t> material;
  material.insert(material.end(), record.recovery_idempotency_key.begin(),
                  record.recovery_idempotency_key.end());
  if (!AppendUuidText(&material, record.durable_publication_uuid)) return {};
  AppendU64(&material, record.durable_publication_generation);
  if (!AppendUuidText(&material, record.mutation_uuid) ||
      !AppendUuidText(&material, record.bulk_batch_uuid) ||
      !AppendUuidText(&material, record.target_relation_uuid)) {
    return {};
  }
  AppendU64(&material, record.target_relation_generation);
  material.insert(material.end(), record.content_sha256.begin(),
                  record.content_sha256.end());
  material.insert(material.end(),
                  record.imported_row_postcondition_sha256.begin(),
                  record.imported_row_postcondition_sha256.end());
  material.insert(material.end(),
                  record.normalized_statement_effect_sha256.begin(),
                  record.normalized_statement_effect_sha256.end());
  material.insert(material.end(), record.record_evidence_sha256.begin(),
                  record.record_evidence_sha256.end());
  AppendU64(&material, record.affected_rows);
  AppendU64(&material, record.rejected_rows);
  return Hash("ScratchBird.BulkImportStreamPostcondition.V1", material);
}

BulkSha ExecutorEvidence(const BulkImportStreamAllocation& allocation,
                         const BulkUuid& publication_uuid,
                         const BulkSha& postcondition) {
  std::vector<std::uint8_t> material;
  AppendUuid(&material, allocation.stream_uuid);
  AppendU64(&material, allocation.stream_generation);
  material.insert(material.end(), allocation.descriptor_evidence.begin(),
                  allocation.descriptor_evidence.end());
  AppendUuid(&material, publication_uuid);
  AppendU64(&material, 1);
  material.insert(material.end(), postcondition.begin(), postcondition.end());
  AppendU64(&material, allocation.executor_availability_generation);
  return Hash("ScratchBird.SblrBulkImportStreamExecutorEvidence.V1", material);
}

bool ExactRowValues(const CrudRowVersionRecord& actual,
                    const EngineRowValue& expected) {
  if (actual.deleted) return false;
  std::map<std::string, std::string> actual_values;
  for (const auto& [name, value] : actual.values) {
    if (!actual_values.emplace(name, value).second) return false;
  }
  if (actual_values.size() != expected.fields.size()) return false;
  for (const auto& [name, typed] : expected.fields) {
    const auto found = actual_values.find(name);
    const std::string expected_value =
        typed.isSqlNull() ? std::string("<NULL>") : typed.encoded_value;
    if (found == actual_values.end() || found->second != expected_value) {
      return false;
    }
  }
  return true;
}

enum class HistoricalRowsState : std::uint8_t {
  all_absent,
  all_exact,
  conflict,
};

HistoricalRowsState ClassifyHistoricalRows(
    const EngineRequestContext& context, const TargetAuthority& target,
    const ParsedExecution& parsed,
    const std::vector<MgaBulkImportImportedRowEventV1>& events,
    EngineApiDiagnostic* diagnostic) {
  if (!events.empty() && events.size() != parsed.rows.size()) {
    return HistoricalRowsState::conflict;
  }
  std::size_t absent = 0;
  std::size_t exact = 0;
  for (std::size_t index = 0; index < parsed.rows.size(); ++index) {
    const auto lineage = ProbeMgaBulkImportRowIdentityLineageV1(
        context, target.descriptor.relation_uuid.canonical,
        parsed.rows[index].requested_row_uuid.canonical);
    if (!lineage.ok) {
      if (diagnostic != nullptr) *diagnostic = lineage.diagnostic;
      return HistoricalRowsState::conflict;
    }
    if (lineage.versions.empty()) {
      ++absent;
      continue;
    }
    if (events.empty()) return HistoricalRowsState::conflict;
    const auto& event = events[index];
    const auto found = std::find_if(
        lineage.versions.begin(), lineage.versions.end(), [&](const auto& row) {
          return row.row_uuid == event.row_uuid &&
                 row.version_uuid == event.row_version_uuid;
        });
    if (found == lineage.versions.end() ||
        found->creator_tx != context.local_transaction_id ||
        !ExactRowValues(*found, parsed.rows[index]) ||
        event.import_ordinal != index + 1 ||
        event.row_uuid != parsed.rows[index].requested_row_uuid.canonical ||
        event.row_image_metadata_generation !=
            target.descriptor.descriptor_generation ||
        event.row_image_domain_hash !=
            event.column_descriptor_set_sha256 ||
        event.row_image_value_hash != parsed.row_value_hashes[index] ||
        event.canonical_typed_field_vector_sha256 !=
            parsed.row_value_hashes[index] ||
        ImportedRowEventEvidence(event) != event.event_evidence_sha256) {
      return HistoricalRowsState::conflict;
    }
    ++exact;
  }
  if (absent == parsed.rows.size()) return HistoricalRowsState::all_absent;
  if (exact == parsed.rows.size()) return HistoricalRowsState::all_exact;
  return HistoricalRowsState::conflict;
}

std::vector<std::uint8_t> BuildBirs(
    const BulkImportStreamEntry& entry,
    const MgaBulkImportPublicationRecordV1& publication) {
  BulkUuid publication_uuid{}, transaction_uuid{};
  if (!ParseUuidText(publication.durable_publication_uuid, &publication_uuid) ||
      !ParseUuidText(publication.owning_transaction_uuid, &transaction_uuid)) {
    return {};
  }
  engine::sblr::SblrBulkImportStreamResultV1 result;
  std::copy(entry.allocation.stream_uuid.begin(),
            entry.allocation.stream_uuid.end(), result.canonical_body.begin());
  StoreU64(&result.canonical_body, 16, entry.allocation.stream_generation);
  std::copy(publication_uuid.begin(), publication_uuid.end(),
            result.canonical_body.begin() + 24);
  StoreU64(&result.canonical_body, 40,
           publication.durable_publication_generation);
  StoreU64(&result.canonical_body, 48, publication.affected_rows);
  StoreU64(&result.canonical_body, 56, publication.rejected_rows);
  StoreU64(&result.canonical_body, 64, entry.seal.total_stream_bytes);
  StoreU64(&result.canonical_body, 72, entry.seal.final_chunk_count);
  std::copy(transaction_uuid.begin(), transaction_uuid.end(),
            result.canonical_body.begin() + 80);
  StoreU64(&result.canonical_body, 96,
           publication.owning_local_transaction_id);
  std::copy(entry.seal.content_sha.begin(), entry.seal.content_sha.end(),
            result.canonical_body.begin() + 104);
  result.availability_generation =
      entry.allocation.executor_availability_generation;
  return engine::sblr::EncodeSblrBulkImportStreamResultV1(result);
}

EngineExecuteBulkImportStreamResultV1 CompleteRegistryPublication(
    const EngineExecuteBulkImportStreamRequestV1& request,
    const BulkImportStreamEntry& entry,
    const MgaBulkImportPublicationRecordV1& publication, bool replayed) {
  BulkUuid publication_uuid{};
  if (!ParseUuidText(publication.durable_publication_uuid, &publication_uuid)) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.publication_identity_invalid",
                   "the MGA publication identity is not canonical");
  }
  const auto postcondition = PostconditionEvidence(publication);
  BulkImportPublicationRecord registry_publication;
  registry_publication.stream_uuid = entry.allocation.stream_uuid;
  registry_publication.stream_generation = entry.allocation.stream_generation;
  registry_publication.recovery_key_sha256 = entry.recovery_key_sha256;
  registry_publication.durable_publication_uuid = publication_uuid;
  registry_publication.durable_publication_generation =
      publication.durable_publication_generation;
  registry_publication.affected_rows = publication.affected_rows;
  registry_publication.rejected_rows = publication.rejected_rows;
  registry_publication.postcondition_evidence_sha256 = postcondition;
  const auto published = request.registry->Publish(registry_publication);
  if (!published.ok) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.registry_publication_failed",
                   published.error);
  }
  BulkImportExecutorEvidenceRecord evidence;
  evidence.stream_uuid = entry.allocation.stream_uuid;
  evidence.stream_generation = entry.allocation.stream_generation;
  evidence.durable_publication_uuid = publication_uuid;
  evidence.durable_publication_generation =
      publication.durable_publication_generation;
  evidence.executor_availability_generation =
      entry.allocation.executor_availability_generation;
  evidence.executor_evidence_sha256 =
      ExecutorEvidence(entry.allocation, publication_uuid, postcondition);
  const auto evidenced = request.registry->RecordEvidence(evidence);
  if (!evidenced.ok) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.executor_evidence_failed",
                   evidenced.error);
  }
  const auto birs = BuildBirs(entry, publication);
  if (birs.empty()) {
    return Failure("BULK.IMPORT.ABORTED",
                   "sblr.bulk_import_stream.result_encoding_failed",
                   "canonical BIRS encoding failed");
  }
  const auto recorded = request.registry->RecordResult(
      entry.allocation.stream_uuid, entry.allocation.stream_generation, birs);
  if (!recorded.ok || recorded.response_wire != birs) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.result_record_failed",
                   recorded.error);
  }
  EngineExecuteBulkImportStreamResultV1 result;
  result.ok = true;
  result.replayed = replayed || recorded.replayed;
  result.affected_rows = publication.affected_rows;
  result.rejected_rows = publication.rejected_rows;
  result.canonical_birs = recorded.response_wire;
  return result;
}

MgaBulkImportPublicationRecordV1 BuildPublicationRecord(
    const EngineRequestContext& context, const BulkImportStreamEntry& entry,
    const BulkUuid& publication_uuid, const BulkUuid& mutation_uuid,
    const BulkUuid& batch_uuid,
    const std::vector<MgaBulkImportImportedRowEventV1>& events,
    const BulkSha& default_set, const BulkSha& constraint_set,
    const BulkSha& trigger_set, const BulkSha& index_set) {
  MgaBulkImportPublicationRecordV1 record;
  record.durable_publication_uuid = UuidText(publication_uuid);
  record.durable_publication_generation = 1;
  record.recovery_idempotency_key = entry.recovery_key_sha256;
  record.stream_uuid = UuidText(entry.allocation.stream_uuid);
  record.stream_generation = entry.allocation.stream_generation;
  record.descriptor_evidence = entry.allocation.descriptor_evidence;
  record.target_relation_uuid = UuidText(entry.allocation.target_relation_uuid);
  record.target_relation_generation =
      entry.allocation.target_relation_generation;
  record.owning_transaction_uuid =
      UuidText(entry.allocation.owning_transaction_uuid);
  record.owning_local_transaction_id =
      entry.allocation.owning_local_transaction_id;
  record.authenticated_receipt_uuid =
      UuidText(entry.allocation.authenticated_receipt_uuid);
  record.statement_uuid = context.statement_uuid.canonical;
  record.savepoint_ordinal = 1;
  record.mutation_uuid = UuidText(mutation_uuid);
  record.bulk_batch_uuid = UuidText(batch_uuid);
  record.content_sha256 = entry.seal.content_sha;
  record.total_stream_bytes = entry.seal.total_stream_bytes;
  record.chunk_count = entry.seal.final_chunk_count;
  record.input_row_count = events.size();
  record.affected_rows = events.size();
  record.rejected_rows = 0;
  record.imported_row_postcondition_count = events.size();
  record.imported_row_postcondition_sha256 =
      ImportedRowsHash(publication_uuid, events);
  record.normalized_statement_effect_sha256 = StatementEffectsHash(
      record.imported_row_postcondition_sha256, entry.allocation, default_set,
      constraint_set, trigger_set, index_set);
  record.column_descriptor_set_sha256 =
      entry.allocation.column_descriptor_set_sha256;
  record.import_policy_bundle_sha256 =
      entry.allocation.import_policy_bundle_sha256;
  record.default_descriptor_set_sha256 = default_set;
  record.constraint_set_sha256 = constraint_set;
  record.trigger_set_sha256 = trigger_set;
  record.index_set_sha256 = index_set;
  record.executor_availability_generation =
      entry.allocation.executor_availability_generation;
  return record;
}

bool PublicationAuthorityEqual(MgaBulkImportPublicationRecordV1 left,
                               MgaBulkImportPublicationRecordV1 right) {
  left.lifecycle = MgaBulkImportPublicationLifecycleV1::prepared;
  right.lifecycle = MgaBulkImportPublicationLifecycleV1::prepared;
  left.record_evidence_sha256 = {};
  right.record_evidence_sha256 = {};
  return left == right;
}

std::vector<MgaBulkImportImportedRowEventV1> BuildImportedRowEvents(
    const EngineRequestContext& context, const BulkImportStreamEntry& entry,
    const TargetAuthority& target, const ParsedExecution& parsed,
    const BulkUuid& publication_uuid, const BulkUuid& mutation_uuid,
    const BulkUuid& batch_uuid, std::span<const std::string> row_uuids,
    std::span<const std::string> version_uuids,
    std::span<const std::string> image_uuids) {
  if (row_uuids.size() != parsed.rows.size() ||
      version_uuids.size() != parsed.rows.size() ||
      image_uuids.size() != parsed.rows.size()) {
    return {};
  }
  std::vector<MgaBulkImportImportedRowEventV1> events;
  events.reserve(parsed.rows.size());
  for (std::size_t index = 0; index < parsed.rows.size(); ++index) {
    if (row_uuids[index] != parsed.rows[index].requested_row_uuid.canonical) {
      return {};
    }
    MgaBulkImportImportedRowEventV1 event;
    event.durable_publication_uuid = UuidText(publication_uuid);
    event.durable_publication_generation = 1;
    event.recovery_idempotency_key = entry.recovery_key_sha256;
    event.mutation_uuid = UuidText(mutation_uuid);
    event.bulk_batch_uuid = UuidText(batch_uuid);
    event.owning_transaction_uuid =
        UuidText(entry.allocation.owning_transaction_uuid);
    event.owning_local_transaction_id =
        entry.allocation.owning_local_transaction_id;
    event.statement_uuid = context.statement_uuid.canonical;
    event.savepoint_ordinal = 1;
    event.target_relation_uuid = target.descriptor.relation_uuid.canonical;
    event.target_relation_generation = target.descriptor.relation_generation;
    event.import_ordinal = index + 1;
    event.row_uuid = row_uuids[index];
    event.row_version_uuid = version_uuids[index];
    event.row_image_uuid = image_uuids[index];
    event.row_image_metadata_generation =
        target.descriptor.descriptor_generation;
    event.row_image_domain_hash =
        entry.allocation.column_descriptor_set_sha256;
    event.row_image_value_hash = parsed.row_value_hashes[index];
    event.column_descriptor_set_sha256 =
        entry.allocation.column_descriptor_set_sha256;
    event.canonical_typed_field_vector_sha256 =
        parsed.row_value_hashes[index];
    event.event_evidence_sha256 = ImportedRowEventEvidence(event);
    if (!Nonzero(event.event_evidence_sha256)) return {};
    events.push_back(std::move(event));
  }
  return events;
}

EngineApiDiagnostic FirstNativeDiagnostic(
    const EngineExecuteNativeBulkIngestResult& result) {
  const auto substantive = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto& diagnostic) {
        return !diagnostic.code.empty() || !diagnostic.message_key.empty() ||
               !diagnostic.detail.empty();
      });
  if (substantive != result.diagnostics.end()) return *substantive;
  std::string evidence_detail =
      "the engine native mutation producer refused without a diagnostic";
  if (!result.evidence.empty()) {
    evidence_detail += ";evidence=";
    const std::size_t limit = std::min<std::size_t>(result.evidence.size(), 8);
    for (std::size_t index = 0; index < limit; ++index) {
      if (index != 0) evidence_detail.push_back(',');
      evidence_detail += result.evidence[index].evidence_kind;
      evidence_detail.push_back('=');
      evidence_detail += result.evidence[index].evidence_id;
    }
  }
  return Diagnostic("BULK.IMPORT.ABORTED",
                    "sblr.bulk_import_stream.native_mutation_failed",
                    std::move(evidence_detail));
}

bool CancellationRequested(const EngineRequestContext& context) {
  if (!context.query_cancellation_requested) return false;
  try {
    return context.query_cancellation_requested();
  } catch (...) {
    return true;
  }
}

}  // namespace

EngineExecuteBulkImportStreamResultV1 ExecuteBulkImportStreamV1(
    const EngineExecuteBulkImportStreamRequestV1& request) {
  DecodedAuthority authority;
  EngineApiDiagnostic diagnostic;
  if (!DecodeAuthority(request, &authority, &diagnostic)) {
    return Failure(diagnostic.code, diagnostic.message_key, diagnostic.detail);
  }
  if (!ExactContext(request.context, authority.entry.allocation, &diagnostic)) {
    return Failure(diagnostic.code, diagnostic.message_key, diagnostic.detail);
  }
  if (authority.entry.state == BulkImportStreamState::result_recorded) {
    engine::sblr::SblrBulkImportStreamResultV1 decoded;
    std::string detail;
    if (authority.entry.result_wire.empty() ||
        !engine::sblr::DecodeSblrBulkImportStreamResultV1(
            authority.entry.result_wire.data(), authority.entry.result_wire.size(),
            &decoded, &detail)) {
      return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                     "sblr.bulk_import_stream.recorded_result_corrupt", detail);
    }
    EngineExecuteBulkImportStreamResultV1 result;
    result.ok = true;
    result.replayed = true;
    result.affected_rows = authority.entry.publication.affected_rows;
    result.rejected_rows = authority.entry.publication.rejected_rows;
    result.canonical_birs = authority.entry.result_wire;
    return result;
  }
  if (authority.entry.state == BulkImportStreamState::aborted ||
      authority.entry.state == BulkImportStreamState::refused) {
    return Failure("BULK.IMPORT.ABORTED",
                   "sblr.bulk_import_stream.terminal_before_execution",
                   "the durable stream is already terminal");
  }
  if (authority.entry.state == BulkImportStreamState::allocated ||
      authority.entry.state == BulkImportStreamState::receiving ||
      !authority.entry.seal.present) {
    return Failure("BULK.IMPORT.STREAM_NOT_SEALED",
                   "sblr.bulk_import_stream.stream_not_sealed",
                   "terminal opcode 775 requires one exact durable seal");
  }
  TargetAuthority target;
  if (!LoadTargetAuthority(request.context, authority.entry.allocation, &target,
                           &diagnostic)) {
    return Failure(diagnostic.code, diagnostic.message_key, diagnostic.detail);
  }
  const auto live_policy =
      PolicyBundleDigest(authority.entry.allocation, target);
  if (!Nonzero(live_policy) ||
      live_policy != authority.entry.allocation.import_policy_bundle_sha256) {
    return Failure("BULK.IMPORT.GENERATION_CONFLICT",
                   "sblr.bulk_import_stream.policy_authority_changed",
                   "the exact converter and empty policy sets changed after bind");
  }
  if (authority.entry.allocation.cluster_bound) {
    return Failure(
        "CLUSTER.WRITE_AUTHORITY_REQUIRED",
        "sblr.bulk_import_stream.cluster_executor_not_admitted",
        "v1 terminal execution has no exact cluster publication producer");
  }
  if (authority.entry.seal.total_stream_bytes == 0 ||
      authority.entry.seal.total_stream_bytes >
          authority.entry.allocation.effective_maximum_stream_bytes ||
      authority.entry.seal.final_chunk_count == 0 ||
      authority.entry.seal.final_chunk_count >
          authority.entry.allocation.effective_maximum_chunk_count) {
    return Failure(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.bulk_import_stream.sealed_resource_limit_exceeded",
        "the sealed stream exceeds its exact engine-frozen resource grant");
  }
  if (!ExactExecutorAvailability(request.context,
                                 authority.entry.allocation, &diagnostic)) {
    return Failure(diagnostic.code, diagnostic.message_key,
                   diagnostic.detail);
  }
  if (CancellationRequested(request.context)) {
    return Failure("PROCESS.CANCELLED",
                   "sblr.bulk_import_stream.cancelled_before_registry_lock",
                   "cancellation was observed before execution admission");
  }
  const auto begun = request.registry->BeginExecution(
      authority.entry.allocation.stream_uuid,
      authority.entry.allocation.stream_generation);
  if (!begun.ok) {
    const std::string code =
        begun.error == "stream_not_sealed"
            ? "BULK.IMPORT.STREAM_NOT_SEALED"
            : "BULK.IMPORT.RECOVERY_CONFLICT";
    return Failure(code, "sblr.bulk_import_stream.execution_begin_refused",
                   begun.error);
  }
  if (!request.registry
           ->Recover(authority.entry.allocation.stream_uuid, &authority.entry)
           .ok ||
      !Nonzero(authority.entry.recovery_key_sha256)) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.execution_record_missing",
                   "the durable execution transition could not be recovered");
  }
  ParsedExecution parsed;
  if (!ParseSealedStream(request, authority.entry, target, &parsed,
                         &diagnostic)) {
    (void)request.registry->AbortBeforePublication(
        authority.entry.allocation.stream_uuid,
        authority.entry.allocation.stream_generation,
        BulkImportStreamAbortReason::execution_aborted,
        diagnostic.message_key);
    return Failure(diagnostic.code, diagnostic.message_key, diagnostic.detail);
  }

  const std::span<const std::uint8_t> recovery_material(
      authority.entry.recovery_key_sha256.data(),
      authority.entry.recovery_key_sha256.size());
  const BulkUuid publication_uuid = DerivedUuid(
      authority.entry.allocation.stream_uuid,
      "ScratchBird.BulkImportStreamPublicationIdentity.V1", recovery_material);
  const BulkUuid mutation_uuid = DerivedUuid(
      authority.entry.allocation.stream_uuid,
      "ScratchBird.BulkImportStreamMutationIdentity.V1", recovery_material);
  const BulkUuid batch_uuid = DerivedUuid(
      authority.entry.allocation.stream_uuid,
      "ScratchBird.BulkImportStreamBatchIdentity.V1", recovery_material);
  std::set<std::string> operation_identities{
      UuidText(authority.entry.allocation.stream_uuid),
      UuidText(authority.entry.allocation.recovery_operation_uuid),
      UuidText(publication_uuid), UuidText(mutation_uuid), UuidText(batch_uuid)};
  for (const auto& row : parsed.rows) {
    if (!operation_identities.insert(row.requested_row_uuid.canonical).second) {
      return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                     "sblr.bulk_import_stream.derived_identity_collision",
                     "one deterministic operation identity collided");
    }
  }

  const BulkSha default_set = EmptySetHash(
      "ScratchBird.BulkImportStreamDefaultDescriptorSet.V1");
  const BulkSha constraint_set =
      EmptySetHash("ScratchBird.BulkImportStreamConstraintSet.V1");
  const BulkSha trigger_set =
      EmptySetHash("ScratchBird.BulkImportStreamTriggerSet.V1");
  const BulkSha index_set =
      EmptySetHash("ScratchBird.BulkImportStreamIndexSet.V1");

  auto publication = RecoverMgaBulkImportPublicationV1(
      request.context, authority.entry.recovery_key_sha256);
  if (!publication.ok) {
    return Failure(publication.diagnostic.code,
                   publication.diagnostic.message_key,
                   publication.diagnostic.detail);
  }
  auto imported_events = RecoverMgaBulkImportImportedRowEventsV1(
      request.context, authority.entry.recovery_key_sha256);
  if (!imported_events.ok) {
    return Failure(imported_events.diagnostic.code,
                   imported_events.diagnostic.message_key,
                   imported_events.diagnostic.detail);
  }
  const auto historical = ClassifyHistoricalRows(
      request.context, target, parsed, imported_events.events, &diagnostic);
  if (publication.found) {
    if (historical != HistoricalRowsState::all_exact ||
        imported_events.events.size() != parsed.rows.size() ||
        publication.record.lifecycle ==
            MgaBulkImportPublicationLifecycleV1::aborted) {
      return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                     "sblr.bulk_import_stream.publication_postcondition_conflict",
                     "the durable MGA publication has no exact historical row event set");
    }
    const auto expected = BuildPublicationRecord(
        request.context, authority.entry, publication_uuid, mutation_uuid,
        batch_uuid, imported_events.events, default_set, constraint_set,
        trigger_set, index_set);
    if (!PublicationAuthorityEqual(publication.record, expected)) {
      return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                     "sblr.bulk_import_stream.publication_authority_conflict",
                     "the durable MGA publication differs from the sealed stream");
    }
    if (publication.record.lifecycle ==
        MgaBulkImportPublicationLifecycleV1::prepared) {
      publication = PublishMgaBulkImportPublicationV1(request.context,
                                                       publication.record);
      if (!publication.ok) {
        return Failure(publication.diagnostic.code,
                       publication.diagnostic.message_key,
                       publication.diagnostic.detail);
      }
    }
    return CompleteRegistryPublication(request, authority.entry,
                                       publication.record, true);
  }
  if (!imported_events.events.empty() ||
      historical != HistoricalRowsState::all_absent) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.partial_publication_conflict",
                   "rows or events exist without the exact MGA publication");
  }
  if (CancellationRequested(request.context)) {
    return Failure("PROCESS.CANCELLED",
                   "sblr.bulk_import_stream.cancelled_before_mutation",
                   "cancellation was observed before target mutation");
  }

  struct PublicationState {
    bool prepared = false;
    bool published = false;
    MgaBulkImportPublicationRecordV1 record;
    std::vector<MgaBulkImportImportedRowEventV1> events;
  } state;
  EngineExecuteNativeBulkIngestRequest native;
  native.context = request.context;
  native.target_table.uuid.canonical = target.descriptor.relation_uuid.canonical;
  native.target_table.object_kind = "table";
  native.canonical_rows = parsed.rows;
  native.shared_row_field_order = parsed.shared_field_order;
  native.estimated_row_count = parsed.rows.size();
  native.duplicate_mode = "error";
  native.require_generated_row_uuid = false;
  native.native_bulk_ingest_enabled = true;
  native.import_policy.strict_bulk_load_requested = true;
  native.option_envelopes.push_back("feature.strict_bulk_load=enabled");
  native.checkpoint_policy.checkpoint_mode = "disabled";
  native.before_row_publication =
      [&](std::span<const std::string> row_uuids,
          std::span<const std::string> version_uuids,
          std::span<const std::string> image_uuids) {
        state.events = BuildImportedRowEvents(
            request.context, authority.entry, target, parsed,
            publication_uuid, mutation_uuid, batch_uuid, row_uuids,
            version_uuids, image_uuids);
        if (state.events.size() != parsed.rows.size()) {
          return Diagnostic(
              "BULK.IMPORT.RECOVERY_CONFLICT",
              "sblr.bulk_import_stream.row_identity_projection_failed",
              "the MGA row/version/image identity set did not match the stream");
        }
        state.record = BuildPublicationRecord(
            request.context, authority.entry, publication_uuid, mutation_uuid,
            batch_uuid, state.events, default_set, constraint_set, trigger_set,
            index_set);
        auto prepared = PrepareMgaBulkImportPublicationV1(request.context,
                                                          state.record);
        if (!prepared.ok) return prepared.diagnostic;
        state.record = prepared.record;
        auto stored = StoreMgaBulkImportImportedRowEventsV1(request.context,
                                                            state.events);
        if (!stored.ok) return stored.diagnostic;
        state.events = std::move(stored.events);
        state.prepared = true;
        return SuccessDiagnostic();
      };
  native.before_statement_publication =
      [&](EngineApiU64 accepted, EngineApiU64 inserted,
          EngineApiU64 rejected) {
        if (!state.prepared || accepted != parsed.rows.size() ||
            inserted != parsed.rows.size() || rejected != 0) {
          return Diagnostic(
              "BULK.IMPORT.RECOVERY_CONFLICT",
              "sblr.bulk_import_stream.statement_publication_count_mismatch",
              "the native producer result differs from the prepared publication");
        }
        return SuccessDiagnostic();
      };
  native.after_statement_publication =
      [&](EngineApiU64 accepted, EngineApiU64 inserted,
          EngineApiU64 rejected) {
        if (accepted != parsed.rows.size() || inserted != parsed.rows.size() ||
            rejected != 0) {
          return Diagnostic(
              "BULK.IMPORT.RECOVERY_CONFLICT",
              "sblr.bulk_import_stream.statement_publication_count_mismatch",
              "the published native result differs from the prepared record");
        }
        auto published =
            PublishMgaBulkImportPublicationV1(request.context, state.record);
        if (!published.ok) return published.diagnostic;
        state.record = published.record;
        state.published = true;
        return SuccessDiagnostic();
      };
  native.after_statement_rollback = [&]() {
    if (state.prepared && !state.published) {
      (void)AbortMgaBulkImportPublicationV1(request.context, state.record);
    }
  };

  const auto mutated = EngineExecuteNativeBulkIngest(native);
  if (!mutated.ok) {
    if (state.published) {
      const auto exact = ClassifyHistoricalRows(
          request.context, target, parsed, state.events, &diagnostic);
      if (exact == HistoricalRowsState::all_exact) {
        return CompleteRegistryPublication(request, authority.entry,
                                           state.record, true);
      }
    }
    (void)request.registry->AbortBeforePublication(
        authority.entry.allocation.stream_uuid,
        authority.entry.allocation.stream_generation,
        BulkImportStreamAbortReason::execution_aborted,
        "native_mutation_refused");
    const auto native_diagnostic = FirstNativeDiagnostic(mutated);
    return Failure(native_diagnostic.code, native_diagnostic.message_key,
                   native_diagnostic.detail);
  }
  if (!state.published || mutated.inserted_rows != parsed.rows.size() ||
      mutated.rejected_rows != 0) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.publication_observation_missing",
                   "row mutation returned without its exact durable publication");
  }
  const auto final_history = ClassifyHistoricalRows(
      request.context, target, parsed, state.events, &diagnostic);
  if (final_history != HistoricalRowsState::all_exact) {
    return Failure("BULK.IMPORT.RECOVERY_CONFLICT",
                   "sblr.bulk_import_stream.final_postcondition_invalid",
                   diagnostic.detail.empty()
                       ? "the published historical row set is incomplete"
                       : diagnostic.detail);
  }
  return CompleteRegistryPublication(request, authority.entry, state.record,
                                     false);
}

}  // namespace scratchbird::engine::internal_api
