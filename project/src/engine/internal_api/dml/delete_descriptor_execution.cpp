// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_candidate_mutation.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "transaction/transaction_api.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "core/platform/savepoint_crash_injection.hpp"
#include <atomic>

namespace scratchbird::engine::internal_api {
namespace {
namespace w = scratchbird::wire;
using State = w::TypedUpdateJournalState;
using Disposition = EngineDmlDeleteRecoveryDispositionV1;
using Fault = EngineDmlDeleteRowsTestFaultPointV1;
std::atomic<Fault> fault{Fault::none};
bool FaultAt(Fault point) { return fault.compare_exchange_strong(point, Fault::none); }
EngineApiDiagnostic Error(const char* detail, const char* code = "DML.DELETE_FAILED") {
  return MakeEngineApiDiagnostic(code, "sblr.dml_delete_rows.execution_failed", detail, true);
}
void Number(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (unsigned n = 0; n < 8; ++n) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * n)));
}
bool AppendHash(std::vector<std::uint8_t>& bytes, const std::string& hash) {
  if (hash.size() != 71 || !hash.starts_with("sha256:")) return false;
  const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (unsigned n = 0; n < 32; ++n) {
    const auto hi = digit(hash[7 + n * 2]), lo = digit(hash[8 + n * 2]);
    if (hi < 0 || lo < 0) return false;
    bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
  }
  return true;
}
bool PrepareResult(const DmlDeleteDurableAuthorityBundleV1& b, std::uint64_t marker_generation,
    const EngineDmlDeleteCandidateMutationV1& mutation, w::TypedDeleteResultCarrier* result) {
  const auto& d = b.descriptor; auto& r = *result;
  r.delete_descriptor_uuid = d.descriptor_uuid; r.delete_descriptor_generation = d.descriptor_generation;
  r.operation_uuid = d.operation_uuid; r.owning_transaction_uuid = d.owning_transaction_uuid;
  r.owning_local_transaction_id = d.owning_local_transaction_id;
  r.relation_uuid = d.target_relation_uuid; r.relation_generation = d.target_relation_generation;
  r.matched_count = mutation.result.matched_count; r.deleted_count = mutation.result.deleted_count;
  r.effect_set_sha256 = mutation.effect_set_sha256;
  r.publication_barrier_uuid = b.reserved_statement_savepoint_uuid; r.publication_barrier_generation = marker_generation;
  constexpr std::string_view domain = "ScratchBird.DmlDelete.ExecutorResult.V1";
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), d.descriptor_evidence_sha256.begin(), d.descriptor_evidence_sha256.end());
  if (!AppendHash(material, b.executor.row_identity_sha256) || !AppendHash(material, b.executor.decision_evidence_sha256)) return false;
  material.insert(material.end(), r.effect_set_sha256.begin(), r.effect_set_sha256.end());
  material.insert(material.end(), r.publication_barrier_uuid.begin(), r.publication_barrier_uuid.end());
  Number(material, marker_generation); Number(material, r.matched_count); Number(material, r.deleted_count);
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(material);
  if (!hash.ok()) return false;
  r.executor_evidence_sha256 = hash.digest;
  w::TypedDeleteCarrierError error;
  std::vector<std::uint8_t> bytes;
  return w::EncodeTypedDeleteResult(r, &bytes, &error) && w::DecodeAndValidateTypedDeleteResult(bytes, &r, &error);
}
EngineDmlDeleteRowsExecuteResultV1 Replay(const EngineRequestContext& context,
    const w::TypedDeleteResultCarrier& prior) {
  EngineDmlDeleteRowsExecuteResultV1 result;
  result.ok = result.immutable_replay = result.delete_result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.canonical_result_bytes = prior.exact_bytes;
  result.delete_result.operation_id = "dml.delete_rows";
  result.delete_result.transaction_uuid = context.transaction_uuid;
  result.delete_result.local_transaction_id = context.local_transaction_id;
  result.delete_result.matched_count = prior.matched_count; result.delete_result.deleted_count = prior.deleted_count;
  result.delete_result.dml_summary.rows_changed = prior.deleted_count;
  result.delete_result.evidence.push_back({"dml_delete_rows_replay", "exact_durable_DDRS_no_rescan"});
  return result;
}
}  // namespace
void SetDmlDeleteRowsTestFaultPointV1(EngineDmlDeleteRowsTestFaultPointV1 point) { fault.store(point); }

