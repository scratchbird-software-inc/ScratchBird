// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_provider_maintenance.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::diagnostic_invalid_record,
          Severity::error,
          Subsystem::engine};
}

void Add(std::vector<std::string>* values, std::string value) {
  values->push_back(std::move(value));
}

void AddBool(std::vector<std::string>* values,
             const std::string& key,
             bool value) {
  Add(values, key + "=" + (value ? "true" : "false"));
}

bool ProofAuthoritySafe(const VectorProviderMaintenanceProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.reference_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool ProofComplete(const VectorProviderMaintenanceProof& proof) {
  return proof.proof_supplied && proof.exact_source_available &&
         proof.exact_recheck_proof_supplied &&
         proof.mga_recheck_proof_supplied &&
         proof.security_recheck_proof_supplied &&
         proof.candidate_only_non_authority && !proof.evidence_ref.empty() &&
         ProofAuthoritySafe(proof);
}

bool PublishProofComplete(const VectorProviderMaintenanceProof& proof) {
  return ProofComplete(proof) && proof.validation_successful &&
         proof.generation_advanced;
}

bool ProviderAuthoritySafe(const VectorExactPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

bool ProviderAuthoritySafe(const VectorHnswPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

bool ProviderAuthoritySafe(const VectorIvfPqPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

void AddCommonEvidence(std::vector<std::string>* evidence,
                       VectorProviderMaintenanceKind provider_kind,
                       const VectorProviderMaintenanceContext& context) {
  Add(evidence, kVectorProviderMaintenanceSearchKey);
  Add(evidence,
      "provider_kind=" +
          std::string(VectorProviderMaintenanceKindName(provider_kind)));
  Add(evidence, "collection_uuid=" + context.collection_uuid);
  Add(evidence, "index_uuid=" + context.index_uuid);
  Add(evidence, "provider_uuid=" + context.provider_uuid);
  Add(evidence,
      "expected_provider_generation=" +
          std::to_string(context.expected_provider_generation));
  Add(evidence,
      "expected_descriptor_epoch=" +
          std::to_string(context.expected_descriptor_epoch));
  Add(evidence,
      "expected_metric_resource_epoch=" +
          std::to_string(context.expected_metric_resource_epoch));
  AddBool(evidence, "candidate_only_non_authority",
          context.proof.candidate_only_non_authority);
  AddBool(evidence, "exact_source_available",
          context.proof.exact_source_available);
  AddBool(evidence, "mga_recheck_proof_supplied",
          context.proof.mga_recheck_proof_supplied);
  AddBool(evidence, "security_recheck_proof_supplied",
          context.proof.security_recheck_proof_supplied);
}

void AddSupport(std::vector<std::string>* rows,
                std::string key,
                std::string value) {
  rows->push_back(kVectorProviderMaintenanceSearchKey + std::string("|") +
                  std::move(key) + "|" + std::move(value));
}

VectorProviderValidationResult RefuseValidation(
    VectorProviderMaintenanceKind kind,
    const VectorProviderMaintenanceContext& context,
    std::string code,
    std::string detail,
    VectorProviderRepairClass repair = VectorProviderRepairClass::none) {
  VectorProviderValidationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, std::move(code),
      "index.vector_provider_maintenance.refused", detail);
  result.provider_kind = kind;
  result.fail_closed = true;
  result.repair_class = repair;
  result.repair_reason = detail;
  result.restricted_repair_required =
      repair != VectorProviderRepairClass::none;
  AddCommonEvidence(&result.evidence, kind, context);
  Add(&result.evidence, "validation_result=refused");
  if (result.restricted_repair_required) {
    Add(&result.evidence,
        "restricted_repair_reason=" +
            std::string(VectorProviderRepairClassName(repair)));
    AddSupport(&result.support_bundle_rows, "restricted_repair_reason",
               VectorProviderRepairClassName(repair));
  }
  return result;
}

VectorProviderMaintenanceResult RefuseJob(
    VectorProviderMaintenanceJob job,
    std::string code,
    std::string detail,
    VectorProviderMaintenanceFailureClass failure_class) {
  job.state = VectorProviderMaintenanceJobState::refused;
  job.failure_class = failure_class;
  Add(&job.evidence, "job_state=refused");
  Add(&job.evidence,
      "failure_class=" +
          std::string(VectorProviderMaintenanceFailureClassName(failure_class)));
  AddSupport(&job.support_bundle_rows, "refusal", detail);
  VectorProviderMaintenanceResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, std::move(code),
      "index.vector_provider_maintenance.job_refused", std::move(detail));
  result.job = std::move(job);
  result.fail_closed = true;
  return result;
}

VectorProviderMaintenanceResult FinishJob(VectorProviderMaintenanceJob job) {
  VectorProviderMaintenanceResult result;
  result.status = OkStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, "INDEX.VECTOR_PROVIDER_MAINTENANCE.OK",
      "index.vector_provider_maintenance.ok");
  result.job = std::move(job);
  result.fail_closed = false;
  return result;
}

std::string JoinPolicyReasons(const std::vector<std::string>& reasons) {
  std::ostringstream out;
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    if (i != 0) out << ',';
    out << reasons[i];
  }
  return out.str();
}

VectorProviderMaintenanceJob BaseJob(
    const VectorProviderMaintenanceContext& context,
    VectorProviderMaintenanceKind provider_kind,
    VectorProviderMaintenanceJobKind job_kind,
    u64 new_provider_generation,
    u64 new_training_generation) {
  VectorProviderMaintenanceJob job;
  job.job_id = DeterministicVectorProviderMaintenanceJobId(
      context, provider_kind, job_kind, new_provider_generation,
      new_training_generation);
  job.provider_kind = provider_kind;
  job.job_kind = job_kind;
  job.state = VectorProviderMaintenanceJobState::scheduled;
  job.collection_uuid = context.collection_uuid;
  job.index_uuid = context.index_uuid;
  job.provider_uuid = context.provider_uuid;
  job.old_provider_generation = context.expected_provider_generation;
  job.new_provider_generation = new_provider_generation;
  job.old_training_generation = context.expected_training_generation;
  job.new_training_generation = new_training_generation;
  job.max_retry_attempts = context.policy.max_retry_attempts;
  job.candidate_only_non_authority = context.proof.candidate_only_non_authority;
  AddCommonEvidence(&job.evidence, provider_kind, context);
  Add(&job.evidence,
      "job_kind=" +
          std::string(VectorProviderMaintenanceJobKindName(job_kind)));
  Add(&job.evidence, "job_id=" + job.job_id);
  Add(&job.evidence,
      "new_provider_generation=" + std::to_string(new_provider_generation));
  Add(&job.evidence,
      "new_training_generation=" + std::to_string(new_training_generation));
  AddSupport(&job.support_bundle_rows, "job_id", job.job_id);
  return job;
}

bool ContextMatchesExact(const VectorExactPhysicalProvider& provider,
                         const VectorProviderMaintenanceContext& context) {
  return provider.relation_uuid == context.collection_uuid &&
         provider.index_uuid == context.index_uuid &&
         provider.provider_uuid == context.provider_uuid &&
         provider.provider_generation == context.expected_provider_generation &&
         provider.descriptor.descriptor_epoch ==
             context.expected_descriptor_epoch &&
         provider.metric.metric_resource_epoch ==
             context.expected_metric_resource_epoch;
}

