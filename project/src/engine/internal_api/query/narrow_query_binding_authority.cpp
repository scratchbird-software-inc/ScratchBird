// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/narrow_query_binding_authority.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace wire = scratchbird::wire;
namespace datatypes = scratchbird::core::datatypes;

constexpr std::string_view kBinderTag =
    "private_narrow_query_binding_binder";
constexpr std::string_view kConsumerTag =
    "private_narrow_query_binding_consumer";
constexpr std::uint64_t kMaximumProfileRows = 1048576;

bool ValidMgaRelationDecodedBytesPerPass(std::uint64_t value) {
  return value >= wire::kNarrowQueryMinimumMgaRelationDecodedBytesPerPass &&
         value <= wire::kNarrowQueryMaximumMgaRelationDecodedBytesPerPass;
}

bool ValidTypedResultTransportBytesPerPacket(std::uint64_t value) {
  return value >= kMinimumTypedResultTransportBytesPerPacket &&
         value <= kMaximumTypedResultTransportBytesPerPacket;
}

std::atomic<std::uint64_t> g_identity_ordinal{1};
std::mutex g_authority_mutex;
std::unordered_map<std::string,
                   std::shared_ptr<
                       EngineNarrowQueryBindingAuthorityHandleV1::Authority>>
    g_authorities_by_receipt;

EngineApiDiagnostic Diagnostic(std::string code,
                               std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool HasTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool ExactUuid(std::string_view text) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

struct ExactEncodedDescriptorFieldLookup {
  bool well_formed = false;
  bool present = false;
  std::string value;
};

ExactEncodedDescriptorFieldLookup LookupExactEncodedDescriptorField(
    const std::string_view descriptor,
    const std::string_view requested_key) {
  ExactEncodedDescriptorFieldLookup result;
  if (descriptor.empty() || requested_key.empty()) return result;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    const auto equals = field.find('=');
    if (field.empty() || equals == std::string_view::npos || equals == 0 ||
        equals + 1 == field.size()) {
      return {};
    }
    if (field.substr(0, equals) == requested_key) {
      if (result.present) return {};
      result.present = true;
      result.value = std::string(field.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  result.well_formed = true;
  return result;
}

bool ToWireUuid(std::string_view text, wire::NarrowQueryUuid* output) {
  if (output == nullptr || !ExactUuid(text)) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            output->begin());
  return true;
}

std::string UuidText(const wire::NarrowQueryUuid& input) {
  scratchbird::core::platform::Uuid uuid{};
  std::copy(input.begin(), input.end(), uuid.bytes.begin());
  if (scratchbird::core::uuid::IsNilUuid(uuid)) return {};
  return scratchbird::core::uuid::UuidToString(uuid);
}

std::string FreshUuid(std::unordered_set<std::string>* issued) {
  if (issued == nullptr) return {};
  constexpr std::uint64_t kAttempts = 64;
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (std::uint64_t attempt = 0; attempt < kAttempts; ++attempt) {
    const auto ordinal =
        g_identity_ordinal.fetch_add(1, std::memory_order_relaxed);
    const auto generated =
        scratchbird::core::uuid::GenerateDurableEngineIdentityV7(
            scratchbird::core::platform::UuidKind::object,
            now + ordinal + attempt);
    if (!generated.ok()) return {};
    auto text = scratchbird::core::uuid::UuidToString(generated.value.value);
    if (issued->insert(text).second) return text;
  }
  return {};
}

void AppendU8(std::vector<std::uint8_t>* bytes, std::uint8_t value) {
  bytes->push_back(value);
}

void AppendU16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value & 0xffu));
  bytes->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void AppendU32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes->push_back(
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu));
  }
}

void AppendU64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    bytes->push_back(
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu));
  }
}

bool AppendUuid(std::vector<std::uint8_t>* bytes,
                std::string_view text,
                bool optional = false) {
  if (text.empty() && optional) {
    bytes->insert(bytes->end(), 16, 0);
    return true;
  }
  if (!ExactUuid(text)) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  bytes->insert(bytes->end(), parsed.value.bytes.begin(),
                parsed.value.bytes.end());
  return true;
}

bool AppendString(std::vector<std::uint8_t>* bytes, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) return false;
  AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
  return true;
}

bool HashBytes(const std::vector<std::uint8_t>& bytes,
               wire::NarrowQueryHash* hash) {
  if (hash == nullptr) return false;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
  if (!digest.ok()) return false;
  std::copy(digest.digest.begin(), digest.digest.end(), hash->begin());
  return std::any_of(hash->begin(), hash->end(),
                     [](std::uint8_t value) { return value != 0; });
}

EngineApiDiagnostic CodecDiagnostic(const wire::NarrowQueryBindingError& error,
                                    std::string_view key) {
  return Diagnostic(error.diagnostic_code.empty()
                        ? "SBLR.OPERAND.INVALID"
                        : error.diagnostic_code,
                    std::string(key),
                    error.field.empty() ? error.detail
                                        : error.field + ":" + error.detail);
}

EngineApiDiagnostic DemandDiagnostic(
    const wire::NarrowQueryBindingDemandError& error) {
  return Diagnostic(error.diagnostic_code.empty()
                        ? "SBLR.OPERAND.INVALID"
                        : error.diagnostic_code,
                    "sblr.query_execute.narrow_demand_invalid",
                    error.field.empty() ? error.detail
                                        : error.field + ":" + error.detail);
}

EngineApiDiagnostic ValidateContext(const EngineRequestContext& context,
                                    std::string_view required_tag) {
  const bool binder = HasTag(context, kBinderTag);
  const bool consumer = HasTag(context, kConsumerTag);
  if (!HasTag(context, required_tag) || binder == consumer ||
      !context.security_context_present ||
      !context.authorization_context.present ||
      !context.statement_metadata_snapshot_engine_owned ||
      context.cluster_transaction_active || context.route_fence_present ||
      context.local_transaction_id == 0 ||
      !ExactUuid(context.database_uuid.canonical) ||
      !ExactUuid(context.transaction_uuid.canonical) ||
      !ExactUuid(context.statement_receipt_uuid.canonical) ||
      !ExactUuid(context.statement_snapshot_uuid.canonical) ||
      !ExactUuid(context.datatype_catalog_snapshot_uuid.canonical) ||
      context.datatype_catalog_generation == 0 ||
      context.datatype_registry_generation == 0 ||
      !ValidMgaRelationDecodedBytesPerPass(
          context.maximum_mga_relation_decoded_bytes_per_pass) ||
      !ExactUuid(context.authorization_context.authority_uuid.canonical) ||
      context.authorization_context.security_context_generation == 0) {
    return Diagnostic("SECURITY.ACCESS_DENIED",
                      "sblr.query_execute.narrow_context_denied",
                      "exact private statement authority is required");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return Diagnostic("PROCESS.CANCELLED",
                      "sblr.query_execute.narrow_cancelled");
  }
  return OkDiagnostic();
}

