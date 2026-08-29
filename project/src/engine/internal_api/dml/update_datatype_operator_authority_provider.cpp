// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_datatype_operator_authority_provider.hpp"

#include "api_diagnostics.hpp"
#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {

namespace update_wire = scratchbird::wire;
namespace datatype_catalog = scratchbird::core::datatypes;

struct EngineDmlUpdateDatatypeSnapshotHandleV1::Authority {
  std::string database_uuid;
  std::string authenticated_statement_receipt_uuid;
  std::string datatype_snapshot_uuid;
  std::uint64_t datatype_catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::vector<std::uint8_t> exact_descriptor_dudc;
  std::vector<std::uint8_t> exact_assignment_vector_duav;
  std::vector<std::uint8_t> exact_predicate_vector_duev;
  std::vector<std::uint8_t> exact_datatype_authority_dudv;
};

struct EngineDmlUpdateBuiltinOperatorSnapshotHandleV1::Authority {
  std::string database_uuid;
  std::string authenticated_statement_receipt_uuid;
  std::string operator_snapshot_uuid;
  std::uint64_t operator_registry_generation = 0;
  std::vector<std::uint8_t> exact_builtin_operator_authority_duov;
};

namespace {

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool HasTraceTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool ExactUuid(std::string_view text) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

bool TypedUuid(std::string_view text, update_wire::TypedUpdateUuid* out) {
  if (out == nullptr || !ExactUuid(text)) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
  return true;
}

std::string UuidText(const update_wire::TypedUpdateUuid& uuid) {
  scratchbird::core::platform::Uuid value;
  std::copy(uuid.begin(), uuid.end(), value.bytes.begin());
  if (scratchbird::core::uuid::IsNilUuid(value)) return {};
  return scratchbird::core::uuid::UuidToString(value);
}

EngineApiDiagnostic CarrierDiagnostic(
    const update_wire::TypedUpdateCarrierError& error,
    std::string_view fallback) {
  return Diagnostic(error.diagnostic_code.empty()
                        ? "DATATYPE.DESCRIPTOR_INVALID"
                        : error.diagnostic_code,
                    std::string(fallback),
                    error.field.empty() ? error.detail
                                        : error.field + ":" + error.detail);
}

enum class ProviderPhaseV1 : std::uint8_t {
  binder = 1,
  consumer = 2,
  recovery = 3,
};

EngineApiDiagnostic ValidateProviderContext(
    const EngineRequestContext& context, ProviderPhaseV1 phase) {
  const bool binder =
      HasTraceTag(context, "private_dml_update_rows_binder");
  const bool consumer =
      HasTraceTag(context, "private_dml_update_rows_consumer");
  const bool recovery =
      HasTraceTag(context, "private_dml_update_rows_recovery");
  const std::string_view required_tag =
      phase == ProviderPhaseV1::binder
          ? "private_dml_update_rows_binder"
          : phase == ProviderPhaseV1::consumer
                ? "private_dml_update_rows_consumer"
                : "private_dml_update_rows_recovery";
  if (!context.security_context_present ||
      !context.authorization_context.present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !HasTraceTag(context, required_tag) ||
      (phase != ProviderPhaseV1::binder && binder) ||
      (phase != ProviderPhaseV1::consumer && consumer) ||
      (phase != ProviderPhaseV1::recovery && recovery)) {
    return Diagnostic(
        "SECURITY.ACCESS_DENIED",
        "sblr.dml_update_rows.datatype_operator_provider_denied");
  }
  if (context.read_only_mode || context.cluster_transaction_active ||
      context.route_fence_present || context.local_transaction_id == 0 ||
      !ExactUuid(context.database_uuid.canonical) ||
      !ExactUuid(context.transaction_uuid.canonical) ||
      !ExactUuid(context.statement_receipt_uuid.canonical) ||
      !ExactUuid(context.datatype_catalog_snapshot_uuid.canonical) ||
      context.datatype_catalog_generation == 0 ||
      context.datatype_registry_generation == 0) {
    return Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.datatype_operator_context_invalid");
  }
  return Ok();
}

EngineApiDiagnostic ValidateRequest(
    const EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1& request) {
  const auto& context = request.context;
  auto diagnostic = ValidateProviderContext(context, ProviderPhaseV1::binder);
  if (diagnostic.error) return diagnostic;
  if (
      request.authenticated_statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      request.exact_descriptor_dudc.empty() ||
      request.exact_assignment_vector_duav.empty() ||
      request.exact_predicate_vector_duev.empty()) {
    return Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.datatype_operator_capture_invalid");
  }
  return Ok();
}

struct DatatypeReference {
  update_wire::TypedUpdateUuid descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  update_wire::TypedUpdateUuid type_uuid{};
  std::uint64_t type_generation = 0;
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;