bool ContextMatchesHnsw(const VectorHnswPhysicalProvider& provider,
                        const VectorProviderMaintenanceContext& context) {
  return provider.relation_uuid == context.collection_uuid &&
         provider.index_uuid == context.index_uuid &&
         provider.provider_uuid == context.provider_uuid &&
         provider.provider_generation == context.expected_provider_generation &&
         provider.training_generation == context.expected_training_generation &&
         provider.descriptor.descriptor_epoch ==
             context.expected_descriptor_epoch &&
         provider.metric.metric_resource_epoch ==
             context.expected_metric_resource_epoch;
}

bool ContextMatchesIvfPq(const VectorIvfPqPhysicalProvider& provider,
                         const VectorProviderMaintenanceContext& context) {
  return provider.relation_uuid == context.collection_uuid &&
         provider.index_uuid == context.index_uuid &&
         provider.provider_uuid == context.provider_uuid &&
         provider.provider_generation == context.expected_provider_generation &&
         provider.training_generation == context.expected_training_generation &&
         provider.descriptor.descriptor_epoch ==
             context.expected_descriptor_epoch &&
         provider.metric.metric_resource_epoch ==
             context.expected_metric_resource_epoch;
}

VectorProviderMaintenanceContext CandidateContext(
    VectorProviderMaintenanceContext context,
    u64 provider_generation,
    u64 training_generation) {
  context.expected_provider_generation = provider_generation;
  context.expected_training_generation = training_generation;
  return context;
}

VectorProviderRepairClass RepairClassFrom(VectorExactOpenClass open_class) {
  switch (open_class) {
    case VectorExactOpenClass::bad_checksum:
      return VectorProviderRepairClass::bad_checksum;
    case VectorExactOpenClass::corrupt_payload:
      return VectorProviderRepairClass::corrupt_payload;
    case VectorExactOpenClass::current:
    case VectorExactOpenClass::stale_format:
    case VectorExactOpenClass::stale_generation:
    case VectorExactOpenClass::identity_mismatch:
    case VectorExactOpenClass::stale_descriptor_epoch:
    case VectorExactOpenClass::stale_metric_epoch:
    case VectorExactOpenClass::dimension_mismatch:
    case VectorExactOpenClass::unsupported_element_profile:
    case VectorExactOpenClass::unsafe_metric_resource:
    case VectorExactOpenClass::missing_exact_recheck_proof:
    case VectorExactOpenClass::authority_claim_refused:
    case VectorExactOpenClass::refused:
      return VectorProviderRepairClass::none;
  }
  return VectorProviderRepairClass::none;
}

VectorProviderRepairClass RepairClassFrom(VectorHnswOpenClass open_class) {
  switch (open_class) {
    case VectorHnswOpenClass::bad_checksum:
      return VectorProviderRepairClass::bad_checksum;
    case VectorHnswOpenClass::corrupt_payload:
      return VectorProviderRepairClass::corrupt_payload;
    case VectorHnswOpenClass::invalid_graph:
      return VectorProviderRepairClass::invalid_graph;
    case VectorHnswOpenClass::current:
    case VectorHnswOpenClass::stale_format:
    case VectorHnswOpenClass::stale_generation:
    case VectorHnswOpenClass::identity_mismatch:
    case VectorHnswOpenClass::stale_descriptor_epoch:
    case VectorHnswOpenClass::stale_metric_epoch:
    case VectorHnswOpenClass::dimension_mismatch:
    case VectorHnswOpenClass::unsupported_element_profile:
    case VectorHnswOpenClass::unsafe_metric_resource:
    case VectorHnswOpenClass::missing_exact_recheck_proof:
    case VectorHnswOpenClass::authority_claim_refused:
    case VectorHnswOpenClass::refused:
      return VectorProviderRepairClass::none;
    case VectorHnswOpenClass::duplicate_row_locator:
      return VectorProviderRepairClass::restricted_repair_required;
  }
  return VectorProviderRepairClass::none;
}

VectorProviderRepairClass RepairClassFrom(VectorIvfPqOpenClass open_class) {
  switch (open_class) {
    case VectorIvfPqOpenClass::bad_checksum:
      return VectorProviderRepairClass::bad_checksum;
    case VectorIvfPqOpenClass::corrupt_payload:
      return VectorProviderRepairClass::corrupt_payload;
    case VectorIvfPqOpenClass::invalid_centroid:
      return VectorProviderRepairClass::invalid_centroid;
    case VectorIvfPqOpenClass::invalid_list:
      return VectorProviderRepairClass::invalid_list;
    case VectorIvfPqOpenClass::invalid_codebook:
      return VectorProviderRepairClass::invalid_codebook;
    case VectorIvfPqOpenClass::invalid_code:
      return VectorProviderRepairClass::invalid_code;
    case VectorIvfPqOpenClass::current:
    case VectorIvfPqOpenClass::stale_format:
    case VectorIvfPqOpenClass::stale_generation:
    case VectorIvfPqOpenClass::identity_mismatch:
    case VectorIvfPqOpenClass::stale_descriptor_epoch:
    case VectorIvfPqOpenClass::stale_metric_epoch:
    case VectorIvfPqOpenClass::dimension_mismatch:
    case VectorIvfPqOpenClass::unsupported_profile:
    case VectorIvfPqOpenClass::unsafe_metric_resource:
    case VectorIvfPqOpenClass::missing_exact_recheck_proof:
    case VectorIvfPqOpenClass::authority_claim_refused:
    case VectorIvfPqOpenClass::refused:
      return VectorProviderRepairClass::none;
    case VectorIvfPqOpenClass::duplicate_row_locator:
      return VectorProviderRepairClass::restricted_repair_required;
  }
  return VectorProviderRepairClass::none;
}

std::vector<float> ZeroVector(u32 dimensions) {
  return std::vector<float>(dimensions, 0.0F);
}

std::optional<std::vector<float>> FirstExactQueryVector(
    const VectorExactPhysicalProvider& provider) {
  if (provider.rows.empty()) return std::nullopt;
  const auto decoded =
      DecodeVectorExactPayload(provider.rows.front().encoded_payload,
                               provider.descriptor);
  if (decoded.size() != provider.descriptor.dimensions) return std::nullopt;
  return decoded;
}