bool SameContext(const EngineRequestContext& expected,
                 const EngineRequestContext& observed) {
  return expected.database_uuid.canonical == observed.database_uuid.canonical &&
         expected.statement_receipt_uuid.canonical ==
             observed.statement_receipt_uuid.canonical &&
         expected.transaction_uuid.canonical ==
             observed.transaction_uuid.canonical &&
         expected.local_transaction_id == observed.local_transaction_id &&
         expected.statement_snapshot_uuid.canonical ==
             observed.statement_snapshot_uuid.canonical &&
         expected.datatype_catalog_snapshot_uuid.canonical ==
             observed.datatype_catalog_snapshot_uuid.canonical &&
         expected.datatype_catalog_generation ==
             observed.datatype_catalog_generation &&
         expected.datatype_registry_generation ==
             observed.datatype_registry_generation &&
         expected.authorization_context.authority_uuid.canonical ==
             observed.authorization_context.authority_uuid.canonical &&
         expected.authorization_context.security_context_generation ==
             observed.authorization_context.security_context_generation &&
         expected.authorization_context.security_epoch ==
             observed.authorization_context.security_epoch &&
         expected.authorization_context.policy_epoch ==
             observed.authorization_context.policy_epoch &&
         expected.resource_epoch == observed.resource_epoch &&
         expected.maximum_mga_relation_decoded_bytes_per_pass ==
             observed.maximum_mga_relation_decoded_bytes_per_pass &&
         expected.maximum_typed_result_transport_bytes_per_packet ==
             observed.maximum_typed_result_transport_bytes_per_packet;
}

std::string ResultRowField(const EngineApiResult& result,
                           std::string_view field_name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == field_name && !value.isSqlNull()) return value.encoded_value;
    }
  }
  return {};
}

std::string RelationCanonicalName(const EngineRequestContext& context,
                                  std::string_view relation_uuid) {
  EngineMapUuidToNameRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::string(relation_uuid);
  request.target_object.object_kind = "relation";
  const auto result = EngineMapUuidToName(request);
  if (!result.ok ||
      result.primary_object.uuid.canonical != relation_uuid ||
      result.primary_object.object_kind != "relation") {
    return {};
  }
  return ResultRowField(result, "name");
}

struct ProjectedColumn {
  MgaRelationColumnStorageDescriptor descriptor;
  std::optional<datatypes::DatatypeTypeCodecIdentityRowV1> datatype;
  std::string charset_name;
  std::uint32_t charset_min_bytes = 0;
  std::uint32_t charset_max_bytes = 0;
  bool charset_variable_width = false;
  std::string collation_name;
  std::uint64_t collation_generation = 0;
};

struct ResolvedSource {
  MgaRelationStorageDescriptor descriptor;
  std::string alias;
  std::vector<ProjectedColumn> columns;
  wire::NarrowQueryHash projection_hash{};
};

bool BuildProjectedSource(const EngineRequestContext& context,
                          const MgaRelationStorageDescriptor& descriptor,
                          std::string alias,
                          ResolvedSource* output,
                          EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                               "sblr.query_execute.source_projection_invalid");
    }
    return false;
  }
  const auto& relation_uuid = descriptor.relation_uuid.canonical;
  if (!ExactUuid(relation_uuid) ||
      descriptor.database_uuid.canonical !=
          context.database_uuid.canonical ||
      descriptor.relation_kind != "table" ||
      descriptor.storage_profile != "local_mga_rowstore_v1" ||
      descriptor.descriptor_generation == 0 || descriptor.columns.empty()) {
    *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                             "sblr.query_execute.source_invalid");
    return false;
  }
  output->descriptor = descriptor;
  output->alias = std::move(alias);
  if (output->alias.empty() || output->alias.size() > 128) {
    *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                             "sblr.query_execute.source_alias_invalid");
    return false;
  }

  std::vector<std::uint8_t> projection;
  if (!AppendUuid(&projection,
                  descriptor.descriptor_uuid.canonical) ||
      !AppendUuid(&projection, relation_uuid) ||
      !AppendUuid(&projection, descriptor.schema_uuid.canonical)) {
    *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                             "sblr.query_execute.source_identity_invalid");
    return false;
  }
  AppendU64(&projection, descriptor.descriptor_generation);
  AppendU64(&projection, context.resource_epoch);
  if (!AppendUuid(&projection,
                  context.datatype_catalog_snapshot_uuid.canonical)) {
    *diagnostic = Diagnostic("DATATYPE.DESCRIPTOR_INVALID",
                             "sblr.query_execute.datatype_snapshot_invalid");
    return false;
  }
  AppendU64(&projection, context.datatype_catalog_generation);
  AppendU64(&projection, context.datatype_registry_generation);
  if (descriptor.columns.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    *diagnostic = Diagnostic("RESOURCE.BUDGET_EXCEEDED",
                             "sblr.query_execute.source_projection_too_large");
    return false;
  }
  AppendU32(&projection,
            static_cast<std::uint32_t>(descriptor.columns.size()));

  output->columns.reserve(descriptor.columns.size());
  for (const auto& column : descriptor.columns) {
    ProjectedColumn projected;
    projected.descriptor = column;
    const auto embedded_datatype_descriptor =
        LookupExactEncodedDescriptorField(
            column.value_descriptor.encoded_descriptor,
            "datatype_descriptor_uuid");
    const std::string& canonical_datatype_descriptor_uuid =
        embedded_datatype_descriptor.present
            ? embedded_datatype_descriptor.value
            : column.value_descriptor.descriptor_uuid.canonical;
    const auto datatype = datatypes::LookupDatatypeTypeCodecIdentityV1(
        context.datatype_catalog_snapshot_uuid.canonical,
        context.datatype_catalog_generation,
        context.datatype_registry_generation,
        canonical_datatype_descriptor_uuid, 1);
    if (embedded_datatype_descriptor.well_formed &&
        ExactUuid(canonical_datatype_descriptor_uuid) && datatype.ok) {
      projected.datatype = datatype.row;
    }

    if (!column.charset_uuid.empty()) {
      EngineUuid uuid{column.charset_uuid};
      const auto charset = LookupEngineResourceDescriptorByUuid(
          context, uuid, "charset");
      if (!charset.ok || !charset.resource_descriptor.present ||
          charset.resource_descriptor.resource_uuid.canonical !=
              column.charset_uuid ||
          charset.resource_descriptor.resource_epoch !=
              context.resource_epoch) {
        *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                                 "sblr.query_execute.charset_stale");
        return false;
      }
      projected.charset_name = charset.resource_descriptor.canonical_name;
      projected.charset_min_bytes = charset.resource_descriptor.min_bytes;
      projected.charset_max_bytes = charset.resource_descriptor.max_bytes;
      projected.charset_variable_width =
          charset.resource_descriptor.variable_width;
    }
    if (!column.collation_uuid.empty()) {
      EngineUuid uuid{column.collation_uuid};
      const auto collation = LookupEngineResourceDescriptorByUuid(
          context, uuid, "collation");
      if (!collation.ok || !collation.resource_descriptor.present ||
          collation.resource_descriptor.resource_uuid.canonical !=
              column.collation_uuid ||
          collation.resource_descriptor.parent_resource_uuid.canonical !=
              column.charset_uuid ||
          collation.resource_descriptor.resource_epoch !=
              context.resource_epoch ||
          collation.resource_descriptor.family_epoch == 0) {
        *diagnostic = Diagnostic("SORT.COLLATION_PROFILE_INVALID",
                                 "sblr.query_execute.collation_stale");
        return false;
      }
      projected.collation_name =
          collation.resource_descriptor.canonical_name;
      projected.collation_generation =
          collation.resource_descriptor.family_epoch;
    }

    std::uint8_t attributes = 0;
    if (column.nullable) attributes |= 0x01u;
    if (column.generated) attributes |= 0x02u;
    if (column.identity_column) attributes |= 0x04u;
    if (projected.charset_variable_width) attributes |= 0x08u;
    if (!AppendUuid(&projection, column.column_uuid.canonical)) {
      *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                               "sblr.query_execute.column_projection_invalid");
      return false;
    }
    AppendU32(&projection, column.ordinal);
    if (!AppendString(&projection, column.canonical_name_key) ||
        !AppendUuid(&projection,
                    column.value_descriptor.descriptor_uuid.canonical) ||
        !AppendString(&projection,
                      column.value_descriptor.descriptor_kind) ||
        !AppendString(&projection,
                      column.value_descriptor.canonical_type_name) ||
        !AppendString(&projection,
                      column.value_descriptor.encoded_descriptor)) {
      return false;
    }
    AppendU8(&projection, attributes);
    if (!AppendUuid(&projection, column.charset_uuid, true) ||
        !AppendString(&projection, projected.charset_name) ||
        !AppendUuid(&projection, column.collation_uuid, true) ||
        !AppendString(&projection, projected.collation_name)) {
      return false;
    }
    AppendU32(&projection, column.character_length);
    AppendU32(&projection, projected.charset_min_bytes);
    AppendU32(&projection, projected.charset_max_bytes);
    AppendU8(&projection, projected.datatype.has_value() ? 1u : 0u);
    if (projected.datatype.has_value()) {
      const auto& row = *projected.datatype;
      AppendU64(&projection, row.descriptor_generation);
      if (!AppendUuid(&projection, row.type_uuid)) return false;
      AppendU64(&projection, row.type_generation);
      if (!AppendString(&projection, row.codec_id)) return false;
      AppendU16(&projection, row.codec_version);
      AppendU64(&projection, row.codec_generation);
      AppendU32(&projection, row.canonical_value_bytes);
      AppendU8(&projection, row.null_supported ? 2u : 1u);
    }
    output->columns.push_back(std::move(projected));
  }
  if (!HashBytes(projection, &output->projection_hash)) {
    *diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
                             "sblr.query_execute.projection_hash_failed");
    return false;
  }
  return true;
}

