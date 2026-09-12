// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_predicate_binding.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "dml/transactional_relation_store.hpp"
#include "api_diagnostics.hpp"
#include <charconv>
#include <chrono>
#include <limits>
#include <map>

namespace scratchbird::engine::internal_api {
namespace {
namespace dt = scratchbird::core::datatypes;
namespace projection = datatype_operator_projection;
bool Issue(wire::TypedUpdateUuid* out) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto id = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now);
  if (!id.ok()) return false;
  std::copy(id.value.value.bytes.begin(), id.value.value.bytes.end(), out->begin());
  return true;
}
std::string Fold(std::string_view value) {
  std::string result(value);
  for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
bool Nonzero(const wire::TypedUpdateUuid& id) {
  return std::any_of(id.begin(), id.end(), [](auto b) { return b != 0; });
}
bool Output(const dt::DatatypeTypeCodecIdentityRowV1& row,
            wire::TypedUpdatePredicateRecord* node) {
  if (!projection::TypedUuid(row.descriptor_uuid, &node->output_descriptor_uuid) ||
      !projection::TypedUuid(row.type_uuid, &node->output_type_uuid)) return false;
  node->output_descriptor_generation = row.descriptor_generation;
  node->output_type_generation = row.type_generation;
  node->output_codec_id = row.codec_id;
  node->output_codec_version = row.codec_version;
  node->output_codec_generation = row.codec_generation;
  return true;
}
// Persisted descriptor fields are not parser hints. Reject duplicates and
// check every identity/generation/codec field against the live registry.
bool ColumnType(const EngineRequestContext& context,
                const MgaRelationColumnStorageDescriptor& column,
                dt::DatatypeTypeCodecIdentityRowV1* row) {
  if (column.column_uuid.is_nil() || !column.column_generation ||
      column.value_descriptor.descriptor_uuid.is_nil()) return false;
  std::map<std::string, std::string> fields;
  std::string_view remaining(column.value_descriptor.encoded_descriptor);
  while (!remaining.empty()) {
    const auto end = remaining.find(';');
    const auto token = remaining.substr(0, end);
    const auto equal = token.find('=');
    if (equal == std::string_view::npos || equal == 0 ||
        !fields.emplace(std::string(token.substr(0, equal)), std::string(token.substr(equal + 1))).second)
      return false;
    if (end == std::string_view::npos) break;
    remaining.remove_prefix(end + 1);
  }
  const auto number = [&](const char* key, std::uint64_t expected) {
    const auto found = fields.find(key);
    if (found == fields.end()) return false;
    std::uint64_t value = 0;
    const auto& text = found->second;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value == expected;
  };
  const auto descriptor = fields.find("datatype_descriptor_uuid");
  if (descriptor == fields.end()) return false;
  // The initial admitted builtin rows all have generation one. A future
  // generation is a new profile, not permission to infer registry identity.
  if (!number("datatype_descriptor_generation", 1)) return false;
  const auto current = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor->second, 1);
  if (!current.ok) return false;
  *row = current.row;
  const auto type = fields.find("type_uuid");
  const auto codec = fields.find("codec_id");
  return type != fields.end() && type->second == row->type_uuid &&
      codec != fields.end() && codec->second == row->codec_id &&
      number("type_generation", row->type_generation) &&
      number("codec_version", row->codec_version) &&
      number("codec_generation", row->codec_generation) &&
      number("null_encoding", row->null_encoding_code);
}
}  // namespace

