// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/contextual_text_graph_authority_verifier_v2.hpp"

#include "api_diagnostics.hpp"
#include "datatype_catalog_manifest.hpp"
#include "hash_digest.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_stream.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using Sha256 = sblr::ContextualTextSha256V2;
using Uuid = sblr::ContextualTextUuidV2;

std::mutex g_selector_provider_mutex;
std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorProviderV2>
    g_selector_provider;

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

bool Nonzero(const Uuid& value) {
  return std::ranges::any_of(value,
                             [](std::uint8_t byte) { return byte != 0; });
}

bool Nonzero(const Sha256& value) {
  return std::ranges::any_of(value,
                             [](std::uint8_t byte) { return byte != 0; });
}

std::string UuidText(const Uuid& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      out.push_back('-');
    }
    out.push_back(hex[value[index] >> 4]);
    out.push_back(hex[value[index] & 0x0f]);
  }
  return out;
}

bool SameContext(const EngineRequestContext& left,
                 const EngineRequestContext& right) {
  return left.security_context_present && right.security_context_present &&
         left.database_uuid.canonical == right.database_uuid.canonical &&
         left.session_uuid.canonical == right.session_uuid.canonical &&
         left.transaction_uuid.canonical == right.transaction_uuid.canonical &&
         left.statement_uuid.canonical == right.statement_uuid.canonical &&
         left.statement_receipt_uuid.canonical ==
             right.statement_receipt_uuid.canonical &&
         left.statement_snapshot_uuid.canonical ==
             right.statement_snapshot_uuid.canonical &&
         left.datatype_catalog_snapshot_uuid.canonical ==
             right.datatype_catalog_snapshot_uuid.canonical &&
         left.datatype_catalog_generation == right.datatype_catalog_generation &&
         left.datatype_registry_generation ==
             right.datatype_registry_generation &&
         left.security_epoch == right.security_epoch &&
         left.resource_epoch == right.resource_epoch;
}