bool ResolveProjectedSource(const EngineRequestContext& context,
                            const wire::NarrowQuerySourceDemand& demand,
                            ResolvedSource* output,
                            EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr ||
      !demand.relation_object_hint_present) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                               "sblr.query_execute.source_hint_required");
    }
    return false;
  }
  const auto relation_uuid = UuidText(demand.relation_object_uuid_hint);
  const auto loaded = LoadMgaRelationStorageDescriptor(context, relation_uuid);
  if (!loaded.ok || loaded.descriptor.relation_uuid.canonical != relation_uuid) {
    *diagnostic = loaded.ok
                      ? Diagnostic("SBLR.PLAN_TREE.INVALID_HANDLE",
                                   "sblr.query_execute.source_invalid")
                      : loaded.diagnostic;
    return false;
  }
  auto alias = demand.explicit_alias
                   ? demand.alias_spelling
                   : RelationCanonicalName(context, relation_uuid);
  return BuildProjectedSource(context, loaded.descriptor, std::move(alias),
                              output, diagnostic);
}

const ProjectedColumn* FindColumn(const ResolvedSource& source,
                                  std::string_view spelling) {
  const auto found = std::find_if(
      source.columns.begin(), source.columns.end(), [&](const auto& column) {
        return column.descriptor.canonical_name_key == spelling;
      });
  return found == source.columns.end() ? nullptr : &*found;
}

bool FillFreshUuid(std::unordered_set<std::string>* issued,
                   wire::NarrowQueryUuid* output) {
  return ToWireUuid(FreshUuid(issued), output);
}

EngineApiDiagnostic ValidateGrant(
    const EngineNarrowQueryBindingAuthorityIssueRequestV1& request) {
  const auto& demand = request.demand;
  if (request.maximum_source_rows_per_occurrence == 0 ||
      request.maximum_source_rows_per_occurrence > kMaximumProfileRows ||
      request.maximum_cumulative_source_rows == 0 ||
      request.maximum_cumulative_source_rows <
          request.maximum_source_rows_per_occurrence ||
      request.maximum_result_rows == 0 ||
      request.maximum_result_rows > kMaximumProfileRows ||
      request.maximum_join_combinations == 0 ||
      request.maximum_join_combinations > kMaximumProfileRows ||
      request.maximum_sort_memory_bytes == 0 ||
      request.maximum_batch_rows == 0 ||
      request.maximum_batch_rows > request.maximum_result_rows ||
      !ValidMgaRelationDecodedBytesPerPass(
          request.maximum_mga_relation_decoded_bytes_per_pass) ||
      request.maximum_mga_relation_decoded_bytes_per_pass !=
          request.context.maximum_mga_relation_decoded_bytes_per_pass ||
      request.maximum_mga_relation_decoded_bytes_per_pass !=
          demand.maximum_mga_relation_decoded_bytes_per_pass ||
      !ValidTypedResultTransportBytesPerPacket(
          request.maximum_typed_result_transport_bytes_per_packet) ||
      request.maximum_typed_result_transport_bytes_per_packet !=
          request.context.maximum_typed_result_transport_bytes_per_packet ||
      (demand.row_limit_present &&
       demand.row_limit > request.maximum_result_rows) ||
      (demand.row_offset_present &&
       demand.row_offset > request.maximum_result_rows) ||
      (demand.row_limit_present && demand.row_offset_present &&
       (demand.row_limit >
            std::numeric_limits<std::uint64_t>::max() - demand.row_offset ||
        demand.row_limit + demand.row_offset >
            request.maximum_result_rows))) {
    return Diagnostic("RESOURCE.BUDGET_EXCEEDED",
                      "sblr.query_execute.resource_grant_invalid");
  }
  return OkDiagnostic();
}

bool SameSource(const wire::NarrowQuerySourceOccurrence& left,
                const wire::NarrowQuerySourceOccurrence& right) {
  return left.source_ordinal == right.source_ordinal &&
         left.source_occurrence_uuid == right.source_occurrence_uuid &&
         left.source_occurrence_generation ==
             right.source_occurrence_generation &&
         left.relation_descriptor_uuid == right.relation_descriptor_uuid &&
         left.relation_descriptor_generation ==
             right.relation_descriptor_generation &&
         left.relation_object_uuid == right.relation_object_uuid &&
         left.schema_uuid == right.schema_uuid &&
         left.validated_resource_epoch == right.validated_resource_epoch &&
         left.relation_projection_sha256 ==
             right.relation_projection_sha256 &&
         left.alias == right.alias;
}

bool SameOutput(const wire::NarrowQueryOutputOccurrence& left,
                const wire::NarrowQueryOutputOccurrence& right) {
  return left.output_ordinal == right.output_ordinal &&
         left.name_occurrence == right.name_occurrence &&
         left.output_occurrence_uuid == right.output_occurrence_uuid &&
         left.output_occurrence_generation ==
             right.output_occurrence_generation &&
         left.source_occurrence_uuid == right.source_occurrence_uuid &&
         left.source_occurrence_generation ==
             right.source_occurrence_generation &&
         left.source_column_uuid == right.source_column_uuid &&
         left.source_column_ordinal == right.source_column_ordinal &&
         left.output_descriptor_uuid == right.output_descriptor_uuid &&
         left.output_descriptor_generation ==
             right.output_descriptor_generation &&
         left.datatype_descriptor_uuid == right.datatype_descriptor_uuid &&
         left.datatype_descriptor_generation ==
             right.datatype_descriptor_generation &&
         left.datatype_type_uuid == right.datatype_type_uuid &&
         left.datatype_type_generation == right.datatype_type_generation &&
         left.datatype_binary_type_code == right.datatype_binary_type_code &&
         left.codec_version == right.codec_version &&
         left.nullability == right.nullability &&
         left.null_encoding == right.null_encoding &&
         left.codec_generation == right.codec_generation &&
         left.canonical_value_bytes == right.canonical_value_bytes &&
         left.name == right.name && left.codec_id == right.codec_id;
}