std::optional<std::vector<float>> FirstHnswQueryVector(
    const VectorHnswPhysicalProvider& provider) {
  for (const auto& node : provider.nodes) {
    if (!node.tombstoned) {
      const auto decoded =
          DecodeVectorExactPayload(node.encoded_payload, provider.descriptor);
      if (decoded.size() == provider.descriptor.dimensions) return decoded;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<float>> FirstIvfPqQueryVector(
    const VectorIvfPqPhysicalProvider& provider) {
  for (const auto& list : provider.lists) {
    for (const auto& entry : list.entries) {
      if (!entry.tombstoned) {
        const auto decoded =
            DecodeVectorExactPayload(entry.exact_payload, provider.descriptor);
        if (decoded.size() == provider.descriptor.dimensions) return decoded;
      }
    }
  }
  return std::nullopt;
}

double HnswEffectiveTombstoneRatio(const VectorHnswPhysicalProvider& provider) {
  const u64 total = provider.live_node_count + provider.tombstone_count;
  if (total == 0) return 0.0;
  return provider.tombstone_ratio > 0.0
             ? provider.tombstone_ratio
             : static_cast<double>(provider.tombstone_count) /
                   static_cast<double>(total);
}

double IvfPqEffectiveTombstoneRatio(
    const VectorIvfPqPhysicalProvider& provider) {
  const u64 total = provider.live_vector_count + provider.tombstone_count;
  if (total == 0) return 0.0;
  return static_cast<double>(provider.tombstone_count) /
         static_cast<double>(total);
}

bool LatencyExceeded(u64 observed, const VectorProviderMaintenancePolicy& policy) {
  return policy.max_latency_units != 0 && observed > policy.max_latency_units;
}

void AppendValidationEvidence(VectorProviderValidationResult* result,
                              bool query_sanity_consumed,
                              bool empty_provider) {
  Add(&result->evidence, "serialize_open_path_consumed=true");
  AddBool(&result->evidence, "query_sanity_consumed",
          query_sanity_consumed);
  AddBool(&result->evidence, "empty_provider_validation", empty_provider);
  Add(&result->evidence, "candidate_only_non_authority=true");
  Add(&result->evidence, "validation_result=passed");
  AddSupport(&result->support_bundle_rows, "validation", "passed");
}

bool CanPublishCandidate(const VectorProviderMaintenanceJob& job) {
  return job.state == VectorProviderMaintenanceJobState::publish_ready &&
         job.validation_successful && job.publish_candidate_available &&
         job.candidate_only_non_authority &&
         job.new_provider_generation > job.old_provider_generation;
}

}  // namespace

const char* VectorProviderMaintenanceKindName(
    VectorProviderMaintenanceKind kind) {
  switch (kind) {
    case VectorProviderMaintenanceKind::exact: return "exact";
    case VectorProviderMaintenanceKind::hnsw: return "hnsw";
    case VectorProviderMaintenanceKind::ivf_pq: return "ivf_pq";
    case VectorProviderMaintenanceKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* VectorProviderMaintenanceJobKindName(
    VectorProviderMaintenanceJobKind kind) {
  switch (kind) {
    case VectorProviderMaintenanceJobKind::validate: return "validate";
    case VectorProviderMaintenanceJobKind::compact: return "compact";
    case VectorProviderMaintenanceJobKind::retrain: return "retrain";
    case VectorProviderMaintenanceJobKind::rebuild: return "rebuild";
    case VectorProviderMaintenanceJobKind::repair: return "repair";
    case VectorProviderMaintenanceJobKind::publish: return "publish";
    case VectorProviderMaintenanceJobKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* VectorProviderMaintenanceJobStateName(
    VectorProviderMaintenanceJobState state) {
  switch (state) {
    case VectorProviderMaintenanceJobState::created: return "created";
    case VectorProviderMaintenanceJobState::scheduled: return "scheduled";
    case VectorProviderMaintenanceJobState::running: return "running";
    case VectorProviderMaintenanceJobState::cancelled: return "cancelled";
    case VectorProviderMaintenanceJobState::validation_failed:
      return "validation_failed";
    case VectorProviderMaintenanceJobState::validation_passed:
      return "validation_passed";
    case VectorProviderMaintenanceJobState::publish_ready:
      return "publish_ready";
    case VectorProviderMaintenanceJobState::published: return "published";
    case VectorProviderMaintenanceJobState::failed: return "failed";
    case VectorProviderMaintenanceJobState::refused: return "refused";
  }
  return "refused";
}

const char* VectorProviderMaintenanceFailureClassName(
    VectorProviderMaintenanceFailureClass failure_class) {
  switch (failure_class) {
    case VectorProviderMaintenanceFailureClass::none: return "none";
    case VectorProviderMaintenanceFailureClass::transient_provider_unavailable:
      return "transient_provider_unavailable";
    case VectorProviderMaintenanceFailureClass::permanent_validation_failed:
      return "permanent_validation_failed";
    case VectorProviderMaintenanceFailureClass::stale_generation:
      return "stale_generation";
    case VectorProviderMaintenanceFailureClass::authority_boundary_refused:
      return "authority_boundary_refused";
    case VectorProviderMaintenanceFailureClass::unsafe_publish:
      return "unsafe_publish";
    case VectorProviderMaintenanceFailureClass::restricted_repair_required:
      return "restricted_repair_required";
    case VectorProviderMaintenanceFailureClass::invalid_state:
      return "invalid_state";
    case VectorProviderMaintenanceFailureClass::unsupported_provider:
      return "unsupported_provider";
    case VectorProviderMaintenanceFailureClass::missing_proof:
      return "missing_proof";
  }
  return "invalid_state";
}

const char* VectorProviderRepairClassName(VectorProviderRepairClass repair) {
  switch (repair) {
    case VectorProviderRepairClass::none: return "none";
    case VectorProviderRepairClass::bad_checksum: return "bad_checksum";
    case VectorProviderRepairClass::corrupt_payload: return "corrupt_payload";
    case VectorProviderRepairClass::invalid_graph: return "invalid_graph";
    case VectorProviderRepairClass::invalid_list: return "invalid_list";
    case VectorProviderRepairClass::invalid_codebook: return "invalid_codebook";
    case VectorProviderRepairClass::invalid_centroid: return "invalid_centroid";
    case VectorProviderRepairClass::invalid_code: return "invalid_code";
    case VectorProviderRepairClass::restricted_repair_required:
      return "restricted_repair_required";
  }
  return "restricted_repair_required";
}

VectorExactRecheckProof ToVectorExactRecheckProof(
    const VectorProviderMaintenanceProof& proof) {
  VectorExactRecheckProof recheck;
  recheck.proof_supplied = proof.proof_supplied;
  recheck.exact_source_vector_available = proof.exact_source_available;
  recheck.exact_rerank_proof_supplied = proof.exact_recheck_proof_supplied;
  recheck.mga_recheck_required = true;
  recheck.security_recheck_required = true;
  recheck.evidence_ref = proof.evidence_ref;
  recheck.parser_finality_authority_claimed =
      proof.parser_finality_authority_claimed;
  recheck.reference_finality_authority_claimed =
      proof.reference_finality_authority_claimed;
  recheck.provider_finality_authority_claimed =
      proof.provider_finality_authority_claimed;
  recheck.index_finality_authority_claimed =
      proof.index_finality_authority_claimed;
  recheck.write_ahead_log_finality_authority_claimed =
      proof.write_ahead_log_finality_authority_claimed;
  recheck.visibility_authority_claimed = proof.visibility_authority_claimed;
  recheck.security_authority_claimed = proof.security_authority_claimed;
  recheck.transaction_finality_authority_claimed =
      proof.transaction_finality_authority_claimed;
  return recheck;
}

std::string DeterministicVectorProviderMaintenanceJobId(
    const VectorProviderMaintenanceContext& context,
    VectorProviderMaintenanceKind provider_kind,
    VectorProviderMaintenanceJobKind job_kind,
    u64 new_provider_generation,
    u64 new_training_generation) {
  std::ostringstream out;
  out << "vector-maintenance:" << context.collection_uuid << ':'
      << context.index_uuid << ':' << context.provider_uuid << ':'
      << VectorProviderMaintenanceKindName(provider_kind) << ':'
      << VectorProviderMaintenanceJobKindName(job_kind) << ':'
      << context.expected_provider_generation << "->"
      << new_provider_generation << ':' << context.expected_training_generation
      << "->" << new_training_generation;
  return out.str();
}

VectorProviderValidationResult ValidateVectorExactProviderForMaintenance(
    const VectorExactPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context) {
  if (!ProofComplete(context.proof)) {
    return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.PROOF_REQUIRED",
                            "exact_source_mga_security_recheck_proof_required");
  }
  if (!ContextMatchesExact(provider, context)) {
    return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.STALE_EXACT",
                            "stale_provider_descriptor_or_metric_generation");
  }
  if (!ProviderAuthoritySafe(provider)) {
    return RefuseValidation(
        VectorProviderMaintenanceKind::exact, context,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.AUTHORITY_REFUSED",
        "provider_candidate_only_non_authority_required");
  }
  const auto serialized = SerializeVectorExactPhysicalProvider(provider);
  if (!serialized.ok()) {
    return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.SERIALIZE_FAILED",
                            serialized.diagnostic.diagnostic_code);
  }
  VectorExactOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = context.collection_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = context.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = context.provider_uuid;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = context.expected_provider_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = context.expected_descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = context.expected_metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = provider.descriptor.dimensions;
  open.recheck_proof = ToVectorExactRecheckProof(context.proof);
  const auto opened = OpenVectorExactPhysicalProvider(open);
  const auto repair = RepairClassFrom(opened.open_class);
  if (!opened.ok()) {
    return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                            opened.diagnostic.diagnostic_code,
                            VectorExactOpenClassName(opened.open_class),
                            repair);
  }
  const bool provider_empty = opened.provider.rows.empty();
  const auto query_vector = provider_empty
                                ? std::optional<std::vector<float>>(
                                      ZeroVector(opened.provider.descriptor.dimensions))
                                : FirstExactQueryVector(opened.provider);
  if (!query_vector) {
    return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.QUERY_REFUSED",
                            "exact_query_sanity_vector_required");
  }
  VectorExactQueryRequest query;
  query.provider = opened.provider;
  query.recheck_proof = ToVectorExactRecheckProof(context.proof);
  query.queries.push_back({*query_vector, 1, {}, {}});
  const auto queried = QueryVectorExactPhysicalProvider(query);
  if (!queried.ok() || queried.batch_results.empty() ||
      (!provider_empty && queried.batch_results.front().candidates.empty()) ||
      (provider_empty && !queried.batch_results.front().candidates.empty())) {
    return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.QUERY_FAILED",
                            queried.diagnostic.diagnostic_code);
  }
  VectorProviderValidationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, "INDEX.VECTOR_PROVIDER_MAINTENANCE.EXACT_VALIDATED",
      "index.vector_provider_maintenance.exact_validated");
  result.provider_kind = VectorProviderMaintenanceKind::exact;
  result.fail_closed = false;
  result.validated = true;
  result.serialize_open_path_consumed = true;
  result.query_sanity_consumed = true;
  result.candidate_only_non_authority = true;
  result.provider_generation = opened.provider.provider_generation;
  AddCommonEvidence(&result.evidence, result.provider_kind, context);
  result.evidence.insert(result.evidence.end(), opened.provider.evidence.begin(),
                         opened.provider.evidence.end());
  result.evidence.insert(result.evidence.end(), queried.evidence.begin(),
                         queried.evidence.end());
  if (provider_empty) {
    Add(&result.evidence, "empty_provider_query_result_empty=true");
  }
  AppendValidationEvidence(&result, true, provider_empty);
  return result;
}

