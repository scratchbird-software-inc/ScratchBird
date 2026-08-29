// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_policy_catalog_authority_provider.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace update_wire = scratchbird::wire;

std::atomic<std::uint64_t> g_policy_source_identity_ordinal{1};

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

std::string FreshUuid() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object,
      now + g_policy_source_identity_ordinal.fetch_add(
                1, std::memory_order_relaxed));
  return generated.ok()
      ? scratchbird::core::uuid::UuidToString(generated.value.value)
      : std::string{};
}

bool RawSha256(std::span<const std::uint8_t> bytes,
               update_wire::TypedUpdateHash* out) {
  if (out == nullptr) return false;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes.data(), bytes.size());
  if (!digest.ok()) return false;
  *out = digest.digest;
  return std::any_of(out->begin(), out->end(),
                     [](std::uint8_t value) { return value != 0; });
}

EngineApiDiagnostic ValidateCaptureRequest(
    const EngineDmlUpdatePolicyCatalogCaptureRequestV1& request) {
  const auto& context = request.context;
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !HasTraceTag(context, "private_dml_update_rows_binder")) {
    return Diagnostic("SECURITY.ACCESS_DENIED",
                      "sblr.dml_update_rows.policy_catalog_provider_denied");
  }
  if (context.read_only_mode || context.cluster_transaction_active ||
      context.route_fence_present || context.local_transaction_id == 0 ||
      !ExactUuid(context.transaction_uuid.canonical) ||
      !ExactUuid(context.statement_snapshot_uuid.canonical) ||
      !ExactUuid(request.authenticated_statement_receipt_uuid) ||
      request.authenticated_statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      request.structural_occurrence_id == 0 ||
      !ExactUuid(request.relation_occurrence.relation_uuid) ||
      request.relation_occurrence.relation_generation == 0 ||
      !ExactUuid(request.relation_occurrence.relation_occurrence_uuid) ||
      request.relation_occurrence.relation_occurrence_generation != 1 ||
      request.relation_occurrence.relation_uuid ==
          request.relation_occurrence.relation_occurrence_uuid ||
      !ExactUuid(request.catalog_snapshot_uuid) ||
      request.catalog_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      request.catalog_generation == 0 ||
      request.catalog_generation != context.catalog_generation_id ||
      !ExactUuid(request.descriptor_uuid) ||
      request.descriptor_generation != 1) {
    return Diagnostic("SBLR.OPERAND_INVALID",
                      "sblr.dml_update_rows.policy_catalog_capture_invalid");
  }
  return Ok();
}

EngineApiDiagnostic CarrierDiagnostic(
    const update_wire::TypedUpdateCarrierError& error,
    std::string_view fallback) {
  return Diagnostic(error.diagnostic_code.empty()
                        ? "SBLR.OPERAND_INVALID"
                        : error.diagnostic_code,
                    std::string(fallback),
                    error.field + ":" + error.detail);
}

bool SamePhaseProjection(
    const EngineDmlUpdateRowPolicyAuthoritySourceV1& left,
    const EngineDmlUpdateRowPolicyAuthoritySourceV1& right) {
  return left.effective_policy_uuid == right.effective_policy_uuid &&
         left.effective_policy_generation ==
             right.effective_policy_generation &&
         left.expression_uuid == right.expression_uuid &&
         left.expression_generation == right.expression_generation &&
         left.expression_evidence_sha256 ==
             right.expression_evidence_sha256;
}

}  // namespace

