// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_api.hpp"
#include "transaction/transaction_api.hpp"
#include "dml/delete_predicate_binding.hpp"
#include "dml/delete_durable_owner_registry.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "api_diagnostics.hpp"
#include <chrono>

namespace scratchbird::engine::internal_api {
namespace {
namespace w = scratchbird::wire;
namespace p = datatype_operator_projection;
bool Issue(w::TypedUpdateUuid* out) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto id = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now);
  if (!id.ok()) return false;
  *out = id.value.value.bytes; return true;
}
struct UnpublishedGrant {
  const EngineRequestContext& context;
  std::shared_ptr<EngineDmlUpdateResourceReceiptV1> receipt;
  EngineDmlUpdateResourceHandleV1 resource;
  bool handed_off = false;
  ~UnpublishedGrant() {
    if (!handed_off && resource.valid() && receipt)
      (void)receipt->ReleaseNoAlloc(context, resource, EngineDmlUpdateResourceReleaseV1::abandoned_before_publication);
  }
};
}  // namespace
EngineDmlDeleteRowsBindResultV1 BindDmlDeleteRowsDescriptorV1(
    const EngineRequestContext& context, const EngineDmlDeleteRowsBindingDemandV1& demand) {
  EngineDmlDeleteRowsBindResultV1 result;
  auto interrupted = MakeEngineApiDiagnostic("DML.DELETE_FAILED", "sblr.dml_delete_rows.bind_interrupted", {}, true);
  UnpublishedGrant grant{context, context.dml_update_resource_receipt.lock()};
  const auto refuse = [&](const char* detail, const char* code = "SBLR.OPERAND_INVALID") {
    result.diagnostic = MakeEngineApiDiagnostic(code, "sblr.dml_delete_rows.bind_refused", detail, true);
    return result;
  };
  try {
    if (!grant.receipt || !grant.receipt->AuthenticatesContext(context) ||
        demand.authenticated_statement_receipt_uuid != context.statement_receipt_uuid ||
        !demand.structural_occurrence_id) return refuse("authenticated_receipt_and_occurrence_required", "SECURITY.ACCESS_DENIED");
    // Freeze catalog/security/source capture against the same engine append
    // authority used by execution. A new descriptor has no competing owner.
    const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
    DmlDeleteDurableAuthorityBundleV1 b;
    auto& d = b.descriptor;
    if (!Issue(&d.descriptor_uuid) || !Issue(&d.target_relation_occurrence_uuid)) return refuse("identity_issuance_failed");
    d.descriptor_generation = d.target_relation_occurrence_generation = 1;
    const auto predicate = BindDmlDeletePredicateV1(context, demand, d.descriptor_uuid, 1,
        d.target_relation_occurrence_uuid, 1);
    if (!predicate.ok) { result.diagnostic = predicate.diagnostic; return result; }
    const auto target = predicate.relation.relation_uuid;
    const auto security = CaptureDmlDeleteSecurityAuthorityV1(context, target);
    if (!security.ok) { result.diagnostic = security.diagnostic; return result; }
    const auto effects = CaptureDmlDeleteEffectAuthorityV1(context, target);
    if (!effects.ok) { result.diagnostic = effects.diagnostic; return result; }
    const auto executor = LoadSblrExecutorAvailabilitySnapshot(context,
        {"dml.delete_rows", 784, "1.0", "dml_delete_rows_descriptor", "mutation_result", 1});
    if (!executor.ok) { result.diagnostic = executor.diagnostic; return result; }
    const auto captured = grant.receipt->Capture(context);
    grant.resource = captured.handle;
    if (!captured.ok) { result.diagnostic = captured.diagnostic; return result; }
    b.resource_budget = *grant.resource.carrier();
    b.predicate = predicate.predicate; b.security = security.snapshot;
    b.matched_grant_uuids = security.matched_grant_uuids; b.effects = effects.snapshot; b.executor = executor.snapshot;
    if (!p::TypedUuid(context.database_uuid, &b.database_uuid) ||
        !p::TypedUuid(context.session_uuid, &b.session_uuid) ||
        !p::TypedUuid(context.principal_uuid, &b.principal_uuid) ||
        !p::TypedUuid(context.statement_receipt_uuid, &d.authenticated_statement_receipt_uuid) ||
        !p::TypedUuid(context.transaction_uuid, &d.owning_transaction_uuid) ||
        !p::TypedUuid(context.statement_snapshot_uuid, &d.statement_snapshot_uuid) ||
        !p::TypedUuid(context.statement_metadata_snapshot_uuid, &d.catalog_snapshot_uuid) ||
        !p::TypedUuid(security.snapshot.security_context_uuid, &d.security_context_uuid) ||
        !p::TypedUuid(security.snapshot.snapshot_uuid, &d.security_snapshot_uuid) ||
        !p::TypedUuid(target, &d.target_relation_uuid) || !Issue(&b.bundle_uuid) || !Issue(&b.reserved_statement_savepoint_uuid) ||
        !Issue(&d.operation_uuid) || !Issue(&d.deterministic_target_order_uuid) || !Issue(&d.recovery_token_uuid))
      return refuse("complete_binary_identity_required");
    d.structural_occurrence_id = demand.structural_occurrence_id; d.operation_generation = 1;
    d.owning_local_transaction_id = context.local_transaction_id;
    d.catalog_generation = context.catalog_generation_id; d.datatype_registry_generation = context.datatype_registry_generation;
    d.security_generation = security.snapshot.security_generation;
    d.target_relation_generation = predicate.relation.relation_generation;
    d.predicate_expression_uuid = b.predicate.identity.vector_uuid;
    d.predicate_expression_generation = b.predicate.identity.vector_generation;
    d.predicate_node_count = d.predicate_root_node_id = b.predicate.records.size();
    d.predicate_vector_sha256 = b.predicate.identity.vector_sha256;
    d.row_policy_set_uuid = d.security_snapshot_uuid; d.row_policy_set_generation = security.snapshot.snapshot_generation;
    if (!ComputeDmlDeleteSecuritySnapshotHashV1(b.security, b.matched_grant_uuids, &d.row_policy_set_sha256))
      return refuse("security_evidence_unavailable");
    d.constraint_set_uuid = b.effects.constraint_set_uuid; d.constraint_set_generation = b.effects.generation;
    d.ordered_constraint_set_sha256 = b.effects.constraint_set_sha256;
    d.trigger_set_uuid = b.effects.trigger_set_uuid; d.trigger_set_generation = b.effects.generation;
    d.ordered_trigger_set_sha256 = b.effects.trigger_set_sha256;
    d.deterministic_target_order_generation = 1;
    d.resource_budget_uuid = b.resource_budget.resource_budget_uuid;
    d.resource_budget_generation = b.resource_budget.resource_budget_generation;
    d.recovery_generation = 1; d.executor_availability_generation = executor.snapshot.generation;
    d.builtin_operator_snapshot_uuid = w::kTypedUpdateOperatorSnapshotUuid;
    d.builtin_operator_registry_generation = 1;
    b.bundle_generation = 1;
    auto& order = b.target_order;
    order.target_order_uuid = d.deterministic_target_order_uuid; order.target_order_generation = 1;
    order.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
    order.target_relation_occurrence_uuid = d.target_relation_occurrence_uuid;
    order.target_relation_occurrence_generation = d.target_relation_occurrence_generation;
    order.statement_snapshot_uuid = d.statement_snapshot_uuid;
    order.maximum_candidate_rows = b.resource_budget.maximum_candidate_rows;
    auto& recovery = b.recovery;
    recovery.recovery_token_uuid = d.recovery_token_uuid; recovery.recovery_generation = 1;
    recovery.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
    recovery.owning_transaction_uuid = d.owning_transaction_uuid; recovery.operation_uuid = d.operation_uuid;
    recovery.descriptor_uuid = d.descriptor_uuid; recovery.descriptor_generation = 1;
    if (!Issue(&recovery.statement_savepoint_profile_uuid)) return refuse("MGA_profile_identity_unavailable");
    recovery.statement_savepoint_profile_generation = 1;
    recovery.durable_registry_uuid = b.bundle_uuid; recovery.durable_registry_generation = 1;
    w::TypedDeleteCarrierError error;
    std::vector<std::uint8_t> bytes;
    if (!w::EncodeTypedDeleteDescriptor(d, &bytes, &error) ||
        !w::DecodeAndValidateTypedDeleteDescriptor(bytes, &d, &error)) return refuse("descriptor_encoding_failed");
    const auto datatype = CaptureDmlDeleteDatatypeAuthorityV1({context, d.exact_bytes, b.predicate.exact_bytes});
    if (!datatype.ok) { result.diagnostic = datatype.diagnostic; return result; }
    b.datatypes = datatype.datatypes; b.operators = datatype.operators;
    if (!ComputeDmlDeleteOwnerContextHashV1(context, &b.owner_context_sha256)) return refuse("owner_binding_unavailable");
    const auto binding = CaptureDmlDeleteBindingAuthorityV1(context, b, datatype, security, effects, grant.resource);
    if (!binding.ok) { result.diagnostic = binding.diagnostic; return result; }
    // Everything needed for the response is prepared before durable bound.
    result.descriptor_ref = {d.descriptor_uuid, d.descriptor_generation};
    const auto published = PublishDmlDeleteBoundAuthorityV1(context, binding.handle, grant.resource);
    grant.handed_off = published.resource_ownership_taken;
    result.ok = published.ok; result.diagnostic = published.diagnostic;
    return result;
  } catch (...) {
    result.diagnostic = std::move(interrupted); return result;
  }
}
}  // namespace scratchbird::engine::internal_api
