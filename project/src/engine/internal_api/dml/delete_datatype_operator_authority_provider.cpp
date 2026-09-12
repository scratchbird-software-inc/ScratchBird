// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_datatype_operator_authority_provider.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "api_diagnostics.hpp"
#include <tuple>

namespace scratchbird::engine::internal_api {
namespace projection = datatype_operator_projection;
struct EngineDmlDeleteDatatypeAuthorityHandleV1::Authority {
  EngineRequestContext owner;
  std::vector<std::uint8_t> descriptor, predicate, datatypes, operators;
};
namespace {
EngineApiDiagnostic Error(std::string code, std::string field) {
  return MakeEngineApiDiagnostic(std::move(code), "sblr.dml_delete_rows.datatype_authority_invalid",
                                 std::move(field), true);
}
EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}
bool HasTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) != context.trace_tags.end();
}
EngineApiDiagnostic ValidateContext(const EngineRequestContext& context, std::string_view phase) {
  if (!context.security_context_present || !context.authorization_context.present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !HasTag(context, phase)) return Error("SECURITY.ACCESS_DENIED", "private_DELETE_provider_required");
  for (const auto tag : {"private_dml_delete_rows_binder", "private_dml_delete_rows_consumer",
                         "private_dml_delete_rows_recovery", "private_dml_update_rows_binder",
                         "private_dml_update_rows_consumer", "private_dml_update_rows_recovery"})
    if (tag != phase && HasTag(context, tag))
      return Error("SECURITY.ACCESS_DENIED", "mixed_operation_or_phase");
  if (context.read_only_mode || context.cluster_transaction_active || context.route_fence_present ||
      context.local_transaction_id == 0 || !context.statement_snapshot_generation ||
      !context.catalog_generation_id || !context.datatype_catalog_generation ||
      !context.datatype_registry_generation || !context.authorization_context.security_context_generation ||
      context.authorization_context.principal_uuid != context.principal_uuid)
    return Error("MGA.TRANSACTION.STALE", "context_generation");
  wire::TypedUpdateUuid binary{};
  for (const auto* id : {&context.database_uuid, &context.session_uuid, &context.principal_uuid,
                        &context.transaction_uuid, &context.statement_receipt_uuid,
                        &context.statement_snapshot_uuid, &context.statement_metadata_snapshot_uuid,
                        &context.datatype_catalog_snapshot_uuid, &context.authorization_context.authority_uuid})
    if (!projection::TypedUuid(id->canonical, &binary))
      return Error("MGA.TRANSACTION.STALE", "context_identity");
  return Ok();
}
auto OwnerKey(const EngineRequestContext& c) {
  return std::tie(c.database_path, c.database_uuid, c.session_uuid,
      c.principal_uuid, c.transaction_uuid, c.local_transaction_id,
      c.statement_receipt_uuid, c.statement_snapshot_uuid,
      c.statement_snapshot_generation, c.statement_metadata_snapshot_uuid,
      c.catalog_generation_id, c.datatype_catalog_snapshot_uuid,
      c.datatype_catalog_generation, c.datatype_registry_generation,
      c.authorization_context.authority_uuid,
      c.authorization_context.security_context_generation, c.security_epoch);
}
EngineDmlDeleteDatatypeAuthorityResultV1 Project(
    const EngineRequestContext& context, const wire::TypedDeleteDescriptorCarrier& descriptor,
    const wire::TypedUpdatePredicateVector& predicate) {
  EngineDmlDeleteDatatypeAuthorityResultV1 result;
  const auto refuse = [&](std::string code, std::string field) {
    result.diagnostic = Error(std::move(code), std::move(field)); return result;
  };
  const auto same_uuid = [](const wire::TypedUpdateUuid& binary, const EngineUuid& text) {
    return projection::UuidText(binary) == text;
  };
  if (!same_uuid(descriptor.authenticated_statement_receipt_uuid, context.statement_receipt_uuid) ||
      !same_uuid(descriptor.owning_transaction_uuid, context.transaction_uuid) ||
      descriptor.owning_local_transaction_id != context.local_transaction_id ||
      !same_uuid(descriptor.statement_snapshot_uuid, context.statement_snapshot_uuid) ||
      !same_uuid(descriptor.catalog_snapshot_uuid, context.statement_metadata_snapshot_uuid) ||
      descriptor.catalog_generation != context.catalog_generation_id ||
      !same_uuid(descriptor.security_context_uuid, context.authorization_context.authority_uuid) ||
      descriptor.datatype_registry_generation != context.datatype_registry_generation)
    return refuse("MGA.TRANSACTION.STALE", "descriptor_owner");
  // The durable security-catalog generation is not the authorization-context
  // generation. DELETE's security provider owns that independent comparison.
  const auto live = scratchbird::core::datatypes::LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();
  if (!live.ok || live.snapshot_uuid != projection::UuidText(descriptor.builtin_operator_snapshot_uuid) ||
      live.registry_generation != descriptor.builtin_operator_registry_generation)
    return refuse("MGA.TRANSACTION.STALE", "operator_snapshot");
  std::vector<projection::DatatypeReference> references;
  for (const auto& node : predicate.records)
    projection::AddReference({node.output_descriptor_uuid, node.output_descriptor_generation,
        node.output_type_uuid, node.output_type_generation, node.output_codec_id,
        node.output_codec_version, node.output_codec_generation}, &references);
  if (references.empty() || references.size() > 2)
    return refuse("DATATYPE.DESCRIPTOR.INVALID", "predicate_datatype_count");
  auto& datatypes = result.datatypes;
  datatypes.identity.vector_uuid = wire::kTypedUpdateDatatypeSnapshotUuid;
  datatypes.identity.vector_generation = descriptor.datatype_registry_generation;
  datatypes.identity.owner_descriptor_uuid = descriptor.descriptor_uuid;
  datatypes.identity.owner_descriptor_generation = descriptor.descriptor_generation;
  for (const auto& reference : references) {
    wire::TypedUpdateDatatypeAuthorityRecord row;
    if (!projection::BuildDatatypeRecord(context, reference, &row) ||
        row.datatype_identity_code == wire::TypedUpdateDatatypeIdentityCode::text_v2)
      return refuse("DATATYPE.DESCRIPTOR.INVALID", "fixed_width_live_registry_row_required");
    datatypes.records.push_back(std::move(row));
  }
  std::sort(datatypes.records.begin(), datatypes.records.end(), projection::DatatypeRowLess);
  for (std::size_t n = 0; n < datatypes.records.size(); ++n)
    datatypes.records[n].datatype_ordinal = static_cast<std::uint32_t>(n + 1);
  auto& operators = result.operators;
  operators.identity.vector_uuid = descriptor.builtin_operator_snapshot_uuid;
  operators.identity.vector_generation = descriptor.builtin_operator_registry_generation;
  operators.identity.owner_descriptor_uuid = descriptor.descriptor_uuid;
  operators.identity.owner_descriptor_generation = descriptor.descriptor_generation;
  if (predicate.records.size() == 3) {
    wire::TypedUpdateBuiltinOperatorAuthorityRecord row;
    if (!projection::BuildOperatorRecord(descriptor, predicate, &row))
      return refuse("DATATYPE.DESCRIPTOR.INVALID", "live_equality_operator_required");
    operators.records.push_back(std::move(row));
  }
  wire::TypedUpdateCarrierError shared;
  wire::TypedDeleteCarrierError error;
  std::vector<std::uint8_t> bytes;
  if (!wire::EncodeTypedUpdateDatatypeAuthorityVector(datatypes, &bytes, &shared) ||
      !wire::DecodeAndValidateTypedUpdateDatatypeAuthorityVector(bytes, &datatypes, &shared) ||
      !wire::EncodeTypedUpdateBuiltinOperatorAuthorityVector(operators, &bytes, &shared) ||
      !wire::DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(bytes, &operators, &shared) ||
      !wire::ValidateTypedDeleteDatatypeOperatorAuthority(descriptor, predicate, datatypes, operators, &error))
    return refuse("DATATYPE.DESCRIPTOR.INVALID", error.field.empty() ? shared.field : error.field);
  result.ok = true; result.diagnostic = Ok(); return result;
}
}  // namespace