EngineDmlUpdatePolicyCatalogCaptureResultV1
CaptureDmlUpdatePolicyCatalogAuthorityV1(
    const EngineDmlUpdatePolicyCatalogCaptureRequestV1& request) {
  EngineDmlUpdatePolicyCatalogCaptureResultV1 result;
  result.diagnostic = ValidateCaptureRequest(request);
  if (result.diagnostic.error) return result;

  const auto security = IssueEngineSecurityPolicySnapshotAuthorityV1(
      request.context, request.relation_occurrence.relation_uuid);
  if (!security.ok) {
    result.diagnostic = security.diagnostic;
    return result;
  }

  const std::string vector_uuid = FreshUuid();
  update_wire::TypedUpdateSecurityPolicySourceVector vector;
  if (!TypedUuid(vector_uuid, &vector.identity.vector_uuid) ||
      !TypedUuid(request.descriptor_uuid,
                 &vector.identity.owner_descriptor_uuid)) {
    result.diagnostic = Diagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.policy_source_identity_issue_failed");
    return result;
  }
  vector.identity.vector_generation = 1;
  vector.identity.owner_descriptor_generation =
      request.descriptor_generation;
  vector.records.reserve(security.snapshot.admitted_policy_rows.size());
  result.immutable_policy_sources.reserve(
      security.snapshot.admitted_policy_rows.size());
  for (std::size_t index = 0;
       index < security.snapshot.admitted_policy_rows.size(); ++index) {
    const auto& admitted = security.snapshot.admitted_policy_rows[index];
    update_wire::TypedUpdateSecurityPolicySourceRecord row;
    row.source_policy_ordinal = static_cast<std::uint32_t>(index + 1);
    row.phase = admitted.phase;
    row.source_state = 1;
    if (!TypedUuid(admitted.policy_uuid, &row.policy_uuid) ||
        !TypedUuid(admitted.policy_version_uuid,
                   &row.policy_version_uuid) ||
        !TypedUuid(admitted.target_relation_uuid,
                   &row.target_relation_uuid) ||
        !TypedUuid(admitted.source_expression_uuid,
                   &row.source_expression_uuid) ||
        !TypedUuid(admitted.catalog_snapshot_uuid,
                   &row.catalog_snapshot_uuid) ||
        !TypedUuid(security.snapshot.snapshot_uuid,
                   &row.security_snapshot_uuid)) {
      result.diagnostic = Diagnostic(
          "SECURITY.ACCESS_DENIED",
          "sblr.dml_update_rows.policy_source_identity_invalid");
      return result;
    }
    row.policy_generation = admitted.policy_generation;
    row.effective_transaction_number =
        admitted.effective_transaction_number;
    row.target_relation_generation = admitted.target_relation_generation;
    row.source_expression_generation =
        admitted.source_expression_generation;
    row.source_expression_evidence_sha256 =
        admitted.source_expression_evidence_sha256;
    row.catalog_generation = admitted.catalog_generation;
    row.security_snapshot_generation =
        security.snapshot.snapshot_generation;
    vector.records.push_back(std::move(row));

    EngineDmlUpdateRowPolicyAuthoritySourceV1 source;
    source.target_relation_uuid = admitted.target_relation_uuid;
    source.target_relation_generation = admitted.target_relation_generation;
    source.source_policy_uuid = admitted.policy_uuid;
    source.source_policy_generation = admitted.policy_generation;
    source.source_policy_version_uuid = admitted.policy_version_uuid;
    source.effective_transaction_number =
        admitted.effective_transaction_number;
    source.phase = static_cast<EngineDmlUpdateRowPolicyPhaseV1>(
        admitted.phase);
    source.effective_policy_uuid = admitted.effective_policy_uuid;
    source.effective_policy_generation =
        admitted.effective_policy_generation;
    source.source_expression_uuid = admitted.source_expression_uuid;
    source.source_expression_generation =
        admitted.source_expression_generation;
    source.source_expression_evidence_sha256 =
        admitted.source_expression_evidence_sha256;
    source.expression_uuid = admitted.effective_expression_uuid;
    source.expression_generation =
        admitted.effective_expression_generation;
    source.expression_evidence_sha256 =
        admitted.effective_expression_evidence_sha256;
    source.catalog_snapshot_uuid = admitted.catalog_snapshot_uuid;
    source.catalog_generation = admitted.catalog_generation;
    source.security_snapshot_uuid = security.snapshot.snapshot_uuid;
    source.security_snapshot_generation =
        security.snapshot.snapshot_generation;
    source.security_generation = admitted.security_generation;
    source.policy_catalog_generation = security.snapshot.policy_generation;
    result.immutable_policy_sources.push_back(std::move(source));
  }

  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> exact;
  if (!update_wire::EncodeTypedUpdateSecurityPolicySourceVector(
          vector, &exact, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          exact, &vector, &error)) {
    result.diagnostic = CarrierDiagnostic(
        error, "sblr.dml_update_rows.policy_source_vector_invalid");
    return result;
  }
  for (auto& source : result.immutable_policy_sources) {
    source.source_policy_catalog_vector_sha256 =
        vector.identity.vector_sha256;
  }

  result.ok = true;
  result.diagnostic = Ok();
  result.security_policy_snapshot = security.snapshot;
  result.source_policy_vector = vector;
  result.exact_source_policy_vector_dusv = std::move(exact);
  return result;
}