VectorProviderValidationResult ValidateVectorHnswProviderForMaintenance(
    const VectorHnswPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context) {
  if (!ProofComplete(context.proof)) {
    return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.PROOF_REQUIRED",
                            "exact_source_mga_security_recheck_proof_required");
  }
  if (!ContextMatchesHnsw(provider, context)) {
    return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.STALE_HNSW",
                            "stale_provider_training_descriptor_or_metric_generation");
  }
  if (!ProviderAuthoritySafe(provider)) {
    return RefuseValidation(
        VectorProviderMaintenanceKind::hnsw, context,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.AUTHORITY_REFUSED",
        "provider_candidate_only_non_authority_required");
  }
  const auto serialized = SerializeVectorHnswPhysicalProvider(provider);
  if (!serialized.ok()) {
    return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.SERIALIZE_FAILED",
                            serialized.diagnostic.diagnostic_code);
  }
  VectorHnswOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = context.collection_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = context.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = context.provider_uuid;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = context.expected_provider_generation;
  open.expected_training_generation_present = true;
  open.expected_training_generation = context.expected_training_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = context.expected_descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = context.expected_metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = provider.descriptor.dimensions;
  open.recheck_proof = ToVectorExactRecheckProof(context.proof);
  const auto opened = OpenVectorHnswPhysicalProvider(open);
  const auto repair = RepairClassFrom(opened.open_class);
  if (!opened.ok()) {
    return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                            opened.diagnostic.diagnostic_code,
                            VectorHnswOpenClassName(opened.open_class),
                            repair);
  }
  const bool provider_empty = opened.provider.live_node_count == 0;
  const auto query_vector = provider_empty
                                ? std::optional<std::vector<float>>(
                                      ZeroVector(opened.provider.descriptor.dimensions))
                                : FirstHnswQueryVector(opened.provider);
  if (!query_vector) {
    return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.QUERY_REFUSED",
                            "hnsw_query_sanity_vector_required");
  }
  VectorHnswQueryRequest query;
  query.provider = opened.provider;
  query.recheck_proof = ToVectorExactRecheckProof(context.proof);
  VectorHnswQuery single;
  single.vector = *query_vector;
  single.top_k = 1;
  single.ef_search = opened.provider.profile.ef_search;
  query.queries.push_back(std::move(single));
  const auto queried = QueryVectorHnswPhysicalProvider(query);
  if (!queried.ok() || queried.batch_results.empty() ||
      (!provider_empty && queried.batch_results.front().candidates.empty()) ||
      (provider_empty && !queried.batch_results.front().candidates.empty())) {
    return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.QUERY_FAILED",
                            queried.diagnostic.diagnostic_code);
  }
  VectorProviderValidationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, "INDEX.VECTOR_PROVIDER_MAINTENANCE.HNSW_VALIDATED",
      "index.vector_provider_maintenance.hnsw_validated");
  result.provider_kind = VectorProviderMaintenanceKind::hnsw;
  result.fail_closed = false;
  result.validated = true;
  result.serialize_open_path_consumed = true;
  result.query_sanity_consumed = true;
  result.candidate_only_non_authority = true;
  result.provider_generation = opened.provider.provider_generation;
  result.training_generation = opened.provider.training_generation;
  AddCommonEvidence(&result.evidence, result.provider_kind, context);
  result.evidence.insert(result.evidence.end(), opened.provider.evidence.begin(),
                         opened.provider.evidence.end());
  result.evidence.insert(result.evidence.end(), queried.evidence.begin(),
                         queried.evidence.end());
  Add(&result.evidence,
      "tombstone_ratio=" +
          std::to_string(HnswEffectiveTombstoneRatio(queried.provider_after_telemetry)));
  Add(&result.evidence,
      "recall_floor=" +
          std::to_string(queried.provider_after_telemetry.last_query_recall_floor));
  Add(&result.evidence,
      "latency_units=" +
          std::to_string(queried.provider_after_telemetry.last_query_latency_units));
  if (provider_empty) {
    Add(&result.evidence, "empty_provider_query_result_empty=true");
  }
  AppendValidationEvidence(&result, true, provider_empty);
  return result;
}

