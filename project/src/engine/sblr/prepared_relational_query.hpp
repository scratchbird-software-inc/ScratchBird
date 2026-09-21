// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "relational_descriptor_codec.hpp"
#include "relational_identity_codec.hpp"
#include "sblr_literal_runtime.hpp"
#include "sblr_parameter_runtime.hpp"
#include "core/datatypes/canonical_utf8.hpp"
#include "core/uuid/uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <type_traits>

namespace scratchbird::engine::sblr {
enum class PreparedRelationalQueryProfileV1 {
  kLiteralValues, kParameterValues, kParameterMatchRecognize,
};

// Structural recognition only. Live receipt, descriptor, parameter, resource
// and MGA validation and real execution remain responsibilities of the caller.
namespace prepared_relational_detail {
using Uuid = core::platform::Uuid;
using Bytes = std::vector<std::uint8_t>;
inline bool Slot(const SblrOperationEnvelope& op, std::size_t index, std::string_view type,
                 std::string_view name, SblrValueKind kind) {
  if (index >= op.operands.size()) return false;
  const auto& item = op.operands[index];
  return item.ordinal == index + 1 && item.type == type && item.name == name && item.value_kind == kind &&
         item.value_flags == 0 && item.value.empty();
}
inline bool TypedPayload(const SblrOperand& operand, std::string_view* output) {
  if (!output || operand.value_kind != SblrValueKind::literal_typed || operand.value_flags ||
      !operand.value.empty() || operand.value_body.size() < 24) return false;
  Uuid descriptor; std::copy_n(operand.value_body.begin(), 16, descriptor.bytes.begin());
  std::uint64_t count = 0;
  for (unsigned i = 0; i < 8; ++i) count |= std::uint64_t(operand.value_body[16 + i]) << (8 * i);
  if (!core::uuid::IsEngineIdentityUuid(descriptor) || count != operand.value_body.size() - 24 ||
      count > kSblrOperationMaximumScalarBytes ||
      !core::datatypes::ValidateCanonicalUtf8(operand.value_body.data() + 24, count)) return false;
  *output = {reinterpret_cast<const char*>(operand.value_body.data() + 24), static_cast<std::size_t>(count)};
  return true;
}
inline bool Text(const SblrOperationEnvelope& op, std::size_t index, std::string_view type,
                 std::string_view name, std::string_view expected) {
  std::string_view payload;
  return Slot(op, index, type, name, SblrValueKind::literal_typed) &&
         TypedPayload(op.operands[index], &payload) && payload == expected;
}
inline bool UnsignedText(std::string_view value, std::uint64_t* output) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), *output);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
         std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
}
inline bool Header(const SblrOperationEnvelope& op) {
  if (op.operation_id != "query.execute" || op.opcode != "SBLR_QUERY_EXECUTE" || op.opcode_code != 4615 ||
      op.operation_version_major != 1 || op.operation_version_minor != 0 || op.result_shape != "query_execute_result" ||
      op.contains_sql_text || op.operands.size() < 11 || !ValidateSblrEnvelope(op).ok ||
      !Text(op, 0, "uint16", "relational_wire_version", "2")) return false;
  for (std::size_t i = 0; i < kRelationalContextIdentitySlots.size(); ++i) {
    Uuid id;
    if (!Slot(op, i + 1, "uuid", kRelationalContextIdentitySlots[i], SblrValueKind::uuid_ref) ||
        !DecodeRelationalContextIdentity(op.operands[i + 1].type, op.operands[i + 1].name, op.operands[i + 1].value_kind,
            op.operands[i + 1].value_body.data(), op.operands[i + 1].value_body.size(), &id)) return false;
  }
  std::uint64_t local = 0, visible = 0; std::string_view text;
  return Slot(op, 8, "uint64", "relational_local_transaction_id", SblrValueKind::literal_typed) &&
      TypedPayload(op.operands[8], &text) && UnsignedText(text, &local) && local != 0 &&
      Slot(op, 9, "uint64", "relational_snapshot_visible_through_local_transaction_id", SblrValueKind::literal_typed) &&
      TypedPayload(op.operands[9], &text) && UnsignedText(text, &visible);
}
inline bool Descriptors(const SblrOperationEnvelope& op, std::size_t count,
                        std::vector<internal_api::RelationalTypeDescriptor>* output) {
  for (std::size_t i = 0; i < count; ++i) {
    const auto index = 11 + i;
    internal_api::RelationalTypeDescriptor descriptor;
    if (!Slot(op, index, "relational_descriptor_v3", "slot_" + std::to_string(i + 1), SblrValueKind::relational_type_descriptor) ||
        !DecodeRelationalTypeDescriptorV1(op.operands[index].value_body.data(), op.operands[index].value_body.size(), &descriptor) ||
        descriptor.descriptor_id != i + 1) return false;
    output->push_back(std::move(descriptor));
  }
  return true;
}
inline bool Binding(const SblrOperationEnvelope& op, std::size_t index, const RelationalNodeBindingRecord& expected) {
  Bytes bytes;
  return Slot(op, index, "relational_node_binding_v2", "slot_" + std::to_string(expected.node_id), SblrValueKind::relational_node_binding) &&
      EncodeRelationalNodeBindingV1(expected, &bytes) && bytes == op.operands[index].value_body;
}
inline bool Output(const SblrOperationEnvelope& op, std::size_t index, std::uint32_t output_id,
                   std::uint32_t node, std::uint32_t expression, std::uint32_t descriptor, std::uint32_t ordinal,
                   std::optional<std::string_view> exact_name_hex = std::nullopt) {
  std::string_view payload;
  if (!Slot(op, index, "relational_output_v1", "slot_" + std::to_string(output_id), SblrValueKind::literal_typed) ||
      !TypedPayload(op.operands[index], &payload)) return false;
  const auto prefix = std::to_string(node) + "|" + std::to_string(expression) + "|" + std::to_string(descriptor) +
      "|1|" + std::to_string(ordinal) + "|";
  if (!payload.starts_with(prefix)) return false;
  const auto name = payload.substr(prefix.size());
  if (exact_name_hex) return name == *exact_name_hex;
  if (name.empty() || name.size() % 2) return false;
  const auto digit = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
  Bytes decoded;
  decoded.reserve(name.size() / 2);
  for (std::size_t i = 0; i < name.size(); i += 2) {
    const int high = digit(name[i]), low = digit(name[i + 1]);
    if (high < 0 || low < 0) return false;
    decoded.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return core::datatypes::ValidateCanonicalUtf8(decoded.data(), decoded.size());
}
inline bool Parameters(const SblrOperationEnvelope& op, const SblrParameterNodeTableCodecResultV1& table,
                       const std::vector<internal_api::RelationalTypeDescriptor>& descriptors,
                       std::size_t expression_begin, std::size_t descriptor_offset) {
  std::vector<SblrParameterNodeReferenceV1> refs;
  for (std::size_t i = 0; i < table.table.nodes.size(); ++i) {
    const auto& node = table.table.nodes[i];
    if (!Slot(op, expression_begin + i, "relational_expression_v1", std::to_string(i + 1), SblrValueKind::parameter_node_ref) ||
        node.node_id != i + 1 || node.parent_operand_ordinal != i + 1 || node.slot_ordinal != i ||
        i + descriptor_offset >= descriptors.size()) return false;
    const auto& descriptor = descriptors[i + descriptor_offset];
    if (node.datatype_descriptor_uuid != descriptor.descriptor_uuid.bytes ||
        (descriptor.datatype_identity_authoritative && node.datatype_descriptor_generation != descriptor.descriptor_generation)) return false;
    SblrParameterNodeReferenceV1 ref;
    if (!DecodeSblrParameterNodeReferenceV1(op.operands[expression_begin + i].value_body.data(),
            op.operands[expression_begin + i].value_body.size(), &ref) ||
        ref.occurrence_ordinal != i + 1 || ref.node_id != i + 1 || ref.slot_ordinal != i) return false;
    refs.push_back(ref);
  }
  return ValidateSblrParameterReferenceBijectionV1(table, refs);
}
inline bool Property(const SblrOperationEnvelope& op, std::size_t index,
                     internal_api::RelationalPropertyRecord expected, Uuid* identity) {
  if (!Slot(op, index, "relational_property_v3", "property", SblrValueKind::relational_property)) return false;
  internal_api::RelationalPropertyRecord actual; Bytes bytes;
  if (!DecodeRelationalPropertyV1(op.operands[index].value_body.data(), op.operands[index].value_body.size(), &actual)) return false;
  expected.property_uuid = actual.property_uuid;
  if (!EncodeRelationalPropertyV1(expected, &bytes) || bytes != op.operands[index].value_body) return false;
  *identity = actual.property_uuid;
  return true;
}
}  // namespace prepared_relational_detail