bool SameOrdering(const wire::NarrowQueryOrderingTerm& left,
                  const wire::NarrowQueryOrderingTerm& right) {
  return left.term_ordinal == right.term_ordinal &&
         left.ordering_term_uuid == right.ordering_term_uuid &&
         left.ordering_term_generation == right.ordering_term_generation &&
         left.source_occurrence_uuid == right.source_occurrence_uuid &&
         left.source_occurrence_generation ==
             right.source_occurrence_generation &&
         left.source_column_uuid == right.source_column_uuid &&
         left.source_column_ordinal == right.source_column_ordinal &&
         left.direction == right.direction &&
         left.null_placement == right.null_placement &&
         left.collation_uuid == right.collation_uuid &&
         left.collation_generation == right.collation_generation;
}

wire::NarrowQueryBindingValidationContext ValidationContext(
    const wire::NarrowQueryBinding& binding,
    std::uint64_t maximum_result_rows) {
  wire::NarrowQueryBindingValidationContext context;
  context.statement_receipt_uuid = binding.statement_receipt_uuid;
  context.owning_transaction_uuid = binding.owning_transaction_uuid;
  context.owning_local_transaction_id = binding.owning_local_transaction_id;
  context.statement_snapshot_uuid = binding.statement_snapshot_uuid;
  context.datatype_catalog_snapshot_uuid =
      binding.datatype_catalog_snapshot_uuid;
  context.datatype_catalog_generation = binding.datatype_catalog_generation;
  context.datatype_registry_generation = binding.datatype_registry_generation;
  context.security_context_uuid = binding.security_context_uuid;
  context.policy_snapshot_uuid = binding.policy_snapshot_uuid;
  context.policy_generation = binding.policy_generation;
  context.resource_grant_receipt_uuid = binding.resource_grant_receipt_uuid;
  context.resource_grant_generation = binding.resource_grant_generation;
  context.cancellation_receipt_uuid = binding.cancellation_receipt_uuid;
  context.cancellation_generation = binding.cancellation_generation;
  context.execution_uuid = binding.execution_uuid;
  context.result_set_uuid = binding.result_set_uuid;
  context.row_descriptor_uuid = binding.row_descriptor_uuid;
  context.row_descriptor_generation = binding.row_descriptor_generation;
  context.maximum_mga_relation_decoded_bytes_per_pass =
      binding.maximum_mga_relation_decoded_bytes_per_pass;
  context.maximum_result_rows = maximum_result_rows;
  context.validate_canonical_alias = [](std::string_view alias) {
    return !alias.empty() && alias.size() <= 128 &&
           alias.find('\0') == std::string_view::npos;
  };
  context.validate_source = [&binding](const auto& source) {
    const auto found = std::find_if(
        binding.sources.begin(), binding.sources.end(), [&](const auto& row) {
          return row.source_ordinal == source.source_ordinal;
        });
    return found != binding.sources.end() && SameSource(*found, source)
               ? wire::NarrowQueryAuthorityDecision::accepted
               : wire::NarrowQueryAuthorityDecision::stale_or_mismatched;
  };
  context.validate_output_datatype = [&binding](const auto& output) {
    const auto found = std::find_if(
        binding.outputs.begin(), binding.outputs.end(), [&](const auto& row) {
          return row.output_ordinal == output.output_ordinal;
        });
    return found != binding.outputs.end() && SameOutput(*found, output)
               ? wire::NarrowQueryAuthorityDecision::accepted
               : wire::NarrowQueryAuthorityDecision::stale_or_mismatched;
  };
  context.validate_collation = [&binding](const auto& term) {
    const auto found = std::find_if(
        binding.ordering_terms.begin(), binding.ordering_terms.end(),
        [&](const auto& row) { return row.term_ordinal == term.term_ordinal; });
    return found != binding.ordering_terms.end() && SameOrdering(*found, term)
               ? wire::NarrowQueryAuthorityDecision::accepted
               : wire::NarrowQueryAuthorityDecision::stale_or_mismatched;
  };
  return context;
}

bool CheckedAdd(std::uint64_t current,
                std::uint64_t amount,
                std::uint64_t maximum,
                std::uint64_t* result) {
  if (result == nullptr || amount > maximum || current > maximum - amount) {
    return false;
  }
  *result = current + amount;
  return true;
}

}  // namespace

struct EngineNarrowQueryBindingAuthorityHandleV1::Authority {
  std::mutex mutex;
  EngineRequestContext pinned_context;
  wire::NarrowQueryBinding binding;
  std::vector<std::uint8_t> exact_binding_bytes;
  EngineNarrowQueryResourceGrantV1 grant;
  bool consumed = false;
  bool released = false;
  bool typed_result_resource_grant_retained = false;
  std::vector<std::uint64_t> source_rows;
  std::uint64_t cumulative_source_rows = 0;
  std::uint64_t result_rows = 0;
  std::uint64_t join_combinations = 0;
  std::uint64_t sort_memory_bytes = 0;
  std::uint64_t batch_rows = 0;
  // The last complete physical-pass observation for each source occurrence.
  // A later pass replaces this value; decoded bytes are deliberately not
  // accumulated across passes or source occurrences.
  std::vector<std::uint64_t> source_decoded_bytes_last_pass;
};

namespace {

class NarrowQueryTypedResultResourceGrantReceiptHandleV1 final
    : public TypedResultResourceGrantReceiptHandleV1 {
 public:
  NarrowQueryTypedResultResourceGrantReceiptHandleV1(
      std::shared_ptr<EngineNarrowQueryBindingAuthorityHandleV1::Authority>
          authority,
      wire::TypedResultUuid grant_receipt_uuid,
      std::uint64_t grant_generation,
      std::uint64_t resource_epoch,
      std::uint64_t maximum_bytes_per_packet)
      : authority_(std::move(authority)),
        grant_receipt_uuid_(grant_receipt_uuid),
        grant_generation_(grant_generation),
        resource_epoch_(resource_epoch),
        maximum_bytes_per_packet_(maximum_bytes_per_packet) {}

  ~NarrowQueryTypedResultResourceGrantReceiptHandleV1() override {
    Release(TypedResultProducerReleaseReasonV1::shutdown);
  }

  TypedResultProducerGrantObservationV1 ObserveGrant(
      const wire::TypedResultUuid& receipt_uuid,
      std::uint64_t receipt_generation,
      std::uint64_t retained_ceiling_bytes,
      std::uint64_t requested_bytes) override {
    std::lock_guard handle_lock(mutex_);
    if (released_ || !authority_) {
      return TypedResultProducerGrantObservationV1::stale_or_released;
    }

    std::lock_guard authority_lock(authority_->mutex);
    if (authority_->released || !authority_->consumed ||
        authority_->pinned_context.resource_epoch != resource_epoch_ ||
        authority_->pinned_context
                .maximum_typed_result_transport_bytes_per_packet !=
            maximum_bytes_per_packet_ ||
        authority_->grant.maximum_typed_result_transport_bytes_per_packet !=
            maximum_bytes_per_packet_ ||
        authority_->binding.resource_grant_receipt_uuid !=
            grant_receipt_uuid_ ||
        authority_->binding.resource_grant_generation != grant_generation_ ||
        authority_->grant.grant_receipt_uuid.canonical !=
            UuidText(grant_receipt_uuid_) ||
        authority_->grant.grant_generation != grant_generation_ ||
        receipt_uuid != grant_receipt_uuid_ ||
        receipt_generation != grant_generation_ ||
        retained_ceiling_bytes != maximum_bytes_per_packet_) {
      return TypedResultProducerGrantObservationV1::stale_or_released;
    }
    if (requested_bytes == 0 ||
        requested_bytes > maximum_bytes_per_packet_) {
      return TypedResultProducerGrantObservationV1::exhausted;
    }
    return TypedResultProducerGrantObservationV1::live;
  }

  void Release(TypedResultProducerReleaseReasonV1 reason) noexcept override {
    (void)reason;
    std::lock_guard lock(mutex_);
    released_ = true;
    authority_.reset();
  }

 private:
  std::mutex mutex_;
  std::shared_ptr<EngineNarrowQueryBindingAuthorityHandleV1::Authority>
      authority_;
  wire::TypedResultUuid grant_receipt_uuid_{};
  std::uint64_t grant_generation_ = 0;
  std::uint64_t resource_epoch_ = 0;
  std::uint64_t maximum_bytes_per_packet_ = 0;
  bool released_ = false;
};

}  // namespace