VectorProviderValidationResult ValidateVectorIvfPqProviderForMaintenance(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context) {
  if (!ProofComplete(context.proof)) {
    return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.PROOF_REQUIRED",
                            "exact_source_mga_security_recheck_proof_required");
  }
  if (!ContextMatchesIvfPq(provider, context)) {
    return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.STALE_IVF_PQ",
                            "stale_provider_training_descriptor_or_metric_generation");
  }
  if (!ProviderAuthoritySafe(provider)) {
    return RefuseValidation(
        VectorProviderMaintenanceKind::ivf_pq, context,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.AUTHORITY_REFUSED",
        "provider_candidate_only_non_authority_required");
  }
  const auto serialized = SerializeVectorIvfPqPhysicalProvider(provider);
  if (!serialized.ok()) {
    return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.SERIALIZE_FAILED",
                            serialized.diagnostic.diagnostic_code);
  }
  VectorIvfPqOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = context.collection_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = context.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = context.provider_uuid;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = context.expected_provider_generation;
  open.expected_training_generation_present = true;
  open.expected_training_generation = context.expected_training_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = context.expected_descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = context.expected_metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = provider.descriptor.dimensions;
  open.recheck_proof = ToVectorExactRecheckProof(context.proof);
  const auto opened = OpenVectorIvfPqPhysicalProvider(open);
  const auto repair = RepairClassFrom(opened.open_class);
  if (!opened.ok()) {
    return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                            opened.diagnostic.diagnostic_code,
                            VectorIvfPqOpenClassName(opened.open_class),
                            repair);
  }
  const bool provider_empty = opened.provider.live_vector_count == 0;
  const auto query_vector = provider_empty
                                ? std::optional<std::vector<float>>(
                                      ZeroVector(opened.provider.descriptor.dimensions))
                                : FirstIvfPqQueryVector(opened.provider);
  if (!query_vector) {
    return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.QUERY_REFUSED",
                            "ivf_pq_query_sanity_vector_required");
  }
  VectorIvfPqQueryRequest query;
  query.provider = opened.provider;
  query.recheck_proof = ToVectorExactRecheckProof(context.proof);
  VectorIvfPqQuery single;
  single.vector = *query_vector;
  single.top_k = 1;
  single.nprobe = opened.provider.profile.nprobe;
  query.queries.push_back(std::move(single));
  const auto queried = QueryVectorIvfPqPhysicalProvider(query);
  if (!queried.ok() || queried.batch_results.empty() ||
      (!provider_empty && queried.batch_results.front().candidates.empty()) ||
      (provider_empty && !queried.batch_results.front().candidates.empty())) {
    return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                            "INDEX.VECTOR_PROVIDER_MAINTENANCE.QUERY_FAILED",
                            queried.diagnostic.diagnostic_code);
  }
  VectorProviderValidationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, "INDEX.VECTOR_PROVIDER_MAINTENANCE.IVF_PQ_VALIDATED",
      "index.vector_provider_maintenance.ivf_pq_validated");
  result.provider_kind = VectorProviderMaintenanceKind::ivf_pq;
  result.fail_closed = false;
  result.validated = true;
  result.serialize_open_path_consumed = true;
  result.query_sanity_consumed = true;
  result.candidate_only_non_authority = true;
  result.provider_generation = opened.provider.provider_generation;
  result.training_generation = opened.provider.training_generation;
  AddCommonEvidence(&result.evidence, result.provider_kind, context);
  result.evidence.insert(result.evidence.end(), opened.provider.evidence.begin(),
                         opened.provider.evidence.end());
  result.evidence.insert(result.evidence.end(), queried.evidence.begin(),
                         queried.evidence.end());
  Add(&result.evidence,
      "list_imbalance_ratio=" +
          std::to_string(queried.provider_after_telemetry.list_imbalance_ratio));
  Add(&result.evidence,
      "tombstone_ratio=" +
          std::to_string(IvfPqEffectiveTombstoneRatio(
              queried.provider_after_telemetry)));
  Add(&result.evidence,
      "residual_error_mean=" +
          std::to_string(queried.provider_after_telemetry.residual_error_mean));
  Add(&result.evidence,
      "compression_error_mean=" +
          std::to_string(
              queried.provider_after_telemetry.compression_error_mean));
  Add(&result.evidence,
      "latency_units=" +
          std::to_string(queried.provider_after_telemetry.last_query_latency_units));
  if (provider_empty) {
    Add(&result.evidence, "empty_provider_query_result_empty=true");
  }
  AppendValidationEvidence(&result, true, provider_empty);
  return result;
}

VectorProviderValidationResult DiagnoseVectorExactProviderRepair(
    const std::vector<byte>& bytes,
    const VectorProviderMaintenanceContext& context) {
  VectorExactOpenRequest open;
  open.bytes = bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = context.collection_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = context.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = context.provider_uuid;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = context.expected_provider_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = context.expected_descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = context.expected_metric_resource_epoch;
  open.recheck_proof = ToVectorExactRecheckProof(context.proof);
  const auto opened = OpenVectorExactPhysicalProvider(open);
  if (opened.ok()) {
    return ValidateVectorExactProviderForMaintenance(opened.provider, context);
  }
  return RefuseValidation(VectorProviderMaintenanceKind::exact, context,
                          opened.diagnostic.diagnostic_code,
                          VectorExactOpenClassName(opened.open_class),
                          RepairClassFrom(opened.open_class));
}

VectorProviderValidationResult DiagnoseVectorHnswProviderRepair(
    const std::vector<byte>& bytes,
    const VectorProviderMaintenanceContext& context) {
  VectorHnswOpenRequest open;
  open.bytes = bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = context.collection_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = context.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = context.provider_uuid;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = context.expected_provider_generation;
  open.expected_training_generation_present = true;
  open.expected_training_generation = context.expected_training_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = context.expected_descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = context.expected_metric_resource_epoch;
  open.recheck_proof = ToVectorExactRecheckProof(context.proof);
  const auto opened = OpenVectorHnswPhysicalProvider(open);
  if (opened.ok()) {
    return ValidateVectorHnswProviderForMaintenance(opened.provider, context);
  }
  return RefuseValidation(VectorProviderMaintenanceKind::hnsw, context,
                          opened.diagnostic.diagnostic_code,
                          VectorHnswOpenClassName(opened.open_class),
                          RepairClassFrom(opened.open_class));
}

VectorProviderValidationResult DiagnoseVectorIvfPqProviderRepair(
    const std::vector<byte>& bytes,
    const VectorProviderMaintenanceContext& context) {
  VectorIvfPqOpenRequest open;
  open.bytes = bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = context.collection_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = context.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = context.provider_uuid;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = context.expected_provider_generation;
  open.expected_training_generation_present = true;
  open.expected_training_generation = context.expected_training_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = context.expected_descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = context.expected_metric_resource_epoch;
  open.recheck_proof = ToVectorExactRecheckProof(context.proof);
  const auto opened = OpenVectorIvfPqPhysicalProvider(open);
  if (opened.ok()) {
    return ValidateVectorIvfPqProviderForMaintenance(opened.provider, context);
  }
  return RefuseValidation(VectorProviderMaintenanceKind::ivf_pq, context,
                          opened.diagnostic.diagnostic_code,
                          VectorIvfPqOpenClassName(opened.open_class),
                          RepairClassFrom(opened.open_class));
}