inline bool ValidatePreparedRelationalQueryV1(const SblrOperationEnvelope& op, PreparedRelationalQueryProfileV1 profile) {
  namespace d = prepared_relational_detail;
  if (!d::Header(op)) return false;
  if (profile == PreparedRelationalQueryProfileV1::kLiteralValues) {
    RelationalNodeBindingRecord literal_binding;
    literal_binding.node_id = 1;
    literal_binding.semantic_variant_id = "values.literal-table.v1";
    literal_binding.bound_expression_ids = {1};
    if (op.operands.size() != 18 ||
        !d::Text(op, 10, "uint32", "relational_root_node_id", "1") ||
        !d::Slot(op, 12, "relational_expression_v1", "1", SblrValueKind::expression_node_ref) ||
        !d::Output(op, 13, 1, 1, 1, 1, 0) ||
        !d::Text(op, 14, "relational_values_row_v1", "slot_1", "1") ||
        !d::Text(op, 15, "relational_node_v1", "slot_1", "13|0|-|1|1") ||
        !d::Binding(op, 16, literal_binding) ||
        !d::Slot(op, 17, "expression.node_table.v1", "expression_nodes", SblrValueKind::expression_node_table)) return false;
    std::vector<internal_api::RelationalTypeDescriptor> descriptors;
    SblrExpressionNodeReferenceV1 ref;
    if (!d::Descriptors(op, 1, &descriptors) ||
        !DecodeSblrExpressionNodeReferenceV1(op.operands[12].value_body.data(), op.operands[12].value_body.size(), &ref)) return false;
    const auto table = DecodeSblrExpressionNodeTableV1(op.operands[17].value_body.data(), op.operands[17].value_body.size());
    return table.ok && table.table.nodes.size() == 1 && table.table.nodes[0].node_id == 1 &&
        table.table.nodes[0].parent_node_id == 0 && table.table.nodes[0].parent_operand_ordinal == 1 &&
        ref.occurrence_ordinal == 1 && ref.node_id == 1 && ref.descriptor_uuid == descriptors[0].descriptor_uuid.bytes &&
        (!descriptors[0].datatype_identity_authoritative || ref.descriptor_generation == descriptors[0].descriptor_generation) &&
        ValidateSblrLiteralReferenceBijectionV1(table, {ref});
  }
  if (profile != PreparedRelationalQueryProfileV1::kParameterValues &&
      profile != PreparedRelationalQueryProfileV1::kParameterMatchRecognize) return false;
  if (!d::Slot(op, op.operands.size() - 1, "expression.parameter_node_table.v1", "parameter_nodes", SblrValueKind::parameter_node_table))
    return false;
  const auto table = DecodeSblrParameterNodeTableV1(op.operands.back().value_body.data(), op.operands.back().value_body.size());
  if (!table.ok || table.table.nodes.empty() || table.table.nodes.size() > 4096) return false;
  const auto count = table.table.nodes.size();
  std::string handles;
  std::vector<std::uint32_t> ids;
  for (std::size_t i = 0; i < count; ++i) { if (i) handles += ','; handles += std::to_string(i + 1); ids.push_back(i + 1); }
  std::vector<internal_api::RelationalTypeDescriptor> descriptors;
  if (profile == PreparedRelationalQueryProfileV1::kParameterValues) {
    const auto expression_begin = 11 + count, output_begin = expression_begin + count, tail = output_begin + count;
    RelationalNodeBindingRecord values_binding;
    values_binding.node_id = 1;
    values_binding.semantic_variant_id = "values.literal-table.v1";
    values_binding.bound_expression_ids = ids;
    if (op.operands.size() != 15 + 3 * count || !d::Descriptors(op, count, &descriptors) ||
        !d::Parameters(op, table, descriptors, expression_begin, 0) ||
        !d::Text(op, 10, "uint32", "relational_root_node_id", "1") ||
        !d::Text(op, tail, "relational_values_row_v1", "slot_1", handles) ||
        !d::Text(op, tail + 1, "relational_node_v1", "slot_1", "13|0|-|" + handles + "|1") ||
        !d::Binding(op, tail + 2, values_binding)) return false;
    for (std::size_t i = 0; i < count; ++i)
      if (!d::Output(op, output_begin + i, i + 1, 1, i + 1, i + 1, i)) return false;
    return true;
  }
  const auto expression_begin = 12 + count, output_begin = expression_begin + count + 1, tail = output_begin + 2;
  if ((count != 2 && count != 3) || op.operands.size() != 24 + 2 * count || !d::Descriptors(op, count + 1, &descriptors) ||
      !d::Parameters(op, table, descriptors, expression_begin, 1) ||
      !d::Text(op, 10, "uint32", "relational_root_node_id", "2")) return false;
  const std::uint32_t output_expression = count + 1;
  constexpr core::platform::Uuid function{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7e,0x2c,0xb4,0x37,0xeb,0xbb,0xc2,0xd4,0xf3,0x5b}};
  internal_api::RelationalExpressionRecord expected_expression;
  expected_expression.expression_id = output_expression;
  expected_expression.expression_kind = internal_api::RelationalExpressionKind::kFunctionCall;
  expected_expression.result_descriptor_id = 1; expected_expression.child_expression_ids = ids; expected_expression.function_uuid = function;
  RelationalNodeBindingRecord source_binding;
  source_binding.node_id = 1;
  source_binding.semantic_variant_id = "table-function.generate-series.v1";
  source_binding.bound_expression_ids = {output_expression};
  source_binding.required_object_uuids = {function};
  d::Bytes expected_bytes;
  if (!d::Slot(op, expression_begin + count, "relational_expression_v2", "slot_" + std::to_string(output_expression),
               SblrValueKind::relational_expression) || !EncodeRelationalExpressionV1(expected_expression, &expected_bytes) ||
      expected_bytes != op.operands[expression_begin + count].value_body ||
      !d::Output(op, output_begin, 1, 1, output_expression, 1, 0, "67656e65726174655f736572696573") ||
      !d::Output(op, output_begin + 1, 2, 2, output_expression, 1, 0, "67656e65726174655f736572696573") ||
      !d::Text(op, tail, "relational_node_v1", "slot_1", "17|0|-|1|-") ||
      !d::Binding(op, tail + 1, source_binding) ||
      !d::Text(op, tail + 2, "relational_table_function_v1", "slot_1", handles) ||
      !d::Text(op, tail + 3, "relational_node_v1", "slot_2", "16|0|1|1|-")) return false;
  internal_api::RelationalPropertyRecord partition, ordering;
  partition.property_kind = internal_api::RelationalPropertyKind::kPartitioning;
  partition.origin_node_id = 2; partition.expression_ids = {output_expression};
  ordering.origin_node_id = 2;
  ordering.ordering_terms = {{output_expression, internal_api::RelationalPropertySortDirection::kAscending,
                              internal_api::RelationalPropertyNullPlacement::kNullsLast, {}}};
  core::platform::Uuid partition_id, ordering_id;
  if (!d::Property(op, tail + 6, partition, &partition_id) || !d::Property(op, tail + 7, ordering, &ordering_id) ||
      partition_id == ordering_id) return false;
  RelationalNodeBindingRecord match_binding;
  match_binding.node_id = 2;
  match_binding.semantic_variant_id = "match-recognize.a-plus.true.all-rows.v1";
  match_binding.bound_expression_ids = {output_expression};
  match_binding.required_property_uuids = {partition_id, ordering_id};
  match_binding.delivered_property_uuids = {partition_id, ordering_id};
  if (!d::Binding(op, tail + 4, match_binding)) return false;
  internal_api::RelationalRowPatternRecord pattern;
  pattern.pattern_id = 1; pattern.relation_node_id = 2; pattern.partition_expression_ids = {output_expression};
  pattern.ordering_terms = ordering.ordering_terms;
  pattern.variables = {{"a", 1, std::nullopt, false, std::nullopt, true}};
  pattern.rows_per_match = internal_api::RelationalRowPatternRowsPerMatch::kAll;
  pattern.after_match_skip = internal_api::RelationalRowPatternAfterMatchSkip::kPastLastRow;
  pattern.maximum_partition_rows = 10000; pattern.maximum_active_states = 2; pattern.maximum_output_rows = 10000;
  pattern.stable_row_identity_tie_break_allowed = true;
  return d::Slot(op, tail + 5, "relational_row_pattern_v2", "slot_1", SblrValueKind::relational_row_pattern) &&
      EncodeRelationalRowPatternV1(pattern, &expected_bytes) && expected_bytes == op.operands[tail + 5].value_body;
}