  bool operator==(const DatatypeReference&) const = default;
};

void AddReference(const DatatypeReference& reference,
                  std::vector<DatatypeReference>* references) {
  if (std::find(references->begin(), references->end(), reference) ==
      references->end()) {
    references->push_back(reference);
  }
}

bool DatatypeRowLess(
    const update_wire::TypedUpdateDatatypeAuthorityRecord& left,
    const update_wire::TypedUpdateDatatypeAuthorityRecord& right) {
  if (left.descriptor_uuid != right.descriptor_uuid) {
    return left.descriptor_uuid < right.descriptor_uuid;
  }
  if (left.descriptor_generation != right.descriptor_generation) {
    return left.descriptor_generation < right.descriptor_generation;
  }
  return left.type_uuid < right.type_uuid;
}

bool BuildDatatypeRecord(
    const EngineRequestContext& context,
    const DatatypeReference& reference,
    update_wire::TypedUpdateDatatypeAuthorityRecord* record) {
  if (record == nullptr) return false;
  const auto lookup = datatype_catalog::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid.canonical,
      context.datatype_catalog_generation,
      context.datatype_registry_generation,
      UuidText(reference.descriptor_uuid), reference.descriptor_generation);
  if (!lookup.ok || lookup.row.type_uuid != UuidText(reference.type_uuid) ||
      lookup.row.type_generation != reference.type_generation ||
      lookup.row.codec_id != reference.codec_id ||
      lookup.row.codec_version != reference.codec_version ||
      lookup.row.codec_generation != reference.codec_generation ||
      lookup.row.datatype_identity_code == 0 ||
      lookup.row.null_encoding_code == 0 ||
      lookup.row.byte_order_code == 0 ||
      lookup.row.representation_code == 0 ||
      lookup.row.canonical_name.empty() ||
      lookup.row.canonical_value_minimum_bytes == 0 ||
      lookup.row.canonical_value_minimum_bytes !=
          lookup.row.canonical_value_maximum_bytes ||
      lookup.row.canonical_value_minimum_bytes !=
          lookup.row.canonical_value_exact_bytes ||
      lookup.row.canonical_value_exact_bytes !=
          lookup.row.canonical_value_bytes ||
      !TypedUuid(lookup.row.descriptor_uuid, &record->descriptor_uuid) ||
      !TypedUuid(lookup.row.type_uuid, &record->type_uuid) ||
      !TypedUuid(lookup.row.catalog_snapshot_uuid,
                 &record->datatype_snapshot_uuid)) {
    return false;
  }
  record->datatype_identity_code =
      static_cast<update_wire::TypedUpdateDatatypeIdentityCode>(
          lookup.row.datatype_identity_code);
  record->null_encoding_code =
      static_cast<update_wire::TypedUpdateNullEncodingCode>(
          lookup.row.null_encoding_code);
  record->byte_order_code =
      static_cast<update_wire::TypedUpdateByteOrderCode>(
          lookup.row.byte_order_code);
  record->is_signed = lookup.row.signed_code;
  record->descriptor_generation = lookup.row.descriptor_generation;
  record->type_generation = lookup.row.type_generation;
  record->codec_version = lookup.row.codec_version;
  record->canonical_name = lookup.row.canonical_name;
  record->codec_id = lookup.row.codec_id;
  record->representation_code =
      static_cast<update_wire::TypedUpdateRepresentationCode>(
          lookup.row.representation_code);
  record->codec_generation = lookup.row.codec_generation;
  record->canonical_value_minimum_bytes =
      lookup.row.canonical_value_minimum_bytes;
  record->canonical_value_maximum_bytes =
      lookup.row.canonical_value_maximum_bytes;
  record->canonical_value_exact_bytes =
      lookup.row.canonical_value_exact_bytes;
  record->datatype_catalog_generation = lookup.row.catalog_generation;
  record->datatype_registry_generation = lookup.row.registry_generation;
  return true;
}