VectorProviderMaintenanceResult ScheduleVectorExactProviderMaintenance(
    const VectorExactPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context) {
  auto job = BaseJob(context,
                     VectorProviderMaintenanceKind::exact,
                     VectorProviderMaintenanceJobKind::validate,
                     provider.provider_generation,
                     0);
  const auto validation =
      ValidateVectorExactProviderForMaintenance(provider, context);
  job.evidence.insert(job.evidence.end(), validation.evidence.begin(),
                      validation.evidence.end());
  job.support_bundle_rows.insert(job.support_bundle_rows.end(),
                                 validation.support_bundle_rows.begin(),
                                 validation.support_bundle_rows.end());
  if (!validation.ok()) {
    job.state = VectorProviderMaintenanceJobState::validation_failed;
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.EXACT_VALIDATION_FAILED",
                     validation.repair_reason.empty()
                         ? validation.diagnostic.diagnostic_code
                         : validation.repair_reason,
                     validation.restricted_repair_required
                         ? VectorProviderMaintenanceFailureClass::
                               restricted_repair_required
                         : VectorProviderMaintenanceFailureClass::
                               permanent_validation_failed);
  }
  job.state = VectorProviderMaintenanceJobState::validation_passed;
  job.validation_successful = true;
  job.completed_units = 1;
  Add(&job.evidence, "exact_provider_validated_no_replacement_required=true");
  return FinishJob(std::move(job));
}

VectorProviderMaintenanceResult ScheduleVectorHnswProviderMaintenance(
    const VectorHnswPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context) {
  std::vector<std::string> reasons;
  if (HnswEffectiveTombstoneRatio(provider) >
      context.policy.max_tombstone_ratio) {
    reasons.push_back("tombstone_ratio");
  }
  if (provider.last_query_recall_floor < context.policy.min_recall_floor ||
      (1.0 - provider.last_query_recall_floor) >
          context.policy.max_recall_drift) {
    reasons.push_back("recall_drift");
  }
  if (LatencyExceeded(provider.last_query_latency_units, context.policy)) {
    reasons.push_back("latency_policy");
  }
  const auto job_kind = reasons.empty()
                            ? VectorProviderMaintenanceJobKind::validate
                            : VectorProviderMaintenanceJobKind::compact;
  auto job = BaseJob(context,
                     VectorProviderMaintenanceKind::hnsw,
                     job_kind,
                     job_kind == VectorProviderMaintenanceJobKind::compact
                         ? provider.provider_generation + 1
                         : provider.provider_generation,
                     provider.training_generation);
  job.policy_reasons = reasons;
  Add(&job.evidence, "policy_reasons=" + JoinPolicyReasons(reasons));
  if (job_kind == VectorProviderMaintenanceJobKind::validate) {
    const auto validation =
        ValidateVectorHnswProviderForMaintenance(provider, context);
    job.evidence.insert(job.evidence.end(), validation.evidence.begin(),
                        validation.evidence.end());
    job.support_bundle_rows.insert(job.support_bundle_rows.end(),
                                   validation.support_bundle_rows.begin(),
                                   validation.support_bundle_rows.end());
    if (!validation.ok()) {
      return RefuseJob(std::move(job),
                       "INDEX.VECTOR_PROVIDER_MAINTENANCE.HNSW_VALIDATION_FAILED",
                       validation.repair_reason,
                       VectorProviderMaintenanceFailureClass::
                           permanent_validation_failed);
    }
    job.state = VectorProviderMaintenanceJobState::validation_passed;
    job.validation_successful = true;
    job.completed_units = 1;
    return FinishJob(std::move(job));
  }
  const auto compacted =
      CompactVectorHnswPhysicalProvider(provider,
                                        ToVectorExactRecheckProof(context.proof));
  if (!compacted.ok()) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.HNSW_COMPACT_FAILED",
                     compacted.diagnostic.diagnostic_code,
                     VectorProviderMaintenanceFailureClass::
                         permanent_validation_failed);
  }
  const auto candidate_context = CandidateContext(
      context, compacted.provider.provider_generation,
      compacted.provider.training_generation);
  const auto validation =
      ValidateVectorHnswProviderForMaintenance(compacted.provider,
                                               candidate_context);
  job.evidence.insert(job.evidence.end(), validation.evidence.begin(),
                      validation.evidence.end());
  job.support_bundle_rows.insert(job.support_bundle_rows.end(),
                                 validation.support_bundle_rows.begin(),
                                 validation.support_bundle_rows.end());
  if (!validation.ok() ||
      compacted.provider.provider_generation <= provider.provider_generation) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.HNSW_CANDIDATE_REFUSED",
                     validation.repair_reason,
                     VectorProviderMaintenanceFailureClass::
                         permanent_validation_failed);
  }
  job.new_provider_generation = compacted.provider.provider_generation;
  job.new_training_generation = compacted.provider.training_generation;
  job.validation_successful = true;
  job.publish_candidate_available = true;
  job.candidate_only_non_authority = true;
  job.hnsw_candidate = compacted.provider;
  job.state = VectorProviderMaintenanceJobState::publish_ready;
  job.completed_units = 1;
  Add(&job.evidence, "hnsw_compaction_replacement_provider_created=true");
  Add(&job.evidence,
      "removed_tombstones=" + std::to_string(compacted.removed_tombstones));
  AddSupport(&job.support_bundle_rows, "hnsw_compaction", "replacement_ready");
  return FinishJob(std::move(job));
}