EngineNarrowQueryBindingAuthorityIssueResultV1
IssueNarrowQueryBindingAuthorityV1(
    const EngineNarrowQueryBindingAuthorityIssueRequestV1& request) {
  EngineNarrowQueryBindingAuthorityIssueResultV1 result;
  result.diagnostic = ValidateContext(request.context, kBinderTag);
  if (result.diagnostic.error) return result;
  if (!ExactUuid(request.policy_snapshot_uuid.canonical) ||
      request.policy_generation == 0 ||
      request.context.statement_receipt_uuid.canonical !=
          UuidText(request.demand.statement_receipt_uuid) ||
      request.demand.exact_bytes.empty()) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND.INVALID",
        "sblr.query_execute.narrow_issue_request_invalid");
    return result;
  }
  result.diagnostic = ValidateGrant(request);
  if (result.diagnostic.error) return result;

  wire::NarrowQueryBindingDemand decoded_demand;
  wire::NarrowQueryBindingDemandError demand_error;
  wire::NarrowQueryBindingDemandValidationContext demand_context;
  demand_context.authenticated_statement_receipt_uuid =
      request.demand.statement_receipt_uuid;
  demand_context.maximum_mga_relation_decoded_bytes_per_pass =
      request.context.maximum_mga_relation_decoded_bytes_per_pass;
  if (!wire::DecodeAndValidateNarrowQueryBindingDemand(
          request.demand.exact_bytes, demand_context, &decoded_demand,
          &demand_error)) {
    result.diagnostic = DemandDiagnostic(demand_error);
    return result;
  }

  // Catalog descriptors may be inspected before authorization, but no source
  // row is opened.  Every relation is authorized before any output/type/order
  // binding is accepted.
  for (const auto& source : decoded_demand.sources) {
    if (!source.relation_object_hint_present) {
      result.diagnostic = Diagnostic(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "sblr.query_execute.relation_hint_required");
      return result;
    }
    const auto relation_uuid = UuidText(source.relation_object_uuid_hint);
    const auto authorization = EvaluateMaterializedAuthorization(
        request.context, request.context.authorization_context, "SELECT",
        relation_uuid);
    if (!authorization.authorized || authorization.denied ||
        authorization.policy_recheck_required ||
        !authorization.diagnostics.empty()) {
      result.diagnostic = Diagnostic(
          "SECURITY.ACCESS_DENIED",
          "sblr.query_execute.source_access_denied");
      return result;
    }
  }

  std::vector<ResolvedSource> sources(decoded_demand.sources.size());
  for (std::size_t index = 0; index < decoded_demand.sources.size(); ++index) {
    if (!ResolveProjectedSource(request.context,
                                decoded_demand.sources[index],
                                &sources[index], &result.diagnostic)) {
      return result;
    }
  }

  wire::NarrowQueryBinding binding;
  binding.profile = decoded_demand.requested_profile;
  binding.row_limit_present = decoded_demand.row_limit_present;
  binding.row_limit = decoded_demand.row_limit;
  binding.row_offset_present = decoded_demand.row_offset_present;
  binding.row_offset = decoded_demand.row_offset;
  binding.maximum_mga_relation_decoded_bytes_per_pass =
      decoded_demand.maximum_mga_relation_decoded_bytes_per_pass;
  if (!ToWireUuid(request.context.statement_receipt_uuid.canonical,
                  &binding.statement_receipt_uuid) ||
      !ToWireUuid(request.context.transaction_uuid.canonical,
                  &binding.owning_transaction_uuid) ||
      !ToWireUuid(request.context.statement_snapshot_uuid.canonical,
                  &binding.statement_snapshot_uuid) ||
      !ToWireUuid(request.context.datatype_catalog_snapshot_uuid.canonical,
                  &binding.datatype_catalog_snapshot_uuid) ||
      !ToWireUuid(request.context.authorization_context.authority_uuid.canonical,
                  &binding.security_context_uuid) ||
      !ToWireUuid(request.policy_snapshot_uuid.canonical,
                  &binding.policy_snapshot_uuid)) {
    result.diagnostic = Diagnostic(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.context_identity_invalid");
    return result;
  }
  binding.owning_local_transaction_id = request.context.local_transaction_id;
  binding.datatype_catalog_generation =
      request.context.datatype_catalog_generation;
  binding.datatype_registry_generation =
      request.context.datatype_registry_generation;
  binding.policy_generation = request.policy_generation;

  std::unordered_set<std::string> issued;
  const auto issue = [&](wire::NarrowQueryUuid* output) {
    return FillFreshUuid(&issued, output);
  };
  if (!issue(&binding.resource_grant_receipt_uuid) ||
      !issue(&binding.cancellation_receipt_uuid) ||
      !issue(&binding.execution_uuid) || !issue(&binding.result_set_uuid) ||
      !issue(&binding.row_descriptor_uuid) ||
      !issue(&binding.source_vector_uuid) ||
      !issue(&binding.output_vector_uuid) ||
      (binding.profile == wire::NarrowQueryProfile::ordered_projection &&
       !issue(&binding.ordering_vector_uuid))) {
    result.diagnostic = Diagnostic(
        "SBLR.EXECUTION_FAILED",
        "sblr.query_execute.identity_allocation_failed");
    return result;
  }
  binding.resource_grant_generation = 1;
  binding.cancellation_generation = 1;
  binding.row_descriptor_generation = 1;
  binding.source_vector_generation = 1;
  binding.output_vector_generation = 1;
  binding.ordering_vector_generation =
      binding.profile == wire::NarrowQueryProfile::ordered_projection ? 1 : 0;

  binding.sources.reserve(sources.size());
  for (std::size_t index = 0; index < sources.size(); ++index) {
    wire::NarrowQuerySourceOccurrence source;
    source.source_ordinal = static_cast<std::uint32_t>(index);
    if (!issue(&source.source_occurrence_uuid) ||
        !ToWireUuid(sources[index].descriptor.descriptor_uuid.canonical,
                    &source.relation_descriptor_uuid) ||
        !ToWireUuid(sources[index].descriptor.relation_uuid.canonical,
                    &source.relation_object_uuid) ||
        !ToWireUuid(sources[index].descriptor.schema_uuid.canonical,
                    &source.schema_uuid)) {
      result.diagnostic = Diagnostic(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "sblr.query_execute.source_identity_invalid");
      return result;
    }
    source.source_occurrence_generation = 1;
    source.relation_descriptor_generation =
        sources[index].descriptor.descriptor_generation;
    source.validated_resource_epoch = request.context.resource_epoch;
    source.relation_projection_sha256 = sources[index].projection_hash;
    source.alias = sources[index].alias;
    binding.sources.push_back(std::move(source));
  }

  binding.outputs.reserve(decoded_demand.outputs.size());
  std::map<std::string, std::uint32_t> name_occurrences;
  for (const auto& demand : decoded_demand.outputs) {
    if (demand.source_ordinal >= sources.size()) {
      result.diagnostic = Diagnostic(
          "PROJECTION.EXPRESSION_VECTOR_INVALID",
          "sblr.query_execute.output_source_invalid");
      return result;
    }
    const auto* column =
        FindColumn(sources[demand.source_ordinal],
                   demand.source_column_spelling);
    if (column == nullptr || !column->datatype.has_value()) {
      result.diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.query_execute.output_datatype_unavailable");
      return result;
    }
    const auto& datatype = *column->datatype;
    wire::NarrowQueryOutputOccurrence output;
    output.output_ordinal = demand.output_ordinal;
    output.name = demand.output_name_present
                      ? demand.output_name_spelling
                      : column->descriptor.canonical_name_key;
    output.name_occurrence = name_occurrences[output.name]++;
    if (!issue(&output.output_occurrence_uuid) ||
        !issue(&output.output_descriptor_uuid) ||
        !ToWireUuid(column->descriptor.column_uuid.canonical,
                    &output.source_column_uuid) ||
        !ToWireUuid(datatype.descriptor_uuid,
                    &output.datatype_descriptor_uuid) ||
        !ToWireUuid(datatype.type_uuid, &output.datatype_type_uuid)) {
      result.diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.query_execute.output_identity_invalid");
      return result;
    }
    output.output_occurrence_generation = 1;
    output.source_occurrence_uuid =
        binding.sources[demand.source_ordinal].source_occurrence_uuid;
    output.source_occurrence_generation =
        binding.sources[demand.source_ordinal].source_occurrence_generation;
    output.source_column_ordinal = column->descriptor.ordinal;
    output.output_descriptor_generation = 1;
    output.datatype_descriptor_generation = datatype.descriptor_generation;
    output.datatype_type_generation = datatype.type_generation;
    output.datatype_binary_type_code = datatype.canonical_binary_type_code;
    output.codec_version = datatype.codec_version;
    output.nullability = column->descriptor.nullable ? 1u : 0u;
    output.null_encoding = 1;
    output.codec_generation = datatype.codec_generation;
    output.canonical_value_bytes = datatype.canonical_value_bytes;
    output.codec_id = datatype.codec_id;
    binding.outputs.push_back(std::move(output));
  }

  binding.ordering_terms.reserve(decoded_demand.ordering_terms.size());
  for (const auto& demand : decoded_demand.ordering_terms) {
    if (demand.source_ordinal >= sources.size()) {
      result.diagnostic = Diagnostic(
          "SORT.ORDERING_VECTOR_INVALID",
          "sblr.query_execute.order_source_invalid");
      return result;
    }
    const auto* column = FindColumn(sources[demand.source_ordinal],
                                    demand.source_column_spelling);
    if (column == nullptr) {
      result.diagnostic = Diagnostic(
          "SORT.ORDERING_VECTOR_INVALID",
          "sblr.query_execute.order_column_invalid");
      return result;
    }
    wire::NarrowQueryOrderingTerm term;
    term.term_ordinal = demand.term_ordinal;
    if (!issue(&term.ordering_term_uuid) ||
        !ToWireUuid(column->descriptor.column_uuid.canonical,
                    &term.source_column_uuid) ||
        (!column->descriptor.collation_uuid.empty() &&
         !ToWireUuid(column->descriptor.collation_uuid,
                     &term.collation_uuid))) {
      result.diagnostic = Diagnostic(
          "SORT.COLLATION_PROFILE_INVALID",
          "sblr.query_execute.order_identity_invalid");
      return result;
    }
    term.ordering_term_generation = 1;
    term.source_occurrence_uuid =
        binding.sources[demand.source_ordinal].source_occurrence_uuid;
    term.source_occurrence_generation =
        binding.sources[demand.source_ordinal].source_occurrence_generation;
    term.source_column_ordinal = column->descriptor.ordinal;
    term.direction = demand.direction;
    term.null_placement = demand.null_placement;
    term.collation_generation = column->collation_generation;
    binding.ordering_terms.push_back(std::move(term));
  }

  wire::NarrowQueryBindingError binding_error;
  std::vector<std::uint8_t> exact_binding;
  if (!wire::EncodeNarrowQueryBinding(binding, &exact_binding,
                                      &binding_error)) {
    result.diagnostic = CodecDiagnostic(
        binding_error, "sblr.query_execute.narrow_binding_encode_failed");
    return result;
  }
  wire::NarrowQueryBinding decoded_binding;
  auto validation = ValidationContext(binding, request.maximum_result_rows);
  if (!wire::DecodeAndValidateNarrowQueryBinding(
          exact_binding, validation, &decoded_binding, &binding_error)) {
    result.diagnostic = CodecDiagnostic(
        binding_error, "sblr.query_execute.narrow_binding_invalid");
    return result;
  }

  auto authority = std::make_shared<
      EngineNarrowQueryBindingAuthorityHandleV1::Authority>();
  authority->pinned_context = request.context;
  authority->binding = decoded_binding;
  authority->exact_binding_bytes = exact_binding;
  authority->grant.grant_receipt_uuid.canonical =
      UuidText(decoded_binding.resource_grant_receipt_uuid);
  authority->grant.grant_generation =
      decoded_binding.resource_grant_generation;
  authority->grant.maximum_source_rows_per_occurrence =
      request.maximum_source_rows_per_occurrence;
  authority->grant.maximum_cumulative_source_rows =
      request.maximum_cumulative_source_rows;
  authority->grant.maximum_result_rows = request.maximum_result_rows;
  authority->grant.maximum_join_combinations =
      request.maximum_join_combinations;
  authority->grant.maximum_sort_memory_bytes =
      request.maximum_sort_memory_bytes;
  authority->grant.maximum_batch_rows = request.maximum_batch_rows;
  authority->grant.maximum_mga_relation_decoded_bytes_per_pass =
      request.maximum_mga_relation_decoded_bytes_per_pass;
  authority->grant.maximum_typed_result_transport_bytes_per_packet =
      request.maximum_typed_result_transport_bytes_per_packet;
  authority->source_rows.resize(decoded_binding.sources.size(), 0);
  authority->source_decoded_bytes_last_pass.resize(
      decoded_binding.sources.size(), 0);
  {
    std::lock_guard lock(g_authority_mutex);
    const auto& receipt = request.context.statement_receipt_uuid.canonical;
    if (g_authorities_by_receipt.contains(receipt)) {
      result.diagnostic = Diagnostic(
          "MGA.TRANSACTION.STALE",
          "sblr.query_execute.binding_already_issued");
      return result;
    }
    g_authorities_by_receipt.emplace(receipt, authority);
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.binding = std::move(decoded_binding);
  result.exact_binding_bytes = std::move(exact_binding);
  return result;
}

EngineNarrowQueryBindingAuthorityConsumeResultV1
ConsumeNarrowQueryBindingAuthorityV1(
    const EngineNarrowQueryBindingAuthorityConsumeRequestV1& request) {
  EngineNarrowQueryBindingAuthorityConsumeResultV1 result;
  result.diagnostic = ValidateContext(request.context, kConsumerTag);
  if (result.diagnostic.error) return result;
  std::shared_ptr<EngineNarrowQueryBindingAuthorityHandleV1::Authority>
      authority;
  {
    std::lock_guard lock(g_authority_mutex);
    const auto found = g_authorities_by_receipt.find(
        request.context.statement_receipt_uuid.canonical);
    if (found == g_authorities_by_receipt.end()) {
      result.diagnostic = Diagnostic(
          "SECURITY.ACCESS_DENIED",
          "sblr.query_execute.binding_hidden_or_missing");
      return result;
    }
    authority = found->second;
  }
  std::lock_guard lock(authority->mutex);
  if (authority->released || authority->consumed ||
      !SameContext(authority->pinned_context, request.context) ||
      authority->exact_binding_bytes != request.exact_binding_bytes) {
    result.diagnostic = Diagnostic(
        authority->exact_binding_bytes == request.exact_binding_bytes
            ? "MGA.TRANSACTION.STALE"
            : "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.binding_stale_or_mismatched");
    return result;
  }
  wire::NarrowQueryBinding decoded;
  wire::NarrowQueryBindingError error;
  auto validation = ValidationContext(
      authority->binding, authority->grant.maximum_result_rows);
  validation.cancelled =
      request.context.query_cancellation_requested &&
      request.context.query_cancellation_requested();
  if (!wire::DecodeAndValidateNarrowQueryBinding(
          request.exact_binding_bytes, validation, &decoded, &error)) {
    result.diagnostic = CodecDiagnostic(
        error, "sblr.query_execute.binding_revalidation_failed");
    return result;
  }
  authority->consumed = true;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.binding = std::move(decoded);
  result.authority.authority_ = std::move(authority);
  return result;
}

bool CopyNarrowQueryBindingAuthoritySnapshotV1(
    const EngineNarrowQueryBindingAuthorityHandleV1& handle,
    EngineNarrowQueryBindingAuthoritySnapshotV1* snapshot,
    EngineApiDiagnostic* diagnostic) {
  if (snapshot == nullptr || diagnostic == nullptr || !handle.authority_) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "sblr.query_execute.binding_handle_invalid");
    }
    return false;
  }
  std::lock_guard lock(handle.authority_->mutex);
  if (handle.authority_->released || !handle.authority_->consumed) {
    *diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.query_execute.binding_handle_stale");
    return false;
  }
  snapshot->pinned_context = handle.authority_->pinned_context;
  snapshot->binding = handle.authority_->binding;
  snapshot->resource_grant = handle.authority_->grant;
  *diagnostic = OkDiagnostic();
  return true;
}