bool BuildOperatorRecord(
    const update_wire::TypedUpdateDescriptorCarrier& descriptor,
    const update_wire::TypedUpdatePredicateVector& predicate,
    update_wire::TypedUpdateBuiltinOperatorAuthorityRecord* record) {
  if (record == nullptr || predicate.records.size() != 3) return false;
  const auto& left = predicate.records[0];
  const auto& right = predicate.records[1];
  const auto& comparison = predicate.records[2];
  const auto lookup =
      datatype_catalog::LookupBuiltinOperatorTypeCodecIdentityV1(
          UuidText(descriptor.builtin_operator_snapshot_uuid),
          descriptor.builtin_operator_registry_generation,
          UuidText(comparison.operator_uuid), comparison.operator_generation,
          UuidText(left.output_descriptor_uuid),
          left.output_descriptor_generation, UuidText(left.output_type_uuid),
          left.output_type_generation, UuidText(right.output_descriptor_uuid),
          right.output_descriptor_generation,
          UuidText(right.output_type_uuid), right.output_type_generation);
  if (!lookup.ok ||
      !TypedUuid(lookup.row.operator_uuid, &record->operator_uuid) ||
      !TypedUuid(lookup.row.operator_snapshot_uuid,
                 &record->operator_snapshot_uuid) ||
      !TypedUuid(lookup.row.left_descriptor_uuid,
                 &record->left_descriptor_uuid) ||
      !TypedUuid(lookup.row.left_type_uuid, &record->left_type_uuid) ||
      !TypedUuid(lookup.row.right_descriptor_uuid,
                 &record->right_descriptor_uuid) ||
      !TypedUuid(lookup.row.right_type_uuid, &record->right_type_uuid) ||
      !TypedUuid(lookup.row.result_descriptor_uuid,
                 &record->result_descriptor_uuid) ||
      !TypedUuid(lookup.row.result_type_uuid, &record->result_type_uuid)) {
    return false;
  }
  record->operator_ordinal = 1;
  record->semantic_code = lookup.row.semantic_code;
  record->operand_arity = lookup.row.operand_arity;
  record->null_behavior_code = lookup.row.null_behavior_code;
  record->accepted_state = lookup.row.accepted_state;
  record->operator_generation = lookup.row.operator_generation;
  record->operator_registry_generation =
      lookup.row.operator_registry_generation;
  record->left_descriptor_generation =
      lookup.row.left_descriptor_generation;
  record->left_type_generation = lookup.row.left_type_generation;
  record->right_descriptor_generation =
      lookup.row.right_descriptor_generation;
  record->right_type_generation = lookup.row.right_type_generation;
  record->result_descriptor_generation =
      lookup.row.result_descriptor_generation;
  record->result_type_generation = lookup.row.result_type_generation;
  record->result_codec_version = lookup.row.result_codec_version;
  record->operator_family_code = lookup.row.operator_family_code;
  record->result_codec_generation = lookup.row.result_codec_generation;
  record->result_codec_id = lookup.row.result_codec_id;
  record->operand_identity_rule = lookup.row.operand_identity_rule;
  record->result_null_encoding_code =
      lookup.row.result_null_encoding_code;
  return true;
}

}  // namespace