VectorProviderMaintenanceResult ScheduleVectorIvfPqProviderMaintenance(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context,
    const std::vector<VectorIvfPqSourceRow>& authoritative_source_rows) {
  std::vector<std::string> reasons;
  const double tombstone_ratio = IvfPqEffectiveTombstoneRatio(provider);
  if (provider.list_imbalance_ratio > context.policy.max_list_imbalance_ratio) {
    reasons.push_back("list_imbalance");
  }
  if (tombstone_ratio > context.policy.max_tombstone_ratio) {
    reasons.push_back("tombstone_ratio");
  }
  if (provider.residual_error_mean > context.policy.max_residual_error_mean) {
    reasons.push_back("residual_error");
  }
  if (provider.compression_error_mean >
      context.policy.max_compression_error_mean) {
    reasons.push_back("compression_error");
  }
  if (provider.last_query_recall_floor < context.policy.min_recall_floor ||
      (1.0 - provider.last_query_recall_floor) >
          context.policy.max_recall_drift) {
    reasons.push_back("recall_drift");
  }
  if (LatencyExceeded(provider.last_query_latency_units, context.policy)) {
    reasons.push_back("latency_policy");
  }
  VectorProviderMaintenanceJobKind job_kind =
      VectorProviderMaintenanceJobKind::validate;
  if (std::find(reasons.begin(), reasons.end(), "list_imbalance") !=
          reasons.end() ||
      std::find(reasons.begin(), reasons.end(), "residual_error") !=
          reasons.end() ||
      std::find(reasons.begin(), reasons.end(), "compression_error") !=
          reasons.end() ||
      std::find(reasons.begin(), reasons.end(), "recall_drift") !=
          reasons.end()) {
    job_kind = VectorProviderMaintenanceJobKind::retrain;
  } else if (!reasons.empty()) {
    job_kind = VectorProviderMaintenanceJobKind::rebuild;
  }
  const u64 new_provider_generation =
      job_kind == VectorProviderMaintenanceJobKind::validate
          ? provider.provider_generation
          : provider.provider_generation + 1;
  const u64 new_training_generation =
      job_kind == VectorProviderMaintenanceJobKind::retrain
          ? provider.training_generation + 1
          : provider.training_generation;
  auto job = BaseJob(context,
                     VectorProviderMaintenanceKind::ivf_pq,
                     job_kind,
                     new_provider_generation,
                     new_training_generation);
  job.policy_reasons = reasons;
  Add(&job.evidence, "policy_reasons=" + JoinPolicyReasons(reasons));
  Add(&job.evidence,
      "list_imbalance_ratio=" + std::to_string(provider.list_imbalance_ratio));
  Add(&job.evidence, "tombstone_ratio=" + std::to_string(tombstone_ratio));
  Add(&job.evidence,
      "residual_error_mean=" +
          std::to_string(provider.residual_error_mean));
  Add(&job.evidence,
      "compression_error_mean=" +
          std::to_string(provider.compression_error_mean));
  if (job_kind == VectorProviderMaintenanceJobKind::validate) {
    const auto validation =
        ValidateVectorIvfPqProviderForMaintenance(provider, context);
    job.evidence.insert(job.evidence.end(), validation.evidence.begin(),
                        validation.evidence.end());
    job.support_bundle_rows.insert(job.support_bundle_rows.end(),
                                   validation.support_bundle_rows.begin(),
                                   validation.support_bundle_rows.end());
    if (!validation.ok()) {
      return RefuseJob(std::move(job),
                       "INDEX.VECTOR_PROVIDER_MAINTENANCE.IVF_PQ_VALIDATION_FAILED",
                       validation.repair_reason,
                       VectorProviderMaintenanceFailureClass::
                           permanent_validation_failed);
    }
    job.state = VectorProviderMaintenanceJobState::validation_passed;
    job.validation_successful = true;
    job.completed_units = 1;
    return FinishJob(std::move(job));
  }
  if (authoritative_source_rows.empty()) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.SOURCE_REQUIRED",
                     "authoritative_source_rows_required_for_rebuild_or_retrain",
                     VectorProviderMaintenanceFailureClass::missing_proof);
  }
  VectorIvfPqBuildRequest build;
  build.relation_uuid = provider.relation_uuid;
  build.index_uuid = provider.index_uuid;
  build.provider_uuid = provider.provider_uuid;
  build.base_generation = provider.base_generation;
  build.provider_generation = new_provider_generation;
  build.training_generation = new_training_generation;
  build.descriptor = provider.descriptor;
  build.metric = provider.metric;
  build.profile = provider.profile;
  build.recheck_proof = ToVectorExactRecheckProof(context.proof);
  build.rows = authoritative_source_rows;
  const auto built = BuildVectorIvfPqPhysicalProvider(build);
  if (!built.ok()) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.IVF_PQ_BUILD_FAILED",
                     built.diagnostic.diagnostic_code,
                     VectorProviderMaintenanceFailureClass::
                         permanent_validation_failed);
  }
  const auto candidate_context =
      CandidateContext(context, built.provider.provider_generation,
                       built.provider.training_generation);
  const auto validation =
      ValidateVectorIvfPqProviderForMaintenance(built.provider,
                                                candidate_context);
  job.evidence.insert(job.evidence.end(), validation.evidence.begin(),
                      validation.evidence.end());
  job.support_bundle_rows.insert(job.support_bundle_rows.end(),
                                 validation.support_bundle_rows.begin(),
                                 validation.support_bundle_rows.end());
  if (!validation.ok() ||
      built.provider.provider_generation <= provider.provider_generation) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.IVF_PQ_CANDIDATE_REFUSED",
                     validation.repair_reason,
                     VectorProviderMaintenanceFailureClass::
                         permanent_validation_failed);
  }
  job.validation_successful = true;
  job.publish_candidate_available = true;
  job.candidate_only_non_authority = true;
  job.ivf_pq_candidate = built.provider;
  job.state = VectorProviderMaintenanceJobState::publish_ready;
  job.completed_units = 1;
  Add(&job.evidence, "ivf_pq_replacement_provider_created=true");
  AddSupport(&job.support_bundle_rows, "ivf_pq_replacement",
             VectorProviderMaintenanceJobKindName(job_kind));
  return FinishJob(std::move(job));
}

VectorProviderMaintenanceResult RecordVectorProviderMaintenanceProgress(
    VectorProviderMaintenanceJob job,
    u64 completed_units,
    u64 total_units,
    std::string stage) {
  if (total_units == 0 || completed_units > total_units ||
      job.state == VectorProviderMaintenanceJobState::cancelled ||
      job.state == VectorProviderMaintenanceJobState::published ||
      job.state == VectorProviderMaintenanceJobState::refused) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.PROGRESS_REFUSED",
                     "progress_invalid_for_job_state",
                     VectorProviderMaintenanceFailureClass::invalid_state);
  }
  job.completed_units = completed_units;
  job.total_units = total_units;
  job.state = completed_units == total_units
                  ? VectorProviderMaintenanceJobState::validation_passed
                  : VectorProviderMaintenanceJobState::running;
  Add(&job.evidence, "progress_stage=" + std::move(stage));
  Add(&job.evidence,
      "progress=" + std::to_string(completed_units) + "/" +
          std::to_string(total_units));
  AddSupport(&job.support_bundle_rows, "progress",
             std::to_string(completed_units) + "/" +
                 std::to_string(total_units));
  return FinishJob(std::move(job));
}

VectorProviderMaintenanceResult CancelVectorProviderMaintenanceJob(
    VectorProviderMaintenanceJob job,
    std::string reason) {
  if (job.state == VectorProviderMaintenanceJobState::published ||
      job.state == VectorProviderMaintenanceJobState::refused ||
      job.state == VectorProviderMaintenanceJobState::cancelled) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.CANCEL_REFUSED",
                     "cancel_invalid_for_job_state",
                     VectorProviderMaintenanceFailureClass::invalid_state);
  }
  job.state = VectorProviderMaintenanceJobState::cancelled;
  Add(&job.evidence, "cancelled=true");
  AddSupport(&job.support_bundle_rows, "cancel_reason", std::move(reason));
  return FinishJob(std::move(job));
}

VectorProviderMaintenanceResult ResumeVectorProviderMaintenanceJob(
    VectorProviderMaintenanceJob job) {
  if (job.state != VectorProviderMaintenanceJobState::cancelled &&
      job.state != VectorProviderMaintenanceJobState::failed) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.RESUME_REFUSED",
                     "resume_requires_cancelled_or_failed_state",
                     VectorProviderMaintenanceFailureClass::invalid_state);
  }
  if (job.retry_attempts >= job.max_retry_attempts) {
    return RefuseJob(std::move(job),
                     "INDEX.VECTOR_PROVIDER_MAINTENANCE.RESUME_RETRY_EXHAUSTED",
                     "retry_budget_exhausted",
                     VectorProviderMaintenanceFailureClass::invalid_state);
  }
  job.state = VectorProviderMaintenanceJobState::scheduled;
  job.failure_class = VectorProviderMaintenanceFailureClass::none;
  Add(&job.evidence, "resumed=true");
  AddSupport(&job.support_bundle_rows, "resume", "scheduled");
  return FinishJob(std::move(job));
}

VectorProviderMaintenanceResult ClassifyVectorProviderMaintenanceFailure(
    VectorProviderMaintenanceJob job,
    VectorProviderMaintenanceFailureClass failure_class,
    bool retryable) {
  job.failure_class = failure_class;
  Add(&job.evidence,
      "failure_class=" +
          std::string(VectorProviderMaintenanceFailureClassName(failure_class)));
  AddSupport(&job.support_bundle_rows, "failure_class",
             VectorProviderMaintenanceFailureClassName(failure_class));
  if (retryable && job.retry_attempts < job.max_retry_attempts) {
    ++job.retry_attempts;
    job.state = VectorProviderMaintenanceJobState::scheduled;
    Add(&job.evidence,
        "retry_attempts=" + std::to_string(job.retry_attempts));
    return FinishJob(std::move(job));
  }
  job.state = VectorProviderMaintenanceJobState::failed;
  Add(&job.evidence, "job_state=failed");
  VectorProviderMaintenanceResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status,
      "INDEX.VECTOR_PROVIDER_MAINTENANCE.FAILURE_CLASSIFIED",
      "index.vector_provider_maintenance.failure_classified",
      VectorProviderMaintenanceFailureClassName(failure_class));
  result.job = std::move(job);
  result.fail_closed = true;
  return result;
}