bool ExactDescriptor(
    const EngineContextualTextGraphDescriptorV2& descriptor,
    const sblr::ContextualTextLiteralProfileV2& profile) {
  if (descriptor.descriptor_handle != profile.literal_descriptor_handle ||
      descriptor.canonical_type_name != "text" ||
      !descriptor.element_profile_empty ||
      (profile.target_character_limit >
           std::numeric_limits<std::uint32_t>::max() &&
       profile.target_character_limit !=
           std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  const std::string width =
      profile.target_character_limit ==
              std::numeric_limits<std::uint64_t>::max()
          ? "-"
          : std::to_string(profile.target_character_limit);
  const std::array<std::string, 17> expected{
      UuidText(profile.descriptor_uuid),
      std::to_string(profile.descriptor_generation),
      UuidText(profile.type_uuid),
      std::to_string(profile.type_generation),
      sblr::kContextualTextCodecIdentifierV2,
      std::to_string(profile.codec_version),
      std::to_string(profile.codec_generation),
      "0",
      UuidText(profile.collation_uuid),
      "-",
      width,
      "-",
      "-",
      UuidText(profile.statement_receipt_uuid),
      UuidText(profile.catalog_snapshot_uuid),
      std::to_string(profile.catalog_generation),
      std::to_string(profile.datatype_registry_generation)};
  return descriptor.exact_relational_descriptor_v2_fields == expected;
}

bool ExactTargetDescriptor(
    const EngineContextualTextGraphDescriptorV2& descriptor,
    const sblr::ContextualTextLiteralProfileV2& profile) {
  if (descriptor.descriptor_handle != profile.target_descriptor_handle ||
      descriptor.canonical_type_name != "text" ||
      !descriptor.element_profile_empty ||
      (profile.target_character_limit >
           std::numeric_limits<std::uint32_t>::max() &&
       profile.target_character_limit !=
           std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  const auto& actual = descriptor.exact_relational_descriptor_v2_fields;
  if (actual[7] != "0" && actual[7] != "1") return false;
  const std::string width =
      profile.target_character_limit ==
              std::numeric_limits<std::uint64_t>::max()
          ? "-"
          : std::to_string(profile.target_character_limit);
  const std::array<std::string, 17> expected{
      UuidText(profile.descriptor_uuid),
      std::to_string(profile.descriptor_generation),
      UuidText(profile.type_uuid),
      std::to_string(profile.type_generation),
      sblr::kContextualTextCodecIdentifierV2,
      std::to_string(profile.codec_version),
      std::to_string(profile.codec_generation),
      actual[7],
      UuidText(profile.collation_uuid),
      "-",
      width,
      "-",
      "-",
      UuidText(profile.statement_receipt_uuid),
      UuidText(profile.catalog_snapshot_uuid),
      std::to_string(profile.catalog_generation),
      std::to_string(profile.datatype_registry_generation)};
  return actual == expected;
}

bool SameSource(const EngineContextualTextGraphSourceV2& left,
                const EngineContextualTextGraphSourceV2& right) {
  return left.node_id == right.node_id &&
         left.top_level_operand_ordinal == right.top_level_operand_ordinal &&
         left.source_ordinal == right.source_ordinal &&
         left.node_kind_is_scan == right.node_kind_is_scan &&
         left.semantic_variant_is_catalog_or_model_source ==
             right.semantic_variant_is_catalog_or_model_source &&
         left.required_relation_uuid == right.required_relation_uuid;
}

bool SourceInventoryCanonical(
    const std::vector<EngineContextualTextGraphSourceV2>& sources) {
  if (sources.empty()) return false;
  std::uint32_t previous_operand_ordinal = 0;
  std::set<std::uint32_t> node_ids;
  for (std::size_t index = 0; index != sources.size(); ++index) {
    const auto& source = sources[index];
    if (index > std::numeric_limits<std::uint32_t>::max() ||
        source.node_id == 0 || source.top_level_operand_ordinal == 0 ||
        source.top_level_operand_ordinal <= previous_operand_ordinal ||
        source.source_ordinal != static_cast<std::uint32_t>(index) ||
        !source.node_kind_is_scan ||
        !source.semantic_variant_is_catalog_or_model_source ||
        !Nonzero(source.required_relation_uuid) ||
        !node_ids.insert(source.node_id).second) {
      return false;
    }
    previous_operand_ordinal = source.top_level_operand_ordinal;
  }
  return true;
}

bool SameLiteralNode(const sblr::SblrExpressionLiteralNodeV1& left,
                     const sblr::SblrExpressionLiteralNodeV1& right) {
  return left.node_id == right.node_id &&
         left.parent_node_id == right.parent_node_id &&
         left.parent_operand_ordinal == right.parent_operand_ordinal &&
         left.descriptor_generation == right.descriptor_generation &&
         left.descriptor_uuid == right.descriptor_uuid &&
         left.literal_body == right.literal_body;
}

std::uint64_t LoadU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index != 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
  }
  return value;
}

bool ParseUnsigned(const std::string_view text, const std::uint64_t maximum,
                   std::uint64_t* out) {
  if (out == nullptr || text.empty() ||
      (text.size() > 1 && text.front() == '0')) {
    return false;
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value > maximum) {
    return false;
  }
  *out = value;
  return true;
}

bool ParseDirectHandle(const std::string_view text, std::uint32_t* out) {
  std::uint64_t value = 0;
  if (!ParseUnsigned(text, std::numeric_limits<std::uint32_t>::max(),
                     &value) ||
      value == 0 || out == nullptr) {
    return false;
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

bool ParseSlotHandle(const std::string_view text, std::uint32_t* out) {
  constexpr std::string_view prefix = "slot_";
  return text.starts_with(prefix) &&
         ParseDirectHandle(text.substr(prefix.size()), out);
}

template <std::size_t Count>
bool SplitFields(const std::string_view encoded,
                 std::array<std::string_view, Count>* fields) {
  if (fields == nullptr) return false;
  std::size_t start = 0;
  for (std::size_t index = 0; index != Count; ++index) {
    const auto separator = encoded.find('|', start);
    if (index + 1 == Count) {
      if (separator != std::string_view::npos) return false;
      (*fields)[index] = encoded.substr(start);
      return true;
    }
    if (separator == std::string_view::npos) return false;
    (*fields)[index] = encoded.substr(start, separator - start);
    start = separator + 1;
  }
  return false;
}

bool DecodeHex(const std::string_view encoded, std::string* out) {
  if (out == nullptr || encoded.size() % 2 != 0) return false;
  const auto nibble = [](const char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    return -1;
  };
  out->clear();
  out->reserve(encoded.size() / 2);
  for (std::size_t index = 0; index != encoded.size(); index += 2) {
    const auto high = nibble(encoded[index]);
    const auto low = nibble(encoded[index + 1]);
    if (high < 0 || low < 0) return false;
    out->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

bool DecodeOptionalHex(const std::string_view encoded,
                       std::optional<std::string>* out) {
  if (out == nullptr) return false;
  if (encoded == "-") {
    out->reset();
    return true;
  }
  std::string decoded;
  if (!DecodeHex(encoded, &decoded)) return false;
  *out = std::move(decoded);
  return true;
}

bool ParseHandleList(const std::string_view encoded,
                     std::vector<std::uint32_t>* out) {
  if (out == nullptr || encoded.empty()) return false;
  out->clear();
  if (encoded == "-") return true;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find(',', start);
    const auto token = encoded.substr(
        start, separator == std::string_view::npos
                   ? encoded.size() - start
                   : separator - start);
    std::uint64_t value = 0;
    if (!ParseUnsigned(token, std::numeric_limits<std::uint32_t>::max(),
                       &value) ||
        value == 0) {
      return false;
    }
    out->push_back(static_cast<std::uint32_t>(value));
    if (out->size() > 131072) return false;
    if (separator == std::string_view::npos) return true;
    start = separator + 1;
  }
  return false;
}

bool ParseStringList(const std::string_view encoded,
                     std::vector<std::string>* out) {
  if (out == nullptr || encoded.empty()) return false;
  out->clear();
  if (encoded == "-") return true;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find(',', start);
    const auto token = encoded.substr(
        start, separator == std::string_view::npos
                   ? encoded.size() - start
                   : separator - start);
    if (token.empty()) return false;
    out->emplace_back(token);
    if (out->size() > 524288) return false;
    if (separator == std::string_view::npos) return true;
    start = separator + 1;
  }
  return false;
}

bool TypedPayload(const sblr::SblrOperand& operand,
                  std::string_view* payload) {
  if (payload == nullptr ||
      operand.value_kind != sblr::SblrValueKind::literal_typed ||
      operand.value_body.size() < 24) {
    return false;
  }
  const auto size = LoadU64(operand.value_body.data() + 16);
  if (size != operand.value_body.size() - 24) return false;
  *payload = std::string_view(
      reinterpret_cast<const char*>(operand.value_body.data() + 24),
      static_cast<std::size_t>(size));
  return true;
}

bool ParseUuidText(const std::string_view text, Uuid* out) {
  if (out == nullptr) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
  return true;
}

struct CanonicalNodeRecord {
  std::uint32_t node_id = 0;
  std::uint8_t node_kind = 0;
  std::uint32_t top_level_operand_ordinal = 0;
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<std::uint32_t> bound_expression_ids;
  std::vector<std::string> required_object_uuids;
  std::string semantic_variant;
  bool binding_present = false;
};

struct CanonicalDescriptorRecord {
  std::uint32_t handle = 0;
  std::array<std::string, 17> fields{};
};

struct CanonicalExpressionRecord {
  std::uint32_t expression_id = 0;
  std::uint8_t kind = 0;
  std::vector<std::uint32_t> children;
  std::uint32_t result_descriptor_handle = 0;
  std::optional<std::string> function_uuid;
  std::optional<std::string> bound_name_uuid;
  std::optional<std::uint8_t> literal_kind;
  std::optional<std::string> operator_name;
  std::optional<std::string> literal_or_parameter_ref;
};

struct CanonicalLiteralReferenceRecord {
  std::uint32_t expression_id = 0;
  sblr::SblrExpressionNodeReferenceV1 reference;
};

struct CanonicalOutputRecord {
  std::uint32_t output_id = 0;
  std::uint32_t node_id = 0;
  std::uint32_t expression_id = 0;
  std::uint32_t descriptor_handle = 0;
  bool visible = false;
  std::uint32_t ordinal = 0;
};

bool ParseDescriptorV2(const sblr::SblrOperand& operand,
                       CanonicalDescriptorRecord* out) {
  if (out == nullptr || !ParseSlotHandle(operand.name, &out->handle)) {
    return false;
  }
  std::string_view payload;
  std::array<std::string_view, 17> fields{};
  if (!TypedPayload(operand, &payload) || payload.size() > 65536 ||
      !SplitFields(payload, &fields)) {
    return false;
  }
  Uuid uuid{};
  std::uint64_t numeric = 0;
  if (!ParseUuidText(fields[0], &uuid) ||
      !ParseUnsigned(fields[1], std::numeric_limits<std::uint64_t>::max(),
                     &numeric) ||
      numeric == 0 || !ParseUuidText(fields[2], &uuid) ||
      !ParseUnsigned(fields[3], std::numeric_limits<std::uint64_t>::max(),
                     &numeric) ||
      numeric == 0 || fields[4].empty() ||
      fields[4].find('|') != std::string_view::npos ||
      !ParseUnsigned(fields[5], std::numeric_limits<std::uint16_t>::max(),
                     &numeric) ||
      numeric == 0 ||
      !ParseUnsigned(fields[6], std::numeric_limits<std::uint64_t>::max(),
                     &numeric) ||
      numeric == 0 || !ParseUnsigned(fields[7], 1, &numeric) ||
      (fields[8] != "-" && !ParseUuidText(fields[8], &uuid)) ||
      !ParseUuidText(fields[13], &uuid) ||
      !ParseUuidText(fields[14], &uuid) ||
      !ParseUnsigned(fields[15], std::numeric_limits<std::uint64_t>::max(),
                     &numeric) ||
      numeric == 0 ||
      !ParseUnsigned(fields[16], std::numeric_limits<std::uint64_t>::max(),
                     &numeric) ||
      numeric == 0) {
    return false;
  }
  std::optional<std::string> optional_hex;
  if (!DecodeOptionalHex(fields[9], &optional_hex)) return false;
  for (const auto index : {10U, 11U, 12U}) {
    if (fields[index] != "-" &&
        !ParseUnsigned(fields[index],
                       std::numeric_limits<std::uint32_t>::max(), &numeric)) {
      return false;
    }
  }
  for (std::size_t index = 0; index != fields.size(); ++index) {
    out->fields[index] = std::string(fields[index]);
  }
  return true;
}

bool DescriptorIsLiveText(
    const EngineRequestContext& context,
    const CanonicalDescriptorRecord& descriptor) {
  std::uint64_t descriptor_generation = 0;
  std::uint64_t type_generation = 0;
  std::uint64_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t registry_generation = 0;
  const auto& fields = descriptor.fields;
  if (!ParseUnsigned(fields[1], std::numeric_limits<std::uint64_t>::max(),
                     &descriptor_generation) ||
      !ParseUnsigned(fields[3], std::numeric_limits<std::uint64_t>::max(),
                     &type_generation) ||
      !ParseUnsigned(fields[5], std::numeric_limits<std::uint16_t>::max(),
                     &codec_version) ||
      !ParseUnsigned(fields[6], std::numeric_limits<std::uint64_t>::max(),
                     &codec_generation) ||
      !ParseUnsigned(fields[15], std::numeric_limits<std::uint64_t>::max(),
                     &catalog_generation) ||
      !ParseUnsigned(fields[16], std::numeric_limits<std::uint64_t>::max(),
                     &registry_generation) ||
      fields[13] != context.statement_receipt_uuid.canonical ||
      fields[14] != context.datatype_catalog_snapshot_uuid.canonical ||
      catalog_generation != context.datatype_catalog_generation ||
      registry_generation != context.datatype_registry_generation) {
    return false;
  }
  const auto identity =
      scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
          fields[14], catalog_generation, registry_generation, fields[0],
          descriptor_generation);
  return identity.ok &&
         scratchbird::core::datatypes::
             IsExactCanonicalTextTypeCodecIdentityV1(identity.row) &&
         identity.row.type_uuid == fields[2] &&
         identity.row.type_generation == type_generation &&
         identity.row.codec_id == fields[4] &&
         identity.row.codec_version == codec_version &&
         identity.row.codec_generation == codec_generation;
}

class CanonicalOperandGraphSelector final
    : public EngineContextualTextCanonicalGraphSelectorV2 {
 public:
  bool SelectCanonicalGraph(
      const EngineRequestContext& exact_live_context,
      const std::vector<std::uint8_t>& exact_sbel_v1,
      const std::vector<std::uint8_t>& exact_canonical_sbos,
      const EngineContextualTextComposedTransferRecordV2& composed_transfer,
      const sblr::ContextualTextLiteralExecuteV2& execute,
      const std::vector<std::uint8_t>& exact_pre_contextual_operand_records,
      const std::uint32_t pre_contextual_operand_count,
      const std::vector<std::uint8_t>& exact_sbxn,
      EngineContextualTextCanonicalGraphSnapshotV2* out,
      EngineApiDiagnostic* diagnostic) const override {
    if (out == nullptr || exact_sbel_v1.empty() ||
        composed_transfer.exact_evidence_material.empty() ||
        exact_canonical_sbos.empty() || execute.exact_bytes.empty()) {
      return Fail("SBLR.OPERAND_INVALID",
                  "engine.contextual_text_graph.selector_input_invalid",
                  diagnostic);
    }
    try {
      const auto records = sblr::DecodeSblrCanonicalOperandRecords(
          exact_pre_contextual_operand_records.data(),
          exact_pre_contextual_operand_records.size(),
          pre_contextual_operand_count,
          sblr::SblrOperandRecordDecodeProfile::
              contextual_query_execute_v1_1_pre_kind206);
      if (!records.ok ||
          records.canonical_bytes != exact_pre_contextual_operand_records) {
        return Fail("SBLR.OPERAND_INVALID",
                    "engine.contextual_text_graph.operand_records_invalid",
                    diagnostic);
      }

      const std::string_view sbos_bytes(
          reinterpret_cast<const char*>(exact_canonical_sbos.data()),
          exact_canonical_sbos.size());
      const auto sbos = sblr::DecodeSblrOpcodeStream(sbos_bytes);
      if (!sbos.ok || sbos.canonical_bytes != exact_canonical_sbos ||
          sbos.stream.operations.size() != 3) {
        return Fail("SBLR.OPERAND_INVALID",
                    "engine.contextual_text_graph.sbos_shape_invalid",
                    diagnostic);
      }
      const auto& operation = sbos.stream.operations[1];
      if (operation.operation_id != "query.execute" ||
          operation.opcode != "SBLR_QUERY_EXECUTE" ||
          operation.opcode_code != 4615 ||
          operation.operation_version_major != 1 ||
          operation.operation_version_minor != 1 ||
          operation.operands.size() != pre_contextual_operand_count + 1 ||
          operation.operands.back().value_kind !=
              sblr::SblrValueKind::contextual_text_literal_profile_set ||
          operation.operands.back().type !=
              "literal.contextual_text_profile_set.v2" ||
          operation.operands.back().name != "contextual_text_profiles" ||
          operation.operands.back().value_body != execute.exact_bytes) {
        return Fail("SBLR.OPERAND_INVALID",
                    "engine.contextual_text_graph.sbos_operation_invalid",
                    diagnostic);
      }
      const std::vector<sblr::SblrOperand> operation_prefix(
          operation.operands.begin(), operation.operands.end() - 1);
      if (sblr::EncodeSblrCanonicalOperandRecords(operation_prefix) !=
          exact_pre_contextual_operand_records) {
        return Fail("SBLR.OPERAND_INVALID",
                    "engine.contextual_text_graph.sbos_prefix_mismatch",
                    diagnostic);
      }

      const auto decoded_sbxn =
          sblr::DecodeSblrContextualComposedExpressionNodeTableV2(
              exact_sbxn.data(), exact_sbxn.size());
      if (!decoded_sbxn.ok || decoded_sbxn.canonical_bytes != exact_sbxn) {
        return Fail("SBLR.OPERAND_INVALID",
                    "engine.contextual_text_graph.sbxn_invalid", diagnostic);
      }

      std::unordered_map<std::uint32_t, CanonicalNodeRecord> nodes;
      std::unordered_map<std::uint32_t, CanonicalDescriptorRecord>
          descriptors;
      std::unordered_map<std::uint32_t, CanonicalExpressionRecord>
          expressions;
      std::vector<CanonicalLiteralReferenceRecord> literal_references;
      std::vector<CanonicalOutputRecord> outputs;
      std::unordered_set<std::uint32_t> expression_ids;
      std::unordered_set<std::uint32_t> descriptor_handles;
      std::size_t sbxn_table_count = 0;
      for (const auto& operand : records.operands) {
        if (operand.value_kind == sblr::SblrValueKind::expression_node_table) {
          if (++sbxn_table_count != 1 ||
              operand.type != "expression.node_table.v1" ||
              operand.name != "expression_nodes" ||
              operand.value_body != exact_sbxn) {
            return Invalid("sbxn_carrier_invalid", diagnostic);
          }
          continue;
        }
        if (operand.type == "relational_node_v1") {
          CanonicalNodeRecord node;
          std::string_view payload;
          std::array<std::string_view, 5> fields{};
          std::uint64_t kind = 0;
          std::vector<std::uint32_t> ignored;
          if (!ParseSlotHandle(operand.name, &node.node_id) ||
              !TypedPayload(operand, &payload) ||
              !SplitFields(payload, &fields) ||
              !ParseUnsigned(fields[0],
                             std::numeric_limits<std::uint8_t>::max(),
                             &kind) ||
              (fields[1] != "0" && fields[1] != "1") ||
              !ParseHandleList(fields[2], &ignored) ||
              !ParseHandleList(fields[3], &node.output_descriptor_ids) ||
              !ParseHandleList(fields[4], &ignored)) {
            return Invalid("node_record_invalid", diagnostic);
          }
          node.node_kind = static_cast<std::uint8_t>(kind);
          node.top_level_operand_ordinal = operand.ordinal;
          if (!nodes.emplace(node.node_id, std::move(node)).second) {
            return Invalid("node_duplicate", diagnostic);
          }
          continue;
        }
        if (operand.type == "relational_node_binding_v1") {
          std::uint32_t node_id = 0;
          std::string_view payload;
          std::array<std::string_view, 5> fields{};
          std::vector<std::string> ignored;
          const auto found = [&]() -> CanonicalNodeRecord* {
            if (!ParseSlotHandle(operand.name, &node_id)) return nullptr;
            const auto item = nodes.find(node_id);
            return item == nodes.end() ? nullptr : &item->second;
          }();
          if (found == nullptr || found->binding_present ||
              !TypedPayload(operand, &payload) ||
              !SplitFields(payload, &fields) ||
              !DecodeHex(fields[0], &found->semantic_variant) ||
              found->semantic_variant.empty() ||
              !ParseHandleList(fields[1], &found->bound_expression_ids) ||
              !ParseStringList(fields[2], &found->required_object_uuids) ||
              !ParseStringList(fields[3], &ignored) ||
              !ParseStringList(fields[4], &ignored)) {
            return Invalid("node_binding_invalid", diagnostic);
          }
          found->binding_present = true;
          continue;
        }
        if (operand.type == "relational_descriptor_v1") {
          std::uint32_t handle = 0;
          std::string_view payload;
          std::array<std::string_view, 8> fields{};
          if (!ParseSlotHandle(operand.name, &handle) ||
              !TypedPayload(operand, &payload) ||
              !SplitFields(payload, &fields) ||
              !descriptor_handles.insert(handle).second) {
            return Invalid("descriptor_v1_invalid", diagnostic);
          }
          continue;
        }
        if (operand.type == "relational_descriptor_v2") {
          CanonicalDescriptorRecord descriptor;
          if (!ParseDescriptorV2(operand, &descriptor) ||
              !descriptor_handles.insert(descriptor.handle).second ||
              !descriptors.emplace(descriptor.handle, descriptor).second) {
            return Invalid("descriptor_v2_invalid", diagnostic);
          }
          continue;
        }
        if (operand.type == "relational_expression_v1") {
          if (operand.value_kind ==
              sblr::SblrValueKind::expression_node_ref) {
            CanonicalLiteralReferenceRecord record;
            if (!ParseDirectHandle(operand.name, &record.expression_id) ||
                !sblr::DecodeSblrExpressionNodeReferenceV1(
                    operand.value_body.data(), operand.value_body.size(),
                    &record.reference) ||
                !expression_ids.insert(record.expression_id).second) {
              return Invalid("literal_reference_invalid", diagnostic);
            }
            literal_references.push_back(std::move(record));
            continue;
          }
          if (operand.value_kind == sblr::SblrValueKind::parameter_node_ref ||
              operand.value_kind == sblr::SblrValueKind::variable_node_ref) {
            std::uint32_t expression_id = 0;
            if (!ParseDirectHandle(operand.name, &expression_id) ||
                !expression_ids.insert(expression_id).second) {
              return Invalid("external_expression_reference_invalid",
                             diagnostic);
            }
            continue;
          }
          CanonicalExpressionRecord expression;
          std::string_view payload;
          std::array<std::string_view, 8> fields{};
          std::uint64_t value = 0;
          if (!ParseSlotHandle(operand.name, &expression.expression_id) ||
              !TypedPayload(operand, &payload) ||
              !SplitFields(payload, &fields) ||
              !ParseUnsigned(fields[0],
                             std::numeric_limits<std::uint8_t>::max(),
                             &value) ||
              value == 0) {
            return Invalid("expression_record_invalid", diagnostic);
          }
          expression.kind = static_cast<std::uint8_t>(value);
          if (!ParseHandleList(fields[1], &expression.children) ||
              !ParseUnsigned(fields[2],
                             std::numeric_limits<std::uint32_t>::max(),
                             &value) ||
              value == 0) {
            return Invalid("expression_handles_invalid", diagnostic);
          }
          expression.result_descriptor_handle =
              static_cast<std::uint32_t>(value);
          if (fields[3] != "-") expression.function_uuid = fields[3];
          if (fields[4] != "-") expression.bound_name_uuid = fields[4];
          if (fields[5] != "-") {
            if (!ParseUnsigned(fields[5],
                               std::numeric_limits<std::uint8_t>::max(),
                               &value) ||
                value == 0) {
              return Invalid("expression_literal_kind_invalid", diagnostic);
            }
            expression.literal_kind = static_cast<std::uint8_t>(value);
          }
          if (!DecodeOptionalHex(fields[6], &expression.operator_name) ||
              !DecodeOptionalHex(fields[7],
                                 &expression.literal_or_parameter_ref) ||
              !expression_ids.insert(expression.expression_id).second ||
              !expressions.emplace(expression.expression_id,
                                   std::move(expression)).second) {
            return Invalid("expression_identity_invalid", diagnostic);
          }
          continue;
        }
        if (operand.type == "relational_output_v1") {
          CanonicalOutputRecord output;
          std::string_view payload;
          std::array<std::string_view, 6> fields{};
          std::uint64_t value = 0;
          std::string ignored;
          if (!ParseSlotHandle(operand.name, &output.output_id) ||
              !TypedPayload(operand, &payload) ||
              !SplitFields(payload, &fields) ||
              !ParseUnsigned(fields[0],
                             std::numeric_limits<std::uint32_t>::max(),
                             &value) ||
              value == 0) {
            return Invalid("output_record_invalid", diagnostic);
          }
          output.node_id = static_cast<std::uint32_t>(value);
          if (!ParseUnsigned(fields[1],
                             std::numeric_limits<std::uint32_t>::max(),
                             &value) ||
              value == 0) {
            return Invalid("output_expression_invalid", diagnostic);
          }
          output.expression_id = static_cast<std::uint32_t>(value);
          if (!ParseUnsigned(fields[2],
                             std::numeric_limits<std::uint32_t>::max(),
                             &value) ||
              value == 0 || (fields[3] != "0" && fields[3] != "1")) {
            return Invalid("output_descriptor_invalid", diagnostic);
          }
          output.descriptor_handle = static_cast<std::uint32_t>(value);
          output.visible = fields[3] == "1";
          if (!ParseUnsigned(fields[4],
                             std::numeric_limits<std::uint32_t>::max(),
                             &value) ||
              !DecodeHex(fields[5], &ignored)) {
            return Invalid("output_ordinal_invalid", diagnostic);
          }
          output.ordinal = static_cast<std::uint32_t>(value);
          outputs.push_back(std::move(output));
        }
      }
      if (sbxn_table_count != 1) {
        return Invalid("sbxn_table_count_invalid", diagnostic);
      }

      std::vector<sblr::SblrExpressionNodeReferenceV1> references;
      references.reserve(literal_references.size());
      for (const auto& reference : literal_references) {
        references.push_back(reference.reference);
      }
      if (!sblr::ValidateSblrLiteralReferenceBijectionV1(decoded_sbxn,
                                                         references)) {
        return Invalid("sbxn_reference_bijection_invalid", diagnostic);
      }

      std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> incoming;
      for (const auto& [expression_id, expression] : expressions) {
        for (const auto child : expression.children) {
          if (!expression_ids.contains(child)) {
            return Invalid("expression_child_invalid", diagnostic);
          }
          incoming[child].push_back(expression_id);
        }
      }

      EngineContextualTextCanonicalGraphSnapshotV2 graph;
      graph.exact_pre_contextual_operand_records =
          exact_pre_contextual_operand_records;
      graph.pre_contextual_operand_count = pre_contextual_operand_count;
      graph.exact_sbxn = exact_sbxn;
      graph.one_global_sbxn_table = true;
      graph.total_sbxn_node_count =
          static_cast<std::uint32_t>(decoded_sbxn.table.nodes.size());

      for (const auto& [node_id, node] : nodes) {
        if (!node.binding_present || node.node_kind != 1 ||
            (node.semantic_variant != "relation.source.v1" &&
             node.semantic_variant != "SBLR_MODEL_SOURCE_V1")) {
          continue;
        }
        Uuid relation{};
        if (node.required_object_uuids.size() != 1 ||
            !ParseUuidText(node.required_object_uuids.front(), &relation)) {
          return Invalid("source_relation_invalid", diagnostic);
        }
        EngineContextualTextGraphSourceV2 source;
        source.node_id = node_id;
        source.top_level_operand_ordinal = node.top_level_operand_ordinal;
        source.node_kind_is_scan = true;
        source.semantic_variant_is_catalog_or_model_source = true;
        source.required_relation_uuid = relation;
        graph.sources.push_back(std::move(source));
      }
      std::ranges::sort(graph.sources, {},
                        &EngineContextualTextGraphSourceV2::
                            top_level_operand_ordinal);
      std::unordered_map<std::uint32_t, std::size_t> source_indexes;
      for (std::size_t index = 0; index != graph.sources.size(); ++index) {
        graph.sources[index].source_ordinal =
            static_cast<std::uint32_t>(index);
        if (!source_indexes.emplace(graph.sources[index].node_id, index)
                 .second) {
          return Invalid("source_identity_duplicate", diagnostic);
        }
      }

      Uuid contextual_descriptor{};
      if (!ParseUuidText("019d0000-0000-7000-8000-00000000d718",
                         &contextual_descriptor)) {
        return Invalid("contextual_descriptor_identity_invalid", diagnostic);
      }
      struct ContextualNode {
        const sblr::SblrExpressionLiteralNodeV1* node = nullptr;
        const CanonicalLiteralReferenceRecord* reference = nullptr;
      };
      std::vector<ContextualNode> contextual_nodes;
      for (const auto& node : decoded_sbxn.table.nodes) {
        const auto reference = std::ranges::find_if(
            literal_references, [&](const auto& candidate) {
              return candidate.reference.node_id == node.node_id;
            });
        if (reference == literal_references.end()) {
          return Invalid("sbxn_reference_missing", diagnostic);
        }
        if (node.descriptor_uuid == contextual_descriptor) {
          if (node.descriptor_generation != 1) {
            return Invalid("contextual_descriptor_generation_invalid",
                           diagnostic);
          }
          contextual_nodes.push_back({&node, &*reference});
        } else {
          ++graph.numeric_v1_sbxn_node_count;
        }
      }
      std::ranges::sort(contextual_nodes, {}, [](const ContextualNode& item) {
        return item.reference->reference.occurrence_ordinal;
      });
      if (contextual_nodes.size() != execute.mappings.size()) {
        return Invalid("contextual_partition_invalid", diagnostic);
      }

      const auto policies =
          LookupEngineContextualTextPolicyRowSetV2(exact_live_context);
      if (!policies.ok) {
        if (diagnostic != nullptr) *diagnostic = policies.diagnostic;
        return false;
      }
      for (std::size_t index = 0; index != contextual_nodes.size(); ++index) {
        const auto& item = contextual_nodes[index];
        const auto& mapping = execute.mappings[index];
        const auto& profile = mapping.profile;
        if (item.reference->reference.occurrence_ordinal !=
                mapping.literal_occurrence ||
            item.node->parent_operand_ordinal != mapping.literal_occurrence ||
            item.reference->reference.node_id != mapping.node_id ||
            item.node->node_id != mapping.node_id ||
            item.reference->reference.descriptor_uuid !=
                profile.descriptor_uuid ||
            item.reference->reference.descriptor_generation !=
                profile.descriptor_generation ||
            item.node->literal_body != profile.canonical_body) {
          return Invalid("contextual_mapping_bijection_invalid", diagnostic);
        }

        const auto parents = incoming.find(item.reference->expression_id);
        if (parents == incoming.end() || parents->second.size() != 1) {
          return TargetMismatch("literal_incoming_edge_invalid", diagnostic);
        }
        const auto comparison = expressions.find(parents->second.front());
        if (comparison == expressions.end() || comparison->second.kind != 6 ||
            comparison->second.children.size() != 2 ||
            comparison->second.operator_name != "=" ||
            comparison->second.function_uuid.has_value() ||
            comparison->second.bound_name_uuid.has_value() ||
            comparison->second.literal_kind.has_value() ||
            comparison->second.literal_or_parameter_ref.has_value() ||
            comparison->first != profile.comparison_occurrence ||
            profile.literal_argument_ordinal < 1 ||
            profile.literal_argument_ordinal > 2 ||
            profile.target_argument_ordinal < 1 ||
            profile.target_argument_ordinal > 2 ||
            profile.literal_argument_ordinal ==
                profile.target_argument_ordinal ||
            comparison->second.children[profile.literal_argument_ordinal - 1] !=
                item.reference->expression_id) {
          return TargetMismatch("comparison_expression_invalid", diagnostic);
        }
        const auto target_id =
            comparison->second.children[profile.target_argument_ordinal - 1];
        const auto target = expressions.find(target_id);
        Uuid column{};
        if (target == expressions.end() || target->second.kind != 3 ||
            !target->second.children.empty() ||
            target->second.function_uuid.has_value() ||
            !target->second.bound_name_uuid.has_value() ||
            target->second.literal_kind.has_value() ||
            target->second.operator_name.has_value() ||
            target->second.literal_or_parameter_ref.has_value() ||
            !ParseUuidText(*target->second.bound_name_uuid, &column)) {
          return TargetMismatch("target_expression_invalid", diagnostic);
        }

        const std::array<std::uint32_t, 3> source_owned_expression_ids{
            target_id, comparison->first, item.reference->expression_id};
        std::array<std::optional<std::uint32_t>, 3> source_owners;
        for (const auto& source : graph.sources) {
          const auto source_node = nodes.find(source.node_id);
          if (source_node == nodes.end()) {
            return Invalid("source_node_missing", diagnostic);
          }
          for (std::size_t role = 0;
               role != source_owned_expression_ids.size(); ++role) {
            const auto count = std::ranges::count(
                source_node->second.bound_expression_ids,
                source_owned_expression_ids[role]);
            if (count > 1 || (count == 1 && source_owners[role].has_value())) {
              return TargetMismatch("source_expression_ownership_duplicate",
                                    diagnostic);
            }
            if (count == 1) source_owners[role] = source.node_id;
          }
        }
        if (std::ranges::any_of(source_owners,
                                [](const auto& owner) {
                                  return !owner.has_value();
                                }) ||
            source_owners[0] != source_owners[1] ||
            source_owners[0] != source_owners[2]) {
          return TargetMismatch("source_expression_ownership_invalid",
                                diagnostic);
        }
        const auto source_index = source_indexes.find(*source_owners[0]);
        if (source_index == source_indexes.end()) {
          return TargetMismatch("target_source_invalid", diagnostic);
        }

        const auto literal_descriptor =
            descriptors.find(mapping.literal_descriptor_handle);
        const auto target_descriptor =
            descriptors.find(mapping.target_descriptor_handle);
        std::uint64_t target_nullable = 0;
        const std::string expected_width =
            profile.target_character_limit ==
                    std::numeric_limits<std::uint64_t>::max()
                ? "-"
                : std::to_string(profile.target_character_limit);
        if (literal_descriptor == descriptors.end() ||
            target_descriptor == descriptors.end() ||
            mapping.literal_descriptor_handle ==
                mapping.target_descriptor_handle ||
            !DescriptorIsLiveText(exact_live_context,
                                  literal_descriptor->second) ||
            !DescriptorIsLiveText(exact_live_context,
                                  target_descriptor->second) ||
            target->second.result_descriptor_handle !=
                mapping.target_descriptor_handle ||
            target_descriptor->second.fields[8] !=
                UuidText(profile.collation_uuid) ||
            target_descriptor->second.fields[9] != "-" ||
            target_descriptor->second.fields[10] != expected_width ||
            target_descriptor->second.fields[11] != "-" ||
            target_descriptor->second.fields[12] != "-" ||
            !ParseUnsigned(target_descriptor->second.fields[7], 1,
                           &target_nullable)) {
          return DescriptorInvalid("descriptor_binding_invalid", diagnostic);
        }

        EngineContextualTextGraphOccurrenceV2 occurrence;
        occurrence.literal_occurrence = mapping.literal_occurrence;
        occurrence.node_id = mapping.node_id;
        occurrence.literal_binding_uuid = mapping.literal_binding_uuid;
        occurrence.literal_binding_generation =
            mapping.literal_binding_generation;
        occurrence.source = graph.sources[source_index->second];
        occurrence.comparison_expression_id = comparison->first;
        occurrence.comparison_kind_is_binary = true;
        occurrence.comparison_child_expression_ids = {
            comparison->second.children[0], comparison->second.children[1]};
        occurrence.comparison_child_count = 2;
        occurrence.canonical_operator_name = "=";
        occurrence.resolved_equality_operation_uuid =
            policies.rows.equality.identity_uuid;
        occurrence.resolved_equality_operation_generation =
            policies.rows.equality.generation;
        occurrence.target_expression_id = target_id;
        occurrence.target_is_simple_bound_column = true;
        occurrence.target_source_node_id = source_owners[0].value();
        occurrence.target_relation_uuid =
            occurrence.source.required_relation_uuid;
        occurrence.target_column_uuid = column;
        occurrence.target_column_ordinal = profile.column_ordinal;
        occurrence.target_descriptor_handle =
            target->second.result_descriptor_handle;
        occurrence.target_descriptor.descriptor_handle =
            target->second.result_descriptor_handle;
        occurrence.target_descriptor.exact_relational_descriptor_v2_fields =
            target_descriptor->second.fields;
        occurrence.target_descriptor.canonical_type_name = "text";
        occurrence.target_descriptor.element_profile_empty = true;
        occurrence.literal_expression_id = item.reference->expression_id;
        occurrence.literal_reference = item.reference->reference;
        occurrence.literal_node = *item.node;
        occurrence.literal_incoming_use_count = 1;
        occurrence.literal_sole_parent_expression_id = comparison->first;
        occurrence.literal_descriptor_handle =
            mapping.literal_descriptor_handle;
        occurrence.literal_descriptor.descriptor_handle =
            mapping.literal_descriptor_handle;
        occurrence.literal_descriptor.exact_relational_descriptor_v2_fields =
            literal_descriptor->second.fields;
        occurrence.literal_descriptor.canonical_type_name = "text";
        occurrence.literal_descriptor.element_profile_empty = true;
        if (!ExactDescriptor(occurrence.literal_descriptor, profile)) {
          return DescriptorInvalid("literal_descriptor_invalid", diagnostic);
        }
        if (!ExactTargetDescriptor(occurrence.target_descriptor, profile)) {
          return DescriptorInvalid("target_descriptor_invalid", diagnostic);
        }
        graph.contextual_occurrences.push_back(std::move(occurrence));
      }
      graph.every_sbxn_node_classified_once =
          graph.numeric_v1_sbxn_node_count +
                  graph.contextual_occurrences.size() ==
              graph.total_sbxn_node_count;
      if (!graph.every_sbxn_node_classified_once) {
        return Invalid("sbxn_partition_incomplete", diagnostic);
      }
      *out = std::move(graph);
      if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
      return true;
    } catch (const std::bad_alloc&) {
      return Fail("ENGINE.RESOURCE.EXHAUSTED",
                  "engine.contextual_text_graph.allocation_failed",
                  diagnostic);
    }
  }

 private:
  static bool Fail(std::string code, std::string key,
                   EngineApiDiagnostic* diagnostic) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(std::move(code), std::move(key));
    }
    return false;
  }

  static bool Invalid(std::string suffix, EngineApiDiagnostic* diagnostic) {
    return Fail("SBLR.OPERAND_INVALID",
                "engine.contextual_text_graph." + std::move(suffix),
                diagnostic);
  }

  static bool TargetMismatch(std::string suffix,
                             EngineApiDiagnostic* diagnostic) {
    return Fail("SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH",
                "engine.contextual_text_graph." + std::move(suffix),
                diagnostic);
  }

  static bool DescriptorInvalid(std::string suffix,
                                EngineApiDiagnostic* diagnostic) {
    return Fail("CTB.TEXT.DESCRIPTOR_INVALID",
                "engine.contextual_text_graph." + std::move(suffix),
                diagnostic);
  }
};

class Verifier final : public EngineContextualTextGraphAuthorityVerifierV2 {
 public:
  Verifier(
      EngineRequestContext pinned_context,
      std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorV2>
          selector)
      : pinned_context_(std::move(pinned_context)),
        selector_(std::move(selector)) {}

  bool VerifyPrepareEvidence(
      const EngineRequestContext& context,
      const sblr::ContextualTextLiteralNegotiationRequestV2& issued_request,
      const sblr::ContextualTextLiteralExecuteV2& execute,
      const std::vector<std::uint8_t>& exact_sbel_v1,
      const std::vector<std::uint8_t>& exact_canonical_sbos,
      const EngineContextualTextComposedTransferRecordV2& composed_transfer,
      const std::vector<std::uint8_t>& exact_pre_contextual_operand_records,
      std::uint32_t pre_contextual_operand_count,
      const std::vector<std::uint8_t>& exact_sbxn,
      std::vector<EngineContextualTextVerifiedGraphBindingV2>*
          verified_bindings,
      EngineApiDiagnostic* diagnostic) const override {
    if (verified_bindings == nullptr) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_graph.binding_output_missing");
      }
      return false;
    }
    verified_bindings->clear();
    if (!SameContext(pinned_context_, context)) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "ENGINE.STATEMENT_CONTEXT.RECEIPT_MISMATCH",
            "engine.contextual_text_graph.context_mismatch");
      }
      return false;
    }
    if (selector_ == nullptr) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_graph.selector_unavailable",
            "no engine-owned canonical query graph selector is installed for "
            "this receipt");
      }
      return false;
    }
    if (execute.mappings.empty() ||
        issued_request.demands.size() != execute.mappings.size() ||
        exact_sbel_v1.empty() ||
        exact_canonical_sbos.empty() ||
        composed_transfer.exact_evidence_material.empty() ||
        !Nonzero(composed_transfer.final_receipt_uuid) ||
        !Nonzero(composed_transfer.admission_token_uuid) ||
        !Nonzero(composed_transfer.preliminary_receipt_uuid) ||
        !Nonzero(composed_transfer.profile_set_uuid) ||
        composed_transfer.profile_set_generation == 0 ||
        !Nonzero(composed_transfer.evidence_sha256) ||
        scratchbird::core::hash::ComputeSha256Digest(
            composed_transfer.exact_evidence_material).digest !=
            composed_transfer.evidence_sha256 ||
        composed_transfer.preliminary_receipt_uuid !=
            issued_request.statement_receipt_uuid ||
        composed_transfer.profile_set_uuid != execute.profile_set_uuid ||
        composed_transfer.profile_set_generation !=
            execute.profile_set_generation ||
        exact_pre_contextual_operand_records.empty() ||
        pre_contextual_operand_count == 0 || exact_sbxn.empty() ||
        sblr::ComputeContextualTextPreContextualOperandVectorSha256V2(
            exact_pre_contextual_operand_records,
            pre_contextual_operand_count) !=
            execute.pre_contextual_operand_vector_sha256 ||
        sblr::ComputeContextualTextSbxnSha256V2(exact_sbxn) !=
            execute.sbxn_sha256) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_graph.hash_or_extent_invalid");
      }
      return false;
    }

    EngineContextualTextCanonicalGraphSnapshotV2 graph;
    EngineApiDiagnostic selector_diagnostic;
    if (!selector_->SelectCanonicalGraph(
            context, exact_sbel_v1, exact_canonical_sbos,
            composed_transfer, execute, exact_pre_contextual_operand_records,
            pre_contextual_operand_count, exact_sbxn, &graph,
            &selector_diagnostic)) {
      if (diagnostic != nullptr) *diagnostic = std::move(selector_diagnostic);
      return false;
    }
    if (graph.exact_pre_contextual_operand_records !=
            exact_pre_contextual_operand_records ||
        graph.pre_contextual_operand_count != pre_contextual_operand_count ||
        graph.exact_sbxn != exact_sbxn || !graph.one_global_sbxn_table ||
        !graph.every_sbxn_node_classified_once ||
        graph.contextual_occurrences.size() != execute.mappings.size() ||
        graph.total_sbxn_node_count == 0 ||
        graph.numeric_v1_sbxn_node_count > graph.total_sbxn_node_count ||
        graph.contextual_occurrences.size() >
            graph.total_sbxn_node_count ||
        graph.numeric_v1_sbxn_node_count +
                graph.contextual_occurrences.size() !=
            graph.total_sbxn_node_count ||
        !SourceInventoryCanonical(graph.sources)) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_graph.inventory_invalid");
      }
      return false;
    }

    const auto decoded_sbxn = DecodeContextualTextComposedSbxnV2(
        exact_sbxn.data(), exact_sbxn.size());
    if (!decoded_sbxn.ok || decoded_sbxn.canonical_bytes != exact_sbxn ||
        decoded_sbxn.table.nodes.size() != graph.total_sbxn_node_count) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "engine.contextual_text_graph.sbxn_invalid",
            decoded_sbxn.detail);
      }
      return false;
    }

    std::vector<EngineContextualTextVerifiedGraphBindingV2>
        resolved_bindings;
    try {
      resolved_bindings.reserve(execute.mappings.size());
    } catch (const std::bad_alloc&) {
      if (diagnostic != nullptr) {
        *diagnostic = Diagnostic(
            "ENGINE.RESOURCE.EXHAUSTED",
            "engine.contextual_text_graph.binding_allocation_failed");
      }
      return false;
    }
    std::set<std::uint64_t> node_ids;
    std::set<std::uint64_t> occurrences;
    std::set<std::uint32_t> literal_handles;
    std::set<std::uint32_t> comparison_ids;
    for (std::size_t index = 0; index != execute.mappings.size(); ++index) {
      const auto& mapping = execute.mappings[index];
      const auto& profile = mapping.profile;
      const auto& demand = issued_request.demands[index];
      const auto& occurrence = graph.contextual_occurrences[index];
      if (demand.literal_occurrence != profile.literal_occurrence ||
          demand.node_id != profile.node_id ||
          demand.comparison_occurrence != profile.comparison_occurrence ||
          demand.literal_argument_ordinal !=
              profile.literal_argument_ordinal ||
          demand.target_argument_ordinal != profile.target_argument_ordinal ||
          demand.relation_uuid != profile.relation_uuid ||
          demand.relation_descriptor_uuid !=
              profile.relation_descriptor_uuid ||
          demand.relation_descriptor_generation !=
              profile.relation_descriptor_generation ||
          demand.column_uuid != profile.column_uuid ||
          demand.column_ordinal != profile.column_ordinal ||
          demand.parent_operand_ordinal != profile.parent_operand_ordinal ||
          demand.target_descriptor_handle !=
              profile.target_descriptor_handle ||
          demand.literal_descriptor_handle !=
              profile.literal_descriptor_handle ||
          mapping.literal_occurrence != profile.literal_occurrence ||
          mapping.node_id != profile.node_id ||
          mapping.literal_binding_uuid != profile.literal_binding_uuid ||
          mapping.literal_binding_generation !=
              profile.literal_binding_generation ||
          mapping.literal_descriptor_handle !=
              profile.literal_descriptor_handle ||
          mapping.target_descriptor_handle !=
              profile.target_descriptor_handle ||
          occurrence.literal_occurrence != profile.literal_occurrence ||
          occurrence.node_id != profile.node_id ||
          occurrence.literal_binding_uuid != profile.literal_binding_uuid ||
          occurrence.literal_binding_generation !=
              profile.literal_binding_generation ||
          profile.literal_occurrence >
              std::numeric_limits<std::uint32_t>::max() ||
          profile.comparison_occurrence >
              std::numeric_limits<std::uint32_t>::max() ||
          profile.node_id > std::numeric_limits<std::uint32_t>::max() ||
          profile.literal_occurrence != profile.parent_operand_ordinal ||
          !((profile.literal_argument_ordinal == 1 &&
             profile.target_argument_ordinal == 2) ||
            (profile.literal_argument_ordinal == 2 &&
             profile.target_argument_ordinal == 1)) ||
          !node_ids.insert(profile.node_id).second ||
          !occurrences.insert(profile.literal_occurrence).second ||
          !literal_handles.insert(profile.literal_descriptor_handle).second ||
          !comparison_ids
               .insert(static_cast<std::uint32_t>(
                   profile.comparison_occurrence))
               .second) {
        return Refuse("engine.contextual_text_graph.profile_bijection_invalid",
                      diagnostic);
      }

      const auto source = std::ranges::find_if(
          graph.sources, [&](const EngineContextualTextGraphSourceV2& item) {
            return item.node_id == occurrence.source.node_id;
          });
      if (source == graph.sources.end() ||
          !SameSource(*source, occurrence.source) ||
          occurrence.source.node_id != demand.source_node_id ||
          occurrence.source.top_level_operand_ordinal !=
              demand.source_operand_ordinal ||
          occurrence.source.source_ordinal != demand.source_ordinal ||
          occurrence.source.required_relation_uuid != profile.relation_uuid) {
        return Refuse("engine.contextual_text_graph.source_invalid",
                      diagnostic);
      }
      // Source occurrence UUIDs are provider-issued opaque identities; the
      // graph proves only the retained structural source triple.
      const auto& demand_source = occurrence.source;
      if (demand_source.node_id == 0 ||
          demand_source.top_level_operand_ordinal == 0 ||
          demand_source.source_ordinal >= graph.sources.size()) {
        return Refuse("engine.contextual_text_graph.source_mapping_invalid",
                      diagnostic);
      }

      if (occurrence.comparison_expression_id !=
              profile.comparison_occurrence ||
          !occurrence.comparison_kind_is_binary ||
          occurrence.comparison_child_count != 2 ||
          occurrence.canonical_operator_name != "=" ||
          occurrence.function_uuid_present || occurrence.bound_name_uuid_present ||
          occurrence.collation_override_present ||
          occurrence.resolved_equality_operation_uuid !=
              profile.equality_operation_uuid ||
          occurrence.resolved_equality_operation_generation !=
              profile.equality_operation_generation ||
          !Nonzero(occurrence.resolved_equality_operation_uuid) ||
          occurrence.resolved_equality_operation_generation == 0 ||
          !occurrence.target_is_simple_bound_column ||
          occurrence.target_source_node_id != occurrence.source.node_id ||
          occurrence.target_relation_uuid != profile.relation_uuid ||
          occurrence.target_column_uuid != profile.column_uuid ||
          occurrence.target_column_ordinal != profile.column_ordinal ||
          occurrence.target_descriptor_handle !=
              profile.target_descriptor_handle ||
          !ExactTargetDescriptor(occurrence.target_descriptor, profile) ||
          occurrence.literal_expression_id == 0 ||
          occurrence.literal_descriptor_handle !=
              profile.literal_descriptor_handle ||
          occurrence.literal_incoming_use_count != 1 ||
          occurrence.literal_sole_parent_expression_id !=
              occurrence.comparison_expression_id) {
        return Refuse("engine.contextual_text_graph.equality_invalid",
                      diagnostic);
      }

      const auto literal_child =
          occurrence.comparison_child_expression_ids
              [profile.literal_argument_ordinal - 1];
      const auto target_child =
          occurrence.comparison_child_expression_ids
              [profile.target_argument_ordinal - 1];
      if (literal_child != occurrence.literal_expression_id ||
          target_child != occurrence.target_expression_id) {
        return Refuse("engine.contextual_text_graph.child_order_invalid",
                      diagnostic);
      }

      const auto sbxn_node = std::ranges::find_if(
          decoded_sbxn.table.nodes,
          [&](const sblr::SblrExpressionLiteralNodeV1& node) {
            return node.node_id == profile.node_id;
          });
      if (sbxn_node == decoded_sbxn.table.nodes.end() ||
          !SameLiteralNode(*sbxn_node, occurrence.literal_node) ||
          occurrence.literal_node.parent_node_id != 0 ||
          occurrence.literal_node.parent_operand_ordinal !=
              profile.parent_operand_ordinal ||
          occurrence.literal_node.descriptor_uuid != profile.descriptor_uuid ||
          occurrence.literal_node.descriptor_generation !=
              profile.descriptor_generation ||
          occurrence.literal_node.literal_body != profile.canonical_body ||
          occurrence.literal_reference.occurrence_ordinal !=
              profile.literal_occurrence ||
          occurrence.literal_reference.node_id != profile.node_id ||
          occurrence.literal_reference.node_table_sha256 !=
              execute.sbxn_sha256 ||
          occurrence.literal_reference.descriptor_uuid !=
              profile.descriptor_uuid ||
          occurrence.literal_reference.descriptor_generation !=
              profile.descriptor_generation ||
          !ExactDescriptor(occurrence.literal_descriptor, profile)) {
        return Refuse("engine.contextual_text_graph.literal_invalid",
                      diagnostic);
      }

      EngineContextualTextVerifiedGraphBindingV2 binding;
      binding.literal_occurrence = profile.literal_occurrence;
      binding.node_id = profile.node_id;
      binding.literal_expression_id = occurrence.literal_expression_id;
      binding.comparison_expression_id =
          occurrence.comparison_expression_id;
      binding.target_expression_id = occurrence.target_expression_id;
      binding.source_node_id = occurrence.source.node_id;
      binding.literal_descriptor_handle =
          occurrence.literal_descriptor_handle;
      binding.target_descriptor_handle = occurrence.target_descriptor_handle;
      binding.exact_relational_descriptor_v2_fields =
          occurrence.literal_descriptor.exact_relational_descriptor_v2_fields;
      binding.canonical_type_name =
          occurrence.literal_descriptor.canonical_type_name;
      binding.element_profile_empty =
          occurrence.literal_descriptor.element_profile_empty;
      binding.exact_target_relational_descriptor_v2_fields =
          occurrence.target_descriptor.exact_relational_descriptor_v2_fields;
      binding.target_canonical_type_name =
          occurrence.target_descriptor.canonical_type_name;
      binding.target_element_profile_empty =
          occurrence.target_descriptor.element_profile_empty;
      resolved_bindings.push_back(std::move(binding));
    }
    *verified_bindings = std::move(resolved_bindings);
    if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
    return true;
  }

 private:
  static bool Refuse(std::string key, EngineApiDiagnostic* diagnostic) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic("SBLR.OPERAND_INVALID", std::move(key));
    }
    return false;
  }

  EngineRequestContext pinned_context_;
  std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorV2> selector_;
};

}  // namespace