struct PreparedRelationalContextV1 {
  core::platform::Uuid catalog_epoch_uuid, security_context_uuid, statement_uuid, transaction_uuid,
      statement_snapshot_uuid, statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0}, snapshot_visible_through_local_transaction_id{0};
};
inline bool RebindPreparedRelationalQueryV1(SblrOperationEnvelope* operation, const PreparedRelationalContextV1& context) {
  if (!operation || !context.local_transaction_id) return false;
  const std::array ids{context.catalog_epoch_uuid, context.security_context_uuid, context.statement_uuid,
                       context.transaction_uuid, context.statement_snapshot_uuid, context.statement_metadata_snapshot_uuid};
  if (std::ranges::any_of(ids, [](const auto& id) { return !core::uuid::IsEngineIdentityUuid(id); })) return false;
  bool recognized = false;
  for (const auto profile : {PreparedRelationalQueryProfileV1::kLiteralValues, PreparedRelationalQueryProfileV1::kParameterValues,
                            PreparedRelationalQueryProfileV1::kParameterMatchRecognize}) {
    if (ValidatePreparedRelationalQueryV1(*operation, profile)) { recognized = true; break; }
  }
  if (!recognized) return false;
  auto staged = *operation;
  for (std::size_t i = 0; i < ids.size(); ++i)
    staged.operands[i + 2].value_body.assign(ids[i].bytes.begin(), ids[i].bytes.end());
  const std::array numbers{context.local_transaction_id, context.snapshot_visible_through_local_transaction_id};
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    auto& body = staged.operands[i + 8].value_body;
    const auto text = std::to_string(numbers[i]);
    body.resize(24);
    const auto size = static_cast<std::uint64_t>(text.size());
    for (unsigned byte = 0; byte < 8; ++byte) body[16 + byte] = static_cast<std::uint8_t>(size >> (8 * byte));
    body.insert(body.end(), text.begin(), text.end());
  }
  static_assert(std::is_nothrow_move_assignable_v<SblrOperationEnvelope>);
  *operation = std::move(staged);
  return true;
}
}  // namespace scratchbird::engine::sblr