VectorProviderPublishResult PublishVectorProviderMaintenanceCandidate(
    VectorProviderMaintenanceJob job,
    const VectorProviderMaintenanceContext& context,
    u64 active_provider_generation) {
  VectorProviderPublishResult result;
  result.status = ErrorStatus();
  result.job = job;
  result.fail_closed = true;
  AddCommonEvidence(&result.evidence, job.provider_kind, context);
  if (!PublishProofComplete(context.proof)) {
    result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
        result.status,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_PROOF_REQUIRED",
        "index.vector_provider_maintenance.publish_refused",
        "publish_requires_exact_mga_security_validation_generation_proof");
    result.job.failure_class =
        VectorProviderMaintenanceFailureClass::missing_proof;
    AddSupport(&result.support_bundle_rows, "publish_refused",
               "missing_publish_proof");
    return result;
  }
  if (active_provider_generation != job.old_provider_generation) {
    result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
        result.status,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_STALE_GENERATION",
        "index.vector_provider_maintenance.publish_refused",
        "active_provider_generation_changed");
    result.job.failure_class =
        VectorProviderMaintenanceFailureClass::stale_generation;
    AddSupport(&result.support_bundle_rows, "publish_refused",
               "active_provider_generation_changed");
    return result;
  }
  if (!CanPublishCandidate(job)) {
    result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
        result.status,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_VALIDATION_REQUIRED",
        "index.vector_provider_maintenance.publish_refused",
        "candidate_validation_generation_and_non_authority_evidence_required");
    result.job.failure_class =
        VectorProviderMaintenanceFailureClass::unsafe_publish;
    AddSupport(&result.support_bundle_rows, "publish_refused",
               "validation_or_candidate_missing");
    return result;
  }
  bool authority_safe = false;
  bool candidate_revalidated = false;
  VectorProviderMaintenanceContext candidate_context =
      CandidateContext(context, job.new_provider_generation,
                       job.new_training_generation);
  if (job.exact_candidate) {
    authority_safe = ProviderAuthoritySafe(*job.exact_candidate);
    const auto validation = ValidateVectorExactProviderForMaintenance(
        *job.exact_candidate, candidate_context);
    result.evidence.insert(result.evidence.end(), validation.evidence.begin(),
                           validation.evidence.end());
    result.support_bundle_rows.insert(
        result.support_bundle_rows.end(), validation.support_bundle_rows.begin(),
        validation.support_bundle_rows.end());
    if (!validation.ok()) {
      result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
          result.status,
          "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_REVALIDATION_FAILED",
          "index.vector_provider_maintenance.publish_refused",
          "exact_candidate_failed_publish_revalidation");
      result.job.failure_class =
          VectorProviderMaintenanceFailureClass::unsafe_publish;
      AddSupport(&result.support_bundle_rows, "publish_refused",
                 "exact_candidate_revalidation_failed");
      return result;
    }
    candidate_revalidated = true;
    result.exact_provider = job.exact_candidate;
  } else if (job.hnsw_candidate) {
    authority_safe = ProviderAuthoritySafe(*job.hnsw_candidate);
    const auto validation = ValidateVectorHnswProviderForMaintenance(
        *job.hnsw_candidate, candidate_context);
    result.evidence.insert(result.evidence.end(), validation.evidence.begin(),
                           validation.evidence.end());
    result.support_bundle_rows.insert(
        result.support_bundle_rows.end(), validation.support_bundle_rows.begin(),
        validation.support_bundle_rows.end());
    if (!validation.ok()) {
      result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
          result.status,
          "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_REVALIDATION_FAILED",
          "index.vector_provider_maintenance.publish_refused",
          "hnsw_candidate_failed_publish_revalidation");
      result.job.failure_class =
          VectorProviderMaintenanceFailureClass::unsafe_publish;
      AddSupport(&result.support_bundle_rows, "publish_refused",
                 "hnsw_candidate_revalidation_failed");
      return result;
    }
    candidate_revalidated = true;
    result.hnsw_provider = job.hnsw_candidate;
  } else if (job.ivf_pq_candidate) {
    authority_safe = ProviderAuthoritySafe(*job.ivf_pq_candidate);
    const auto validation = ValidateVectorIvfPqProviderForMaintenance(
        *job.ivf_pq_candidate, candidate_context);
    result.evidence.insert(result.evidence.end(), validation.evidence.begin(),
                           validation.evidence.end());
    result.support_bundle_rows.insert(
        result.support_bundle_rows.end(), validation.support_bundle_rows.begin(),
        validation.support_bundle_rows.end());
    if (!validation.ok()) {
      result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
          result.status,
          "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_REVALIDATION_FAILED",
          "index.vector_provider_maintenance.publish_refused",
          "ivf_pq_candidate_failed_publish_revalidation");
      result.job.failure_class =
          VectorProviderMaintenanceFailureClass::unsafe_publish;
      AddSupport(&result.support_bundle_rows, "publish_refused",
                 "ivf_pq_candidate_revalidation_failed");
      return result;
    }
    candidate_revalidated = true;
    result.ivf_pq_provider = job.ivf_pq_candidate;
  }
  if (!candidate_revalidated || !authority_safe) {
    result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
        result.status,
        "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISH_AUTHORITY_REFUSED",
        "index.vector_provider_maintenance.publish_refused",
        "replacement_provider_must_remain_candidate_only_non_authority");
    result.job.failure_class =
        VectorProviderMaintenanceFailureClass::authority_boundary_refused;
    AddSupport(&result.support_bundle_rows, "publish_refused",
               "candidate_authority_claim");
    return result;
  }
  result.status = OkStatus();
  result.diagnostic = MakeVectorProviderMaintenanceDiagnostic(
      result.status, "INDEX.VECTOR_PROVIDER_MAINTENANCE.PUBLISHED",
      "index.vector_provider_maintenance.published");
  result.fail_closed = false;
  result.published = true;
  result.job.state = VectorProviderMaintenanceJobState::published;
  result.job.failure_class = VectorProviderMaintenanceFailureClass::none;
  Add(&result.evidence, "published=true");
  Add(&result.evidence,
      "old_provider_generation=" +
          std::to_string(job.old_provider_generation));
  Add(&result.evidence,
      "new_provider_generation=" +
          std::to_string(job.new_provider_generation));
  Add(&result.evidence, "candidate_only_non_authority=true");
  Add(&result.evidence, "candidate_revalidated_before_publish=true");
  AddSupport(&result.support_bundle_rows, "publish", "generation_advanced");
  return result;
}

DiagnosticRecord MakeVectorProviderMaintenanceDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = kVectorProviderMaintenanceSearchKey;
  diagnostic.remediation_hint =
      "use authoritative source vectors plus exact source, MGA, security, validation, and generation recheck proof before repair or publish";
  if (!detail.empty()) {
    diagnostic.arguments.push_back({"detail", std::move(detail)});
  }
  return diagnostic;
}

}  // namespace scratchbird::core::index