EngineDmlUpdateDatatypeOperatorBindingResultV1
ResolveDmlUpdateDatatypeOperatorBindingAuthorityV1(
    const EngineDmlUpdateDatatypeOperatorBindingRequestV1& request) {
  EngineDmlUpdateDatatypeOperatorBindingResultV1 result;
  result.diagnostic =
      ValidateProviderContext(request.context, ProviderPhaseV1::binder);
  if (result.diagnostic.error) return result;

  const auto boolean =
      datatype_catalog::LookupCanonicalBooleanTypeCodecIdentityV1(
      request.context.datatype_catalog_snapshot_uuid.canonical,
      request.context.datatype_catalog_generation,
      request.context.datatype_registry_generation);
  const auto operator_snapshot =
      datatype_catalog::LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();
  if (!boolean.ok || boolean.row.datatype_identity_code != 1 ||
      boolean.row.canonical_name != "boolean" ||
      boolean.row.null_encoding_code != 1 ||
      boolean.row.byte_order_code != 1 || boolean.row.signed_code ||
      boolean.row.representation_code != 1 ||
      boolean.row.canonical_value_exact_bytes != 1 ||
      !operator_snapshot.ok ||
      !ExactUuid(operator_snapshot.snapshot_uuid) ||
      operator_snapshot.registry_generation == 0) {
    result.diagnostic = Diagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.dml_update_rows.boolean_operator_registry_unavailable");
    return result;
  }

  result.datatype_snapshot_uuid = boolean.row.catalog_snapshot_uuid;
  result.datatype_catalog_generation = boolean.row.catalog_generation;
  result.datatype_registry_generation = boolean.row.registry_generation;
  result.boolean_descriptor_uuid = boolean.row.descriptor_uuid;
  result.boolean_descriptor_generation = boolean.row.descriptor_generation;
  result.boolean_type_uuid = boolean.row.type_uuid;
  result.boolean_type_generation = boolean.row.type_generation;
  result.boolean_codec_id = boolean.row.codec_id;
  result.boolean_codec_version = boolean.row.codec_version;
  result.boolean_codec_generation = boolean.row.codec_generation;
  result.builtin_operator_snapshot_uuid = operator_snapshot.snapshot_uuid;
  result.builtin_operator_registry_generation =
      operator_snapshot.registry_generation;

  if (request.equality_required) {
    if (!ExactUuid(request.left_descriptor_uuid) ||
        request.left_descriptor_generation == 0 ||
        !ExactUuid(request.left_type_uuid) ||
        request.left_type_generation == 0 ||
        !ExactUuid(request.right_descriptor_uuid) ||
        request.right_descriptor_generation == 0 ||
        !ExactUuid(request.right_type_uuid) ||
        request.right_type_generation == 0) {
      result.diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.equality_operand_identity_invalid");
      return result;
    }
    const auto equality =
        datatype_catalog::LookupBuiltinOperatorTypeCodecIdentityV1(
            result.builtin_operator_snapshot_uuid,
            result.builtin_operator_registry_generation,
            operator_snapshot.equality_operator_uuid,
            operator_snapshot.equality_operator_generation,
            request.left_descriptor_uuid,
            request.left_descriptor_generation, request.left_type_uuid,
            request.left_type_generation, request.right_descriptor_uuid,
            request.right_descriptor_generation, request.right_type_uuid,
            request.right_type_generation);
    if (!equality.ok ||
        equality.row.result_descriptor_uuid !=
            result.boolean_descriptor_uuid ||
        equality.row.result_descriptor_generation !=
            result.boolean_descriptor_generation ||
        equality.row.result_type_uuid != result.boolean_type_uuid ||
        equality.row.result_type_generation !=
            result.boolean_type_generation ||
        equality.row.result_codec_id != result.boolean_codec_id ||
        equality.row.result_codec_version != result.boolean_codec_version ||
        equality.row.result_codec_generation !=
            result.boolean_codec_generation) {
      result.diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.equality_operator_registry_unavailable");
      return result;
    }
    result.equality_operator_uuid = equality.row.operator_uuid;
    result.equality_operator_generation = equality.row.operator_generation;
  }

  result.ok = true;
  result.diagnostic = Ok();
  return result;
}

EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1
CaptureDmlUpdateDatatypeOperatorAuthorityV1(
    const EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1& request) {
  EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1 result;
  result.diagnostic = ValidateRequest(request);
  if (result.diagnostic.error) return result;

  update_wire::TypedUpdateCarrierError error;
  update_wire::TypedUpdateDescriptorCarrier descriptor;
  update_wire::TypedUpdateAssignmentVector assignments;
  update_wire::TypedUpdatePredicateVector predicate;
  if (!update_wire::DecodeAndValidateTypedUpdateDescriptor(
          request.exact_descriptor_dudc, &descriptor, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateAssignmentVector(
          request.exact_assignment_vector_duav, &assignments, &error) ||
      !update_wire::DecodeAndValidateTypedUpdatePredicateVector(
          request.exact_predicate_vector_duev, &predicate, &error)) {
    result.diagnostic = CarrierDiagnostic(
        error, "sblr.dml_update_rows.datatype_operator_source_invalid");
    return result;
  }

  update_wire::TypedUpdateUuid receipt_uuid{};
  update_wire::TypedUpdateUuid transaction_uuid{};
  const auto operator_snapshot =
      datatype_catalog::LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();
  if (!TypedUuid(request.authenticated_statement_receipt_uuid,
                 &receipt_uuid) ||
      !TypedUuid(request.context.transaction_uuid.canonical,
                 &transaction_uuid) ||
      !operator_snapshot.ok ||
      descriptor.authenticated_statement_receipt_uuid != receipt_uuid ||
      descriptor.owning_transaction_uuid != transaction_uuid ||
      descriptor.owning_local_transaction_id !=
          request.context.local_transaction_id ||
      descriptor.datatype_registry_generation !=
          request.context.datatype_registry_generation ||
      assignments.identity.vector_uuid != descriptor.assignment_vector_uuid ||
      assignments.identity.vector_generation !=
          descriptor.assignment_vector_generation ||
      assignments.identity.owner_descriptor_uuid != descriptor.descriptor_uuid ||
      assignments.identity.owner_descriptor_generation !=
          descriptor.descriptor_generation ||
      predicate.identity.vector_uuid != descriptor.predicate_expression_uuid ||
      predicate.identity.vector_generation !=
          descriptor.predicate_expression_generation ||
      predicate.identity.owner_descriptor_uuid != descriptor.descriptor_uuid ||
      predicate.identity.owner_descriptor_generation !=
          descriptor.descriptor_generation ||
      UuidText(descriptor.builtin_operator_snapshot_uuid) !=
          operator_snapshot.snapshot_uuid ||
      descriptor.builtin_operator_registry_generation !=
          operator_snapshot.registry_generation) {
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.datatype_operator_source_stale");
    return result;
  }

  std::vector<DatatypeReference> references;
  references.reserve(assignments.records.size() + predicate.records.size());
  for (const auto& assignment : assignments.records) {
    AddReference({assignment.value_descriptor_uuid,
                  assignment.value_descriptor_generation,
                  assignment.value_type_uuid, assignment.value_type_generation,
                  assignment.codec_id, assignment.codec_version,
                  assignment.codec_generation},
                 &references);
  }
  for (const auto& node : predicate.records) {
    AddReference({node.output_descriptor_uuid,
                  node.output_descriptor_generation, node.output_type_uuid,
                  node.output_type_generation, node.output_codec_id,
                  node.output_codec_version, node.output_codec_generation},
                 &references);
  }
  if (references.empty() || references.size() > 5) {
    result.diagnostic = Diagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.dml_update_rows.datatype_reference_count_invalid");
    return result;
  }

  update_wire::TypedUpdateDatatypeAuthorityVector datatypes;
  datatypes.identity.vector_uuid = update_wire::kTypedUpdateDatatypeSnapshotUuid;
  datatypes.identity.vector_generation =
      descriptor.datatype_registry_generation;
  datatypes.identity.owner_descriptor_uuid = descriptor.descriptor_uuid;
  datatypes.identity.owner_descriptor_generation =
      descriptor.descriptor_generation;
  datatypes.records.reserve(references.size());
  for (const auto& reference : references) {
    update_wire::TypedUpdateDatatypeAuthorityRecord row;
    if (!BuildDatatypeRecord(request.context, reference, &row)) {
      result.diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.datatype_registry_row_unavailable");
      return result;
    }
    datatypes.records.push_back(std::move(row));
  }
  std::sort(datatypes.records.begin(), datatypes.records.end(),
            DatatypeRowLess);
  for (std::size_t index = 0; index < datatypes.records.size(); ++index) {
    datatypes.records[index].datatype_ordinal =
        static_cast<std::uint32_t>(index + 1);
  }

  update_wire::TypedUpdateBuiltinOperatorAuthorityVector operators;
  operators.identity.vector_uuid = descriptor.builtin_operator_snapshot_uuid;
  operators.identity.vector_generation =
      descriptor.builtin_operator_registry_generation;
  operators.identity.owner_descriptor_uuid = descriptor.descriptor_uuid;
  operators.identity.owner_descriptor_generation =
      descriptor.descriptor_generation;
  if (predicate.records.size() == 3) {
    update_wire::TypedUpdateBuiltinOperatorAuthorityRecord row;
    if (!BuildOperatorRecord(descriptor, predicate, &row)) {
      result.diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.builtin_operator_registry_row_unavailable");
      return result;
    }
    operators.records.push_back(std::move(row));
  }

  std::vector<std::uint8_t> exact_dudv;
  std::vector<std::uint8_t> exact_duov;
  if (!update_wire::EncodeTypedUpdateDatatypeAuthorityVector(
          datatypes, &exact_dudv, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
          exact_dudv, &datatypes, &error) ||
      !update_wire::EncodeTypedUpdateBuiltinOperatorAuthorityVector(
          operators, &exact_duov, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
          exact_duov, &operators, &error) ||
      !update_wire::ValidateTypedUpdateDatatypeOperatorAuthority(
          descriptor, assignments, predicate, datatypes, operators, &error)) {
    result.diagnostic = CarrierDiagnostic(
        error, "sblr.dml_update_rows.datatype_operator_authority_invalid");
    return result;
  }

  auto datatype_handle =
      std::make_shared<EngineDmlUpdateDatatypeSnapshotHandleV1::Authority>();
  datatype_handle->database_uuid = request.context.database_uuid.canonical;
  datatype_handle->authenticated_statement_receipt_uuid =
      request.authenticated_statement_receipt_uuid;
  datatype_handle->datatype_snapshot_uuid =
      request.context.datatype_catalog_snapshot_uuid.canonical;
  datatype_handle->datatype_catalog_generation =
      request.context.datatype_catalog_generation;
  datatype_handle->datatype_registry_generation =
      request.context.datatype_registry_generation;
  datatype_handle->exact_descriptor_dudc = request.exact_descriptor_dudc;
  datatype_handle->exact_assignment_vector_duav =
      request.exact_assignment_vector_duav;
  datatype_handle->exact_predicate_vector_duev =
      request.exact_predicate_vector_duev;
  datatype_handle->exact_datatype_authority_dudv = exact_dudv;

  auto operator_handle = std::make_shared<
      EngineDmlUpdateBuiltinOperatorSnapshotHandleV1::Authority>();
  operator_handle->database_uuid = request.context.database_uuid.canonical;
  operator_handle->authenticated_statement_receipt_uuid =
      request.authenticated_statement_receipt_uuid;
  operator_handle->operator_snapshot_uuid =
      UuidText(descriptor.builtin_operator_snapshot_uuid);
  operator_handle->operator_registry_generation =
      descriptor.builtin_operator_registry_generation;
  operator_handle->exact_builtin_operator_authority_duov = exact_duov;

  result.ok = true;
  result.diagnostic = Ok();
  result.datatypes = std::move(datatypes);
  result.operators = std::move(operators);
  result.exact_datatype_authority_dudv = std::move(exact_dudv);
  result.exact_builtin_operator_authority_duov = std::move(exact_duov);
  result.datatype_snapshot_handle.authority_ = std::move(datatype_handle);
  result.operator_snapshot_handle.authority_ = std::move(operator_handle);
  return result;
}

EngineApiDiagnostic RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1& captured) {
  const auto consumer_context =
      ValidateProviderContext(context, ProviderPhaseV1::consumer);
  if (consumer_context.error) return consumer_context;
  if (!captured.ok || !captured.datatype_snapshot_handle.valid() ||
      !captured.operator_snapshot_handle.valid()) {
    return Diagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.dml_update_rows.datatype_operator_handle_invalid");
  }
  const auto& datatype = *captured.datatype_snapshot_handle.authority_;
  const auto& operation = *captured.operator_snapshot_handle.authority_;
  if (context.database_uuid.canonical != datatype.database_uuid ||
      context.database_uuid.canonical != operation.database_uuid ||
      context.statement_receipt_uuid.canonical !=
          datatype.authenticated_statement_receipt_uuid ||
      context.statement_receipt_uuid.canonical !=
          operation.authenticated_statement_receipt_uuid ||
      context.datatype_catalog_snapshot_uuid.canonical !=
          datatype.datatype_snapshot_uuid ||
      context.datatype_catalog_generation !=
          datatype.datatype_catalog_generation ||
      context.datatype_registry_generation !=
          datatype.datatype_registry_generation) {
    return Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.datatype_operator_handle_stale");
  }
  EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1 request;
  request.context = context;
  request.context.trace_tags.erase(
      std::remove_if(request.context.trace_tags.begin(),
                     request.context.trace_tags.end(),
                     [](const std::string& tag) {
                       return tag == "private_dml_update_rows_binder" ||
                              tag == "private_dml_update_rows_consumer" ||
                              tag == "private_dml_update_rows_recovery";
                     }),
      request.context.trace_tags.end());
  request.context.trace_tags.push_back("private_dml_update_rows_binder");
  request.authenticated_statement_receipt_uuid =
      datatype.authenticated_statement_receipt_uuid;
  request.exact_descriptor_dudc = datatype.exact_descriptor_dudc;
  request.exact_assignment_vector_duav = datatype.exact_assignment_vector_duav;
  request.exact_predicate_vector_duev = datatype.exact_predicate_vector_duev;
  const auto current = CaptureDmlUpdateDatatypeOperatorAuthorityV1(request);
  if (!current.ok ||
      current.exact_datatype_authority_dudv !=
          datatype.exact_datatype_authority_dudv ||
      current.exact_builtin_operator_authority_duov !=
          operation.exact_builtin_operator_authority_duov ||
      current.exact_datatype_authority_dudv !=
          captured.exact_datatype_authority_dudv ||
      current.exact_builtin_operator_authority_duov !=
          captured.exact_builtin_operator_authority_duov) {
    return Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.datatype_operator_authority_stale");
  }
  return Ok();
}