EngineDmlDeletePredicateBindingResultV1 BindDmlDeletePredicateV1(
    const EngineRequestContext& context, const EngineDmlDeleteRowsBindingDemandV1& demand,
    const wire::TypedUpdateUuid& descriptor_uuid, std::uint64_t descriptor_generation,
    const wire::TypedUpdateUuid& occurrence_uuid, std::uint64_t occurrence_generation) {
  EngineDmlDeletePredicateBindingResultV1 result;
  const auto refuse = [&](std::string detail, std::string code = "SBLR.OPERAND_INVALID") {
    result.diagnostic = MakeEngineApiDiagnostic(std::move(code),
        "sblr.dml_delete_rows.predicate_binding_refused", std::move(detail), true);
    return result;
  };
  const auto has = [&](std::string_view tag) {
    return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) != context.trace_tags.end();
  };
  if (!has("private_dml_delete_rows_binder") || !context.security_context_present ||
      !context.authorization_context.present || !context.statement_metadata_snapshot_engine_owned)
    return refuse("private_DELETE_binder_required", "SECURITY.ACCESS_DENIED");
  for (const auto tag : {"private_dml_delete_rows_consumer", "private_dml_delete_rows_recovery",
                         "private_dml_update_rows_binder", "private_dml_update_rows_consumer",
                         "private_dml_update_rows_recovery"})
    if (has(tag)) return refuse("mixed_operation_or_phase", "SECURITY.ACCESS_DENIED");
  wire::TypedUpdateUuid target{};
  if (!Nonzero(descriptor_uuid) || !descriptor_generation || !Nonzero(occurrence_uuid) ||
      !occurrence_generation || !demand.structural_occurrence_id ||
      context.statement_receipt_uuid.is_nil() ||
      demand.authenticated_statement_receipt_uuid != context.statement_receipt_uuid ||
      !projection::TypedUuid(demand.target_relation_uuid_hint, &target))
    return refuse("owner_or_demand_identity");
  if (context.read_only_mode || context.cluster_transaction_active || context.route_fence_present ||
      !context.local_transaction_id || !context.datatype_catalog_generation || !context.datatype_registry_generation)
    return refuse("context_write_fence", "MGA.TRANSACTION.STALE");
  if (context.query_cancellation_requested && context.query_cancellation_requested())
    return refuse("cancelled", "PROCESS.CANCELLED");
  if (demand.predicate_kind.empty() && (!demand.predicate_column_spelling.empty() ||
      !demand.predicate_literal_spelling.empty() || !demand.predicate_literal_type_spelling.empty()))
    return refuse("all_rows_has_unbound_fields");
  if (!demand.predicate_kind.empty() && demand.predicate_kind != "column_equals")
    return refuse("predicate_profile", "SBLR.OPERATION_UNSUPPORTED");
  auto loaded = TransactionalRelationStore(context).LoadRelationDescriptor(demand.target_relation_uuid_hint);
  if (!loaded.ok) { result.diagnostic = loaded.diagnostic; return result; }
  result.relation = std::move(loaded.descriptor);
  if (result.relation.relation_uuid != demand.target_relation_uuid_hint ||
      ValidateMgaRelationStorageDescriptor(result.relation).error)
    return refuse("live_relation_descriptor", "DATATYPE.DESCRIPTOR.INVALID");
  const auto boolean = dt::LookupCanonicalBooleanTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation);
  const auto operators = dt::LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();
  if (!boolean.ok || !operators.ok) return refuse("live_registry", "DATATYPE.DESCRIPTOR.INVALID");
  auto& predicate = result.predicate;
  if (!Issue(&predicate.identity.vector_uuid)) return refuse("expression_identity");
  predicate.identity.vector_generation = 1;
  predicate.identity.owner_descriptor_uuid = descriptor_uuid;
  predicate.identity.owner_descriptor_generation = descriptor_generation;
  wire::TypedUpdatePredicateRecord root;
  if (!Issue(&root.node_occurrence_uuid) || !Output(boolean.row, &root))
    return refuse("boolean_identity", "DATATYPE.DESCRIPTOR.INVALID");
  root.node_occurrence_generation = 1;
  if (demand.predicate_kind.empty()) {
    root.node_id = 1;
    root.node_kind = wire::TypedUpdatePredicateNodeKind::canonical_boolean_constant;
    root.value_state = wire::TypedUpdateValueState::value;
    root.canonical_value = {1};
  } else {
    const MgaRelationColumnStorageDescriptor* selected = nullptr;
    const auto folded = Fold(demand.predicate_column_spelling);
    for (const auto& column : result.relation.columns) {
      if (Fold(column.canonical_name_key) != folded) continue;
      if (selected) return refuse("ambiguous_column");
      selected = &column;
    }
    if (!selected) return refuse("column_not_found");
    dt::DatatypeTypeCodecIdentityRowV1 type;
    if (!ColumnType(context, *selected, &type))
      return refuse("persisted_column_type", "DATATYPE.DESCRIPTOR.INVALID");
    const bool int32 = type.codec_id == "datatype.int32.le.v1" && type.canonical_value_bytes == 4;
    const bool int64 = type.codec_id == "datatype.int64.le.v1" && type.canonical_value_bytes == 8;
    if (!int32 && !int64) return refuse("fixed_width_integer_equality_required", "SBLR.OPERATION_UNSUPPORTED");
    std::int64_t value = 0;
    const auto& text = demand.predicate_literal_spelling;
    if (text.empty() || text.size() > 20 || demand.predicate_literal_type_spelling == "null")
      return refuse("integer_literal");
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        (int32 && (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())))
      return refuse("integer_literal_range", "DATATYPE.DESCRIPTOR.INVALID");
    const auto equality = dt::LookupBuiltinOperatorTypeCodecIdentityV1(
        operators.snapshot_uuid, operators.registry_generation, operators.equality_operator_uuid,
        operators.equality_operator_generation, type.descriptor_uuid, type.descriptor_generation,
        type.type_uuid, type.type_generation, type.descriptor_uuid, type.descriptor_generation,
        type.type_uuid, type.type_generation);
    if (!equality.ok || equality.row.result_descriptor_uuid != boolean.row.descriptor_uuid ||
        equality.row.result_descriptor_generation != boolean.row.descriptor_generation ||
        equality.row.result_type_uuid != boolean.row.type_uuid || equality.row.result_type_generation != boolean.row.type_generation ||
        equality.row.result_codec_id != boolean.row.codec_id || equality.row.result_codec_version != boolean.row.codec_version ||
        equality.row.result_codec_generation != boolean.row.codec_generation)
      return refuse("equality_registry", "DATATYPE.DESCRIPTOR.INVALID");
    wire::TypedUpdatePredicateRecord column, literal;
    if (!Issue(&column.node_occurrence_uuid) || !Issue(&literal.node_occurrence_uuid) ||
        !Issue(&column.referenced_column_occurrence_uuid) || !Output(type, &column) || !Output(type, &literal) ||
        !projection::TypedUuid(selected->column_uuid, &column.referenced_column_uuid) ||
        !projection::TypedUuid(equality.row.operator_uuid, &root.operator_uuid)) return refuse("predicate_identity");
    column.node_id = 1; literal.node_id = 2; root.node_id = 3;
    column.node_occurrence_generation = literal.node_occurrence_generation = 1;
    column.node_kind = wire::TypedUpdatePredicateNodeKind::column_reference;
    column.value_state = wire::TypedUpdateValueState::absent;
    column.referenced_relation_occurrence_uuid = occurrence_uuid;
    column.referenced_relation_occurrence_generation = occurrence_generation;
    column.referenced_column_occurrence_generation = 1;
    column.referenced_column_generation = selected->column_generation;
    literal.node_kind = wire::TypedUpdatePredicateNodeKind::typed_literal;
    literal.value_state = wire::TypedUpdateValueState::value;
    for (std::size_t n = 0; n < type.canonical_value_bytes; ++n)
      literal.canonical_value.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (n * 8)));
    root.node_kind = wire::TypedUpdatePredicateNodeKind::comparison;
    root.value_state = wire::TypedUpdateValueState::absent;
    root.left_child_node_id = 1; root.right_child_node_id = 2;
    root.operator_generation = equality.row.operator_generation;
    EngineTypedValue bound;
    bound.descriptor = selected->value_descriptor;
    bound.encoded_value = std::to_string(value);
    bound.binary_value = literal.canonical_value;
    bound.setState(EngineValueState::value);
    result.execution_predicate.predicate_kind = "column_equals";
    result.execution_predicate.canonical_predicate_envelope = selected->canonical_name_key;
    result.execution_predicate.bound_values.push_back(std::move(bound));
    predicate.records.push_back(std::move(column));
    predicate.records.push_back(std::move(literal));
  }
  predicate.records.push_back(std::move(root));
  wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> bytes;
  if (!wire::EncodeTypedUpdatePredicateVector(predicate, &bytes, &error) ||
      !wire::DecodeAndValidateTypedUpdatePredicateVector(bytes, &predicate, &error))
    return refuse(error.field, "DATATYPE.DESCRIPTOR.INVALID");
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}
}  // namespace scratchbird::engine::internal_api