EngineDmlDeleteDatatypeAuthorityResultV1 CaptureDmlDeleteDatatypeAuthorityV1(
    const EngineDmlDeleteDatatypeAuthorityRequestV1& request) {
  EngineDmlDeleteDatatypeAuthorityResultV1 result;
  result.diagnostic = ValidateContext(request.context, "private_dml_delete_rows_binder");
  if (result.diagnostic.error) return result;
  wire::TypedDeleteDescriptorCarrier descriptor;
  wire::TypedUpdatePredicateVector predicate;
  wire::TypedDeleteCarrierError error;
  wire::TypedUpdateCarrierError shared;
  if (!wire::DecodeAndValidateTypedDeleteDescriptor(request.exact_descriptor_dddc, &descriptor, &error) ||
      !wire::DecodeAndValidateTypedUpdatePredicateVector(request.exact_predicate_duev, &predicate, &shared)) {
    result.diagnostic = Error("DML.DELETE_FAILED", error.field.empty() ? shared.field : error.field);
    return result;
  }
  result = Project(request.context, descriptor, predicate);
  if (!result.ok) return result;
  auto authority = std::make_shared<EngineDmlDeleteDatatypeAuthorityHandleV1::Authority>();
  authority->owner = request.context;
  authority->descriptor = request.exact_descriptor_dddc;
  authority->predicate = request.exact_predicate_duev;
  authority->datatypes = result.datatypes.exact_bytes;
  authority->operators = result.operators.exact_bytes;
  result.handle.authority_ = std::move(authority);
  return result;
}