bool InstallEngineContextualTextCanonicalGraphSelectorProviderV2(
    std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorProviderV2>
        provider,
    EngineApiDiagnostic* diagnostic) {
  if (provider == nullptr) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "engine.contextual_text_graph.provider_missing");
    }
    return false;
  }
  std::lock_guard<std::mutex> guard(g_selector_provider_mutex);
  if (g_selector_provider != nullptr &&
      g_selector_provider.get() != provider.get()) {
    if (diagnostic != nullptr) {
      *diagnostic = Diagnostic(
          "ENGINE.INTERNAL.CONFLICT",
          "engine.contextual_text_graph.provider_replacement_refused");
    }
    return false;
  }
  g_selector_provider = std::move(provider);
  if (diagnostic != nullptr) *diagnostic = OkDiagnostic();
  return true;
}

std::unique_ptr<EngineContextualTextGraphAuthorityVerifierV2>
CreateEngineContextualTextGraphAuthorityVerifierForReceiptV2(
    const EngineRequestContext& exact_live_context) {
  try {
    std::shared_ptr<
        const EngineContextualTextCanonicalGraphSelectorProviderV2>
        provider;
    {
      std::lock_guard<std::mutex> guard(g_selector_provider_mutex);
      provider = g_selector_provider;
    }
    std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorV2>
        selector;
    if (provider != nullptr) {
      selector = provider->SelectForReceipt(exact_live_context);
    } else {
      selector = std::make_shared<CanonicalOperandGraphSelector>();
    }
    return std::make_unique<Verifier>(exact_live_context,
                                      std::move(selector));
  } catch (...) {
    return nullptr;
  }
}

}  // namespace scratchbird::engine::internal_api