EngineApiDiagnostic RevalidateRecoveredDmlUpdateDatatypeOperatorAuthorityV1(
    const EngineRequestContext& context,
    const update_wire::TypedUpdateDescriptorCarrier& descriptor,
    const update_wire::TypedUpdateAssignmentVector& assignments,
    const update_wire::TypedUpdatePredicateVector& predicate,
    const update_wire::TypedUpdateDatatypeAuthorityVector& datatypes,
    const update_wire::TypedUpdateBuiltinOperatorAuthorityVector& operators) {
  const auto validated =
      ValidateProviderContext(context, ProviderPhaseV1::recovery);
  if (validated.error) return validated;
  if (descriptor.exact_bytes.empty() || assignments.exact_bytes.empty() ||
      predicate.exact_bytes.empty() || datatypes.exact_bytes.empty() ||
      operators.exact_bytes.empty()) {
    return Diagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.recovered_datatype_operator_bytes_missing");
  }
  EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1 request;
  request.context = context;
  request.context.trace_tags.erase(
      std::remove_if(request.context.trace_tags.begin(),
                     request.context.trace_tags.end(),
                     [](const std::string& tag) {
                       return tag == "private_dml_update_rows_binder" ||
                              tag == "private_dml_update_rows_consumer" ||
                              tag == "private_dml_update_rows_recovery";
                     }),
      request.context.trace_tags.end());
  request.context.trace_tags.push_back("private_dml_update_rows_binder");
  request.authenticated_statement_receipt_uuid =
      context.statement_receipt_uuid.canonical;
  request.exact_descriptor_dudc = descriptor.exact_bytes;
  request.exact_assignment_vector_duav = assignments.exact_bytes;
  request.exact_predicate_vector_duev = predicate.exact_bytes;
  const auto current = CaptureDmlUpdateDatatypeOperatorAuthorityV1(request);
  if (!current.ok) return current.diagnostic;
  if (current.exact_datatype_authority_dudv != datatypes.exact_bytes ||
      current.exact_builtin_operator_authority_duov != operators.exact_bytes) {
    return Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.recovered_datatype_operator_authority_stale");
  }
  return Ok();
}

}  // namespace scratchbird::engine::internal_api