EngineDmlDeleteRowsExecuteResultV1 ExecuteDmlDeleteRowsDescriptorV1(
    const EngineRequestContext& context, const EngineDmlDeleteRowsDescriptorRefV1& reference,
    std::uint64_t occurrence) {
  EngineDmlDeleteRowsExecuteResultV1 failure;
  failure.diagnostic = Error("interrupted_execution_requires_recovery");
  auto allocation_failure = Error("allocation_failed_before_publication", "RESOURCE.BUDGET_EXCEEDED");
  auto injected_failure = Error("injected_interruption_requires_recovery");
  std::unique_ptr<DmlDeleteExecutionLeaseV1> lease;
  const auto recover_failure = [&](EngineApiDiagnostic diagnostic) {
    try {
      if (lease) {
        const auto recovered = lease->Reconcile();
        if (!recovered.ok) diagnostic = Error("recovery_required_before_further_transaction_work");
        else if (recovered.disposition == Disposition::published) {
          // A lost publication acknowledgement is never a rollback. Returning
          // the already prepared result requires current replay authority.
          const auto validation = lease->RevalidateRecovered(context);
          if (!validation.error && recovered.head.prior_result) return Replay(context, *recovered.head.prior_result);
          diagnostic = validation.error ? validation : Error("published_result_unavailable");
        }
      }
      failure.diagnostic = std::move(diagnostic);
    } catch (...) {} // Keep original grant/ticket and a retryable diagnostic.
    return failure;
  };
  try {
    lease = AcquireDmlDeleteExecutionLeaseV1(context, reference.descriptor_uuid,
        reference.descriptor_generation, occurrence, &failure.diagnostic);
    if (!lease) return failure;
    // All engine append/finality routes use this same recursive guard; the
    // routed server owns cross-process exclusion. Hold it from source/version
    // validation through the durable statement barrier and terminal successor.
    const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
    auto observed = ObserveDmlDeleteRecoveryAuthorityV1(context, lease->store().chain());
    if (!observed.ok) return recover_failure(observed.diagnostic);
    const auto& bundle = lease->bundle();
    const auto marker_key = MgaSavepointUuidKey(datatype_operator_projection::UuidText(bundle.reserved_statement_savepoint_uuid));
    const auto prior_marker = ObserveUniqueMgaSavepointMarkerV1(context, marker_key);
    if (!prior_marker.ok) return recover_failure(prior_marker.diagnostic);
    if (!lease->has_live_binding() || observed.head.lifecycle_state != State::bound ||
        prior_marker.lifecycle != MgaSavepointMarkerLifecycle::missing) {
      const auto recovered = lease->Reconcile();
      if (!recovered.ok) { failure.diagnostic = recovered.diagnostic; return failure; }
      if (recovered.disposition != Disposition::published || !recovered.head.prior_result) {
        failure.diagnostic = Error("operation_was_aborted_not_reexecuted", "MGA.TRANSACTION.STALE"); return failure;
      }
      const auto validation = lease->RevalidateRecovered(context);
      if (validation.error) { failure.diagnostic = validation; return failure; }
      return Replay(context, *recovered.head.prior_result);
    }
    auto diagnostic = lease->RevalidateLive(context);
    if (diagnostic.error) return recover_failure(std::move(diagnostic));
    diagnostic = CreateMgaSavepointMarker(context, marker_key);
    if (diagnostic.error) return recover_failure(std::move(diagnostic));
    if (FaultAt(Fault::after_native_marker)) { failure.diagnostic = std::move(injected_failure); return failure; }
    scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary("delete_after_native_marker", context.local_transaction_id);
    const auto marker = ObserveUniqueMgaSavepointMarkerV1(context, marker_key);
    if (!marker.ok || !marker.creation_ordinal || marker.lifecycle != MgaSavepointMarkerLifecycle::active || marker.rolled_back)
      return recover_failure(Error("native_statement_boundary_unavailable"));
    auto intent = observed.head;
    intent.lifecycle_state = State::intent; ++intent.journal_sequence;
    intent.prior_record_sha256 = observed.head.record_evidence_sha256;
    intent.statement_savepoint_uuid = bundle.reserved_statement_savepoint_uuid;
    intent.statement_savepoint_generation = marker.creation_ordinal;
    if (!lease->store().Append(intent, &diagnostic)) return recover_failure(std::move(diagnostic));
    if (FaultAt(Fault::after_durable_intent)) { failure.diagnostic = std::move(injected_failure); return failure; }
    scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary("delete_after_intent", context.local_transaction_id);
    auto mutation = ExecuteDmlDeleteCandidateMutationV1(context, *lease);
    if (!mutation.ok) return recover_failure(std::move(mutation.diagnostic));
    observed = ObserveDmlDeleteRecoveryAuthorityV1(context, lease->store().chain());
    if (!observed.ok || observed.head.lifecycle_state != State::intent)
      return recover_failure(Error("intent_changed_during_mutation"));
    auto prepared = observed.head;
    prepared.lifecycle_state = State::prepared; ++prepared.journal_sequence;
    prepared.prior_record_sha256 = observed.head.record_evidence_sha256;
    if (!PrepareResult(bundle, marker.creation_ordinal, mutation, &prepared.prior_result.emplace()))
      return recover_failure(Error("result_preparation_failed"));
    // Construct the complete outward result before any publication barrier.
    EngineDmlDeleteRowsExecuteResultV1 success;
    success.ok = true; success.delete_result = std::move(mutation.result);
    success.delete_result.transaction_uuid = context.transaction_uuid;
    success.delete_result.local_transaction_id = context.local_transaction_id;
    success.canonical_result_bytes = prepared.prior_result->exact_bytes;
    success.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    if (!lease->store().Append(prepared, &diagnostic) || !lease->store().StagePublication(&diagnostic))
      return recover_failure(std::move(diagnostic));
    if (FaultAt(Fault::after_prepared_outcome)) { failure.diagnostic = std::move(injected_failure); return failure; }
    scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary("delete_after_prepared", context.local_transaction_id);
    diagnostic = lease->RevalidateLive(context);
    if (diagnostic.error) return recover_failure(std::move(diagnostic));
    const auto publication = lease->PublishAndRelease();
    if (publication != MgaDmlDeletePublicationStatusV1::published)
      return recover_failure(Error("publication_acknowledgement_uncertain"));
    if (FaultAt(Fault::after_publication_barrier)) { failure.diagnostic = std::move(injected_failure); return failure; }
    scratchbird::core::platform::MaybeCrashAtMgaSavepointBoundary("delete_after_barrier", context.local_transaction_id);
    return success;
  } catch (const std::bad_alloc&) {
    return recover_failure(std::move(allocation_failure));
  } catch (...) {
    return recover_failure(std::move(failure.diagnostic));
  }
}
}  // namespace scratchbird::engine::internal_api