bool MatchesDmlDeleteDatatypeCaptureV1(const EngineDmlDeleteDatatypeAuthorityResultV1& captured,
    const wire::TypedDeleteDescriptorCarrier& descriptor, const wire::TypedUpdatePredicateVector& predicate) {
  if (!captured.ok || !captured.handle.valid()) return false;
  const auto& authority = *captured.handle.authority_;
  std::vector<std::uint8_t> dddc, duev;
  wire::TypedDeleteCarrierError de; wire::TypedUpdateCarrierError se;
  return wire::EncodeTypedDeleteDescriptor(descriptor, &dddc, &de) &&
      wire::EncodeTypedUpdatePredicateVector(predicate, &duev, &se) &&
      dddc == authority.descriptor && duev == authority.predicate;
}

EngineApiDiagnostic RevalidateDmlDeleteDatatypeAuthorityV1(
    const EngineRequestContext& context, const EngineDmlDeleteDatatypeAuthorityResultV1& captured) {
  const auto valid = ValidateContext(context, "private_dml_delete_rows_consumer");
  if (valid.error) return valid;
  if (!captured.ok || !captured.handle.valid())
    return Error("DATATYPE.DESCRIPTOR.INVALID", "engine_handle_required");
  const auto& authority = *captured.handle.authority_;
  if (OwnerKey(context) != OwnerKey(authority.owner)) return Error("MGA.TRANSACTION.STALE", "handle_owner");
  wire::TypedDeleteDescriptorCarrier descriptor;
  wire::TypedUpdatePredicateVector predicate;
  wire::TypedDeleteCarrierError error;
  wire::TypedUpdateCarrierError shared;
  if (!wire::DecodeAndValidateTypedDeleteDescriptor(authority.descriptor, &descriptor, &error) ||
      !wire::DecodeAndValidateTypedUpdatePredicateVector(authority.predicate, &predicate, &shared) ||
      !wire::ValidateTypedDeleteDatatypeOperatorAuthority(descriptor, predicate,
          captured.datatypes, captured.operators, &error) ||
      captured.datatypes.exact_bytes != authority.datatypes || captured.operators.exact_bytes != authority.operators)
    return Error("DML.DELETE_FAILED", "handle_projection_changed");
  const auto current = Project(context, descriptor, predicate);
  if (!current.ok) return current.diagnostic;
  if (current.datatypes.exact_bytes != authority.datatypes || current.operators.exact_bytes != authority.operators)
    return Error("MGA.TRANSACTION.STALE", "live_registry_changed");
  return Ok();
}

EngineApiDiagnostic RevalidateRecoveredDmlDeleteDatatypeAuthorityV1(
    const EngineRequestContext& context, const wire::TypedDeleteDescriptorCarrier& descriptor,
    const wire::TypedUpdatePredicateVector& predicate, const wire::TypedUpdateDatatypeAuthorityVector& datatypes,
    const wire::TypedUpdateBuiltinOperatorAuthorityVector& operators) {
  const auto valid = ValidateContext(context, "private_dml_delete_rows_recovery");
  if (valid.error) return valid;
  wire::TypedDeleteCarrierError error;
  if (!wire::ValidateTypedDeleteDatatypeOperatorAuthority(descriptor, predicate, datatypes, operators, &error))
    return Error("DML.DELETE_FAILED", error.field);
  const auto current = Project(context, descriptor, predicate);
  if (!current.ok) return current.diagnostic;
  if (current.datatypes.exact_bytes != datatypes.exact_bytes || current.operators.exact_bytes != operators.exact_bytes)
    return Error("MGA.TRANSACTION.STALE", "recovered_registry_changed");
  return Ok();
}
}  // namespace scratchbird::engine::internal_api