EngineNarrowQueryTypedResultResourceGrantRetentionResultV1
RetainNarrowQueryTypedResultResourceGrantReceiptV1(
    const EngineNarrowQueryBindingAuthorityHandleV1& handle,
    const EngineRequestContext& context) {
  EngineNarrowQueryTypedResultResourceGrantRetentionResultV1 result;
  if (!handle.authority_) {
    result.diagnostic = Diagnostic(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.typed_result_grant_handle_invalid");
    return result;
  }
  result.diagnostic = ValidateContext(context, kConsumerTag);
  if (result.diagnostic.error) return result;

  auto& authority = *handle.authority_;
  std::lock_guard lock(authority.mutex);
  if (authority.released || !authority.consumed ||
      !SameContext(authority.pinned_context, context)) {
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.query_execute.typed_result_grant_lifetime_stale");
    return result;
  }
  if (authority.typed_result_resource_grant_retained) {
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.query_execute.typed_result_grant_already_retained");
    return result;
  }

  const auto& grant = authority.grant;
  const auto& binding = authority.binding;
  const auto grant_receipt_text =
      UuidText(binding.resource_grant_receipt_uuid);
  if (grant_receipt_text.empty() ||
      grant.grant_receipt_uuid.canonical != grant_receipt_text ||
      binding.resource_grant_generation == 0 ||
      grant.grant_generation != binding.resource_grant_generation ||
      !ValidTypedResultTransportBytesPerPacket(
          grant.maximum_typed_result_transport_bytes_per_packet) ||
      grant.maximum_typed_result_transport_bytes_per_packet !=
          context.maximum_typed_result_transport_bytes_per_packet) {
    result.diagnostic = Diagnostic(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.query_execute.typed_result_resource_grant_invalid");
    return result;
  }

  std::unique_ptr<TypedResultResourceGrantReceiptHandleV1> receipt_handle;
  try {
    receipt_handle =
        std::make_unique<NarrowQueryTypedResultResourceGrantReceiptHandleV1>(
            handle.authority_, binding.resource_grant_receipt_uuid,
            binding.resource_grant_generation, context.resource_epoch,
            grant.maximum_typed_result_transport_bytes_per_packet);
  } catch (const std::bad_alloc&) {
    result.diagnostic = Diagnostic(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.query_execute.typed_result_grant_retention_allocation_failed");
    return result;
  }

  authority.typed_result_resource_grant_retained = true;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.grant_receipt_uuid = binding.resource_grant_receipt_uuid;
  result.grant_generation = binding.resource_grant_generation;
  result.maximum_typed_result_transport_bytes_per_packet =
      grant.maximum_typed_result_transport_bytes_per_packet;
  result.receipt_handle = std::move(receipt_handle);
  return result;
}

EngineNarrowQuerySourceOccurrenceAuthorityResultV1
RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
    const EngineNarrowQueryBindingAuthorityHandleV1& handle,
    const EngineRequestContext& context,
    std::uint32_t source_ordinal,
    const MgaRelationStorageDescriptor& current_descriptor) {
  EngineNarrowQuerySourceOccurrenceAuthorityResultV1 result;
  if (!handle.authority_) {
    result.stale = true;
    result.diagnostic = Diagnostic(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.source_revalidation_handle_invalid");
    return result;
  }
  const auto context_diagnostic = ValidateContext(context, kConsumerTag);
  if (context_diagnostic.error) {
    result.stale = true;
    result.diagnostic = context_diagnostic;
    return result;
  }

  auto& authority = *handle.authority_;
  std::lock_guard lock(authority.mutex);
  if (authority.released || !authority.consumed ||
      !SameContext(authority.pinned_context, context)) {
    result.stale = true;
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.query_execute.source_revalidation_lifetime_stale");
    return result;
  }
  if (source_ordinal >= authority.binding.sources.size()) {
    result.stale = true;
    result.diagnostic = Diagnostic(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.source_revalidation_ordinal_stale");
    return result;
  }

  const auto& retained = authority.binding.sources[source_ordinal];
  ResolvedSource current;
  EngineApiDiagnostic projection_diagnostic;
  if (!BuildProjectedSource(context, current_descriptor, retained.alias,
                            &current, &projection_diagnostic)) {
    result.stale = true;
    result.diagnostic = projection_diagnostic.error
                            ? std::move(projection_diagnostic)
                            : Diagnostic(
                                  "SBLR.PLAN_TREE.INVALID_HANDLE",
                                  "sblr.query_execute."
                                  "source_revalidation_projection_stale");
    return result;
  }

  if (retained.source_ordinal != source_ordinal ||
      retained.validated_resource_epoch != context.resource_epoch ||
      UuidText(retained.relation_descriptor_uuid) !=
          current.descriptor.descriptor_uuid.canonical ||
      retained.relation_descriptor_generation !=
          current.descriptor.descriptor_generation ||
      UuidText(retained.relation_object_uuid) !=
          current.descriptor.relation_uuid.canonical ||
      UuidText(retained.schema_uuid) !=
          current.descriptor.schema_uuid.canonical ||
      retained.relation_projection_sha256 != current.projection_hash) {
    result.stale = true;
    result.diagnostic = Diagnostic(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.source_revalidation_projection_stale");
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineNarrowQueryBindingLivenessResultV1
ObserveNarrowQueryBindingLivenessV1(
    EngineNarrowQueryBindingAuthorityHandleV1* handle,
    const EngineRequestContext& context,
    EngineNarrowQueryWorkClassV1 work_class,
    std::uint32_t source_ordinal,
    std::uint64_t amount) {
  EngineNarrowQueryBindingLivenessResultV1 result;
  if (handle == nullptr || !handle->authority_) {
    result.stale = true;
    result.diagnostic = Diagnostic(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "sblr.query_execute.binding_handle_invalid");
    return result;
  }
  auto& authority = *handle->authority_;
  std::lock_guard lock(authority.mutex);
  if (authority.released || !authority.consumed ||
      !SameContext(authority.pinned_context, context)) {
    result.stale = true;
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.query_execute.binding_lifetime_stale");
    return result;
  }
  if ((authority.pinned_context.query_cancellation_requested &&
       authority.pinned_context.query_cancellation_requested()) ||
      (context.query_cancellation_requested &&
       context.query_cancellation_requested())) {
    result.cancelled = true;
    result.diagnostic = Diagnostic(
        "PROCESS.CANCELLED", "sblr.query_execute.cancelled");
    return result;
  }
  std::uint64_t charged = 0;
  bool admitted = true;
  switch (work_class) {
    case EngineNarrowQueryWorkClassV1::liveness_only:
      admitted = amount == 0;
      break;
    case EngineNarrowQueryWorkClassV1::source_rows:
      admitted = source_ordinal < authority.source_rows.size() &&
                 CheckedAdd(
                     authority.source_rows[source_ordinal], amount,
                     authority.grant.maximum_source_rows_per_occurrence,
                     &charged);
      if (admitted) {
        std::uint64_t cumulative = 0;
        admitted = CheckedAdd(
            authority.cumulative_source_rows, amount,
            authority.grant.maximum_cumulative_source_rows, &cumulative);
        if (admitted) {
          authority.source_rows[source_ordinal] = charged;
          authority.cumulative_source_rows = cumulative;
        }
      }
      break;
    case EngineNarrowQueryWorkClassV1::result_rows:
      admitted = CheckedAdd(authority.result_rows, amount,
                            authority.grant.maximum_result_rows, &charged);
      if (admitted) authority.result_rows = charged;
      break;
    case EngineNarrowQueryWorkClassV1::join_combinations:
      admitted = CheckedAdd(authority.join_combinations, amount,
                            authority.grant.maximum_join_combinations,
                            &charged);
      if (admitted) authority.join_combinations = charged;
      break;
    case EngineNarrowQueryWorkClassV1::sort_memory_bytes:
      admitted = CheckedAdd(authority.sort_memory_bytes, amount,
                            authority.grant.maximum_sort_memory_bytes,
                            &charged);
      if (admitted) authority.sort_memory_bytes = charged;
      break;
    case EngineNarrowQueryWorkClassV1::batch_rows:
      admitted = amount <= authority.grant.maximum_batch_rows;
      if (admitted) authority.batch_rows = amount;
      break;
    case EngineNarrowQueryWorkClassV1::mga_relation_decoded_bytes_per_pass:
      admitted =
          source_ordinal < authority.source_decoded_bytes_last_pass.size() &&
          amount <= authority.grant.maximum_mga_relation_decoded_bytes_per_pass;
      if (admitted) {
        authority.source_decoded_bytes_last_pass[source_ordinal] = amount;
      }
      break;
  }
  if (!admitted) {
    result.resource_exhausted = true;
    result.diagnostic = Diagnostic(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.query_execute.resource_grant_exhausted");
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineNarrowQueryPublicationChargeStatusV1
CommitNarrowQueryPublicationChargeV1(
    EngineNarrowQueryBindingAuthorityHandleV1* handle,
    const EngineRequestContext& context,
    std::uint64_t result_rows,
    std::uint64_t batch_rows) noexcept {
  using Status = EngineNarrowQueryPublicationChargeStatusV1;
  try {
    if (handle == nullptr || !handle->authority_) return Status::stale;
    const auto authority = handle->authority_;
    std::unique_lock lock(authority->mutex);
    if (authority->released || !authority->consumed ||
        !SameContext(authority->pinned_context, context)) {
      return Status::stale;
    }

    bool cancelled = false;
    try {
      cancelled =
          (authority->pinned_context.query_cancellation_requested &&
           authority->pinned_context.query_cancellation_requested()) ||
          (context.query_cancellation_requested &&
           context.query_cancellation_requested());
    } catch (...) {
      return Status::stale;
    }
    if (cancelled) return Status::cancelled;

    std::uint64_t next_result_rows = 0;
    if (!CheckedAdd(authority->result_rows, result_rows,
                    authority->grant.maximum_result_rows,
                    &next_result_rows) ||
        batch_rows > authority->grant.maximum_batch_rows) {
      return Status::resource_budget_exceeded;
    }

    // Paired no-fail mutation: all authority, cancellation, overflow, and
    // per-batch checks completed above while the same lock is held.
    authority->result_rows = next_result_rows;
    authority->batch_rows = batch_rows;
    return Status::committed;
  } catch (...) {
    // Lock acquisition and any future observation implementation remain
    // fail-closed without entering the paired mutation above.
    return Status::stale;
  }
}

EngineApiDiagnostic ReleaseNarrowQueryBindingAuthorityV1(
    EngineNarrowQueryBindingAuthorityHandleV1* handle) {
  if (handle == nullptr || !handle->authority_) return OkDiagnostic();
  auto authority = std::move(handle->authority_);
  {
    std::lock_guard lock(authority->mutex);
    authority->released = true;
  }
  std::lock_guard registry_lock(g_authority_mutex);
  const auto& receipt = authority->pinned_context.statement_receipt_uuid.canonical;
  const auto found = g_authorities_by_receipt.find(receipt);
  if (found != g_authorities_by_receipt.end() &&
      found->second == authority) {
    g_authorities_by_receipt.erase(found);
  }
  return OkDiagnostic();
}

void RevokeNarrowQueryBindingAuthorityForReceiptV1(
    const std::string& statement_receipt_uuid) {
  std::shared_ptr<EngineNarrowQueryBindingAuthorityHandleV1::Authority>
      authority;
  {
    std::lock_guard lock(g_authority_mutex);
    const auto found = g_authorities_by_receipt.find(statement_receipt_uuid);
    if (found == g_authorities_by_receipt.end()) return;
    authority = found->second;
    g_authorities_by_receipt.erase(found);
  }
  std::lock_guard lock(authority->mutex);
  authority->released = true;
}

}  // namespace scratchbird::engine::internal_api