EngineDmlUpdateSecuritySnapshotProofResultV1
BuildDmlUpdateSecuritySnapshotProofV1(
    const EngineDmlUpdateSecuritySnapshotProofRequestV1& request) {
  EngineDmlUpdateSecuritySnapshotProofResultV1 result;
  if (!request.captured.ok || request.exact_descriptor_dudc.empty() ||
      request.exact_row_policy_vector_dupv.empty()) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.security_snapshot_proof_input_invalid");
    return result;
  }
  update_wire::TypedUpdateCarrierError error;
  update_wire::TypedUpdateDescriptorCarrier descriptor;
  update_wire::TypedUpdateRowPolicyVector policies;
  update_wire::TypedUpdateSecurityPolicySourceVector sources;
  if (!update_wire::DecodeAndValidateTypedUpdateDescriptor(
          request.exact_descriptor_dudc, &descriptor, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          request.exact_row_policy_vector_dupv, &policies, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          request.captured.exact_source_policy_vector_dusv, &sources,
          &error)) {
    result.diagnostic = CarrierDiagnostic(
        error, "sblr.dml_update_rows.security_snapshot_proof_carrier_invalid");
    return result;
  }
  const auto& snapshot = request.captured.security_policy_snapshot;
  update_wire::TypedUpdateUuid snapshot_uuid{};
  update_wire::TypedUpdateUuid context_uuid{};
  update_wire::TypedUpdateUuid receipt_uuid{};
  update_wire::TypedUpdateUuid database_uuid{};
  if (!TypedUuid(snapshot.snapshot_uuid, &snapshot_uuid) ||
      !TypedUuid(snapshot.security_context_uuid, &context_uuid) ||
      !TypedUuid(snapshot.authenticated_statement_receipt_uuid,
                 &receipt_uuid) ||
      !TypedUuid(request.context.database_uuid.canonical,
                 &database_uuid) ||
      descriptor.authenticated_statement_receipt_uuid != receipt_uuid ||
      descriptor.security_snapshot_uuid != snapshot_uuid ||
      descriptor.security_context_uuid != context_uuid ||
      descriptor.security_generation != snapshot.security_generation ||
      descriptor.row_policy_set_uuid != policies.identity.vector_uuid ||
      descriptor.row_policy_set_generation !=
          policies.identity.vector_generation ||
      descriptor.row_policy_count != policies.records.size() ||
      descriptor.descriptor_uuid != sources.identity.owner_descriptor_uuid ||
      descriptor.descriptor_generation !=
          sources.identity.owner_descriptor_generation ||
      sources.records.size() !=
          request.captured.immutable_policy_sources.size()) {
    result.diagnostic = Diagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.security_snapshot_proof_binding_stale");
    return result;
  }
  for (std::size_t index = 0; index < policies.records.size(); ++index) {
    const auto& policy = policies.records[index];
    if (policy.policy_ordinal != index + 1 ||
        policy.security_snapshot_uuid != snapshot_uuid ||
        policy.security_generation != snapshot.security_generation ||
        policy.source_policy_catalog_vector_sha256 !=
            sources.identity.vector_sha256) {
      result.diagnostic = Diagnostic(
          "MGA.TRANSACTION.STALE",
          "sblr.dml_update_rows.security_snapshot_effective_policy_stale");
      return result;
    }
    const EngineDmlUpdateRowPolicyAuthoritySourceV1* phase_source = nullptr;
    for (const auto& source : request.captured.immutable_policy_sources) {
      if (static_cast<std::uint8_t>(source.phase) != policy.phase) continue;
      if (phase_source == nullptr) {
        phase_source = &source;
      } else if (!SamePhaseProjection(*phase_source, source)) {
        result.diagnostic = Diagnostic(
            "SBLR.OPERAND_INVALID",
            "sblr.dml_update_rows.security_snapshot_phase_conflict");
        return result;
      }
    }
    update_wire::TypedUpdateUuid effective_policy{};
    update_wire::TypedUpdateUuid effective_expression{};
    if (phase_source == nullptr ||
        !TypedUuid(phase_source->effective_policy_uuid,
                   &effective_policy) ||
        !TypedUuid(phase_source->expression_uuid,
                   &effective_expression) ||
        policy.effective_policy_uuid != effective_policy ||
        policy.effective_policy_generation !=
            phase_source->effective_policy_generation ||
        policy.expression_uuid != effective_expression ||
        policy.expression_generation !=
            phase_source->expression_generation ||
        policy.expression_evidence_sha256 !=
            phase_source->expression_evidence_sha256) {
      result.diagnostic = Diagnostic(
          "MGA.TRANSACTION.STALE",
          "sblr.dml_update_rows.security_snapshot_effective_projection_stale");
      return result;
    }
  }
  for (const auto& source : request.captured.immutable_policy_sources) {
    const auto phase = static_cast<std::uint8_t>(source.phase);
    const auto matching = std::count_if(
        policies.records.begin(), policies.records.end(),
        [phase](const update_wire::TypedUpdateRowPolicyRecord& policy) {
          return policy.phase == phase;
        });
    if (matching != 1) {
      result.diagnostic = Diagnostic(
          "SBLR.OPERAND_INVALID",
          "sblr.dml_update_rows.security_snapshot_phase_projection_invalid");
      return result;
    }
  }

  update_wire::TypedUpdateSecuritySnapshotProof proof;
  proof.security_snapshot_uuid = snapshot_uuid;
  proof.security_snapshot_generation = snapshot.snapshot_generation;
  proof.security_context_uuid = context_uuid;
  proof.security_context_generation = snapshot.security_context_generation;
  proof.security_epoch = snapshot.security_generation;
  proof.policy_generation = snapshot.policy_generation;
  proof.database_uuid = database_uuid;
  proof.authenticated_statement_receipt_uuid =
      descriptor.authenticated_statement_receipt_uuid;
  proof.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  proof.owning_local_transaction_id =
      descriptor.owning_local_transaction_id;
  proof.operation_uuid = descriptor.operation_uuid;
  proof.operation_generation = descriptor.operation_generation;
  proof.recovery_token_uuid = descriptor.recovery_token_uuid;
  proof.recovery_generation = descriptor.recovery_generation;
  proof.statement_snapshot_uuid = descriptor.statement_snapshot_uuid;
  proof.catalog_snapshot_uuid = descriptor.catalog_snapshot_uuid;
  proof.catalog_generation = descriptor.catalog_generation;
  proof.policy_catalog_epoch = snapshot.policy_generation;
  proof.target_relation_uuid = descriptor.target_relation_uuid;
  proof.target_relation_generation = descriptor.target_relation_generation;
  proof.target_relation_occurrence_uuid =
      descriptor.target_relation_occurrence_uuid;
  proof.target_relation_occurrence_generation =
      descriptor.target_relation_occurrence_generation;
  proof.descriptor_uuid = descriptor.descriptor_uuid;
  proof.descriptor_generation = descriptor.descriptor_generation;
  proof.row_policy_set_uuid = policies.identity.vector_uuid;
  proof.row_policy_set_generation = policies.identity.vector_generation;
  proof.source_policy_vector_uuid = sources.identity.vector_uuid;
  proof.source_policy_vector_generation = sources.identity.vector_generation;
  proof.row_policy_count = static_cast<std::uint32_t>(policies.records.size());
  proof.source_policy_count = static_cast<std::uint32_t>(sources.records.size());
  proof.snapshot_state = 1;
  proof.descriptor_evidence_sha256 = descriptor.descriptor_evidence_sha256;
  proof.row_policy_set_sha256 = descriptor.row_policy_set_sha256;
  if (!RawSha256(request.exact_descriptor_dudc,
                 &proof.exact_dudc_sha256) ||
      !RawSha256(request.exact_row_policy_vector_dupv,
                 &proof.exact_dupv_sha256)) {
    result.diagnostic = Diagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.security_snapshot_proof_hash_failed");
    return result;
  }
  proof.source_policy_catalog_vector_sha256 =
      sources.identity.vector_sha256;
  std::vector<std::uint8_t> exact;
  if (!update_wire::EncodeTypedUpdateSecuritySnapshotProof(
          proof, &exact, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          exact, &proof, &error)) {
    result.diagnostic = CarrierDiagnostic(
        error, "sblr.dml_update_rows.security_snapshot_proof_invalid");
    return result;
  }
  result.ok = true;
  result.diagnostic = Ok();
  result.proof = proof;
  result.exact_security_snapshot_proof_dusp = std::move(exact);
  return result;
}

}  // namespace scratchbird::engine::internal_api
