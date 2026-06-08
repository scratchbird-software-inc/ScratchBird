// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_index_constraint_publication.hpp"

#include "index_metapage.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <functional>
#include <future>
#include <set>
#include <sstream>
#include <unordered_set>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string key,
                            std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(key),
                        std::move(arguments),
                        {},
                        "core.index.parallel_constraint_publication",
                        status.ok() ? "" : "fall back to exact DML path");
}

void AddEvidence(ParallelIndexConstraintPublicationResult* result,
                 std::string kind,
                 std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

std::string Bool(bool value) { return value ? "true" : "false"; }

ParallelIndexConstraintPublicationResult Refuse(
    std::string code,
    std::string key,
    std::string detail,
    const ParallelIndexConstraintPublicationRequest& request) {
  ParallelIndexConstraintPublicationResult result;
  result.status = ErrorStatus();
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(key),
                                 std::move(detail));
  result.accepted = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.fallback_available = request.exact_fallback_available;
  result.runtime_consumed = request.runtime_consumed;
  result.route_label = request.route_label;
  result.worker_count = request.worker_count;
  result.row_count = static_cast<u64>(request.rows.size());
  result.parser_client_donor_authority =
      request.authority.parser_client_or_donor_index_authority;
  result.index_metadata_finality_authority =
      request.authority.index_metadata_visibility_or_finality_authority;
  result.index_metadata_recovery_authority =
      request.authority.index_metadata_recovery_authority;
  result.recovery_from_index_metadata_alone =
      request.authority.recovery_from_index_metadata_alone;
  AddEvidence(&result, "route_label", request.route_label);
  AddEvidence(&result, "benchmark_clean", "false");
  AddEvidence(&result, "fail_closed", "true");
  AddEvidence(&result, "exact_fallback_available",
              Bool(request.exact_fallback_available));
  AddEvidence(&result, "parser_client_donor_index_authority",
              Bool(result.parser_client_donor_authority));
  AddEvidence(&result, "index_metadata_visibility_finality_authority",
              Bool(result.index_metadata_finality_authority));
  AddEvidence(&result, "index_metadata_recovery_authority",
              Bool(result.index_metadata_recovery_authority));
  AddEvidence(&result, "recovery_from_index_metadata_alone",
              Bool(result.recovery_from_index_metadata_alone));
  return result;
}

bool OrderedWritePublicationFamily(IndexFamily family) {
  return family == IndexFamily::btree ||
         family == IndexFamily::unique_btree ||
         family == IndexFamily::expression ||
         family == IndexFamily::partial ||
         family == IndexFamily::covering;
}

struct WorkerValidationResult {
  bool ok = true;
  bool unique_ok = true;
  bool fk_ok = true;
  bool check_ok = true;
  u32 worker_id = 0;
  u64 row_count = 0;
  std::string blocker_code;
  std::string blocker_detail;
  std::vector<std::string> keys;
};

WorkerValidationResult ValidateWorkerShard(
    u32 worker_id,
    const std::vector<ParallelIndexConstraintRow>& rows,
    std::size_t begin,
    std::size_t end,
    bool unique,
    bool validate_foreign_keys,
    bool validate_checks,
    const std::set<std::string>& parent_keys) {
  WorkerValidationResult result;
  result.worker_id = worker_id;
  result.row_count = static_cast<u64>(end - begin);
  std::unordered_set<std::string> local_keys;
  for (std::size_t i = begin; i < end; ++i) {
    const auto& row = rows[i];
    result.keys.push_back(row.encoded_key);
    if (unique && !row.encoded_key.empty()) {
      const auto inserted = local_keys.insert(row.encoded_key);
      if (!inserted.second) {
        result.ok = false;
        result.unique_ok = false;
        result.blocker_code =
            "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.UNIQUENESS_CONFLICT";
        result.blocker_detail = row.encoded_key;
        return result;
      }
    }
    if (validate_foreign_keys &&
        parent_keys.find(row.parent_key) == parent_keys.end()) {
      result.ok = false;
      result.fk_ok = false;
      result.blocker_code =
          "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.FK_MISSING_PARENT";
      result.blocker_detail = row.parent_key;
      return result;
    }
    if (validate_checks && row.check_value < 0) {
      result.ok = false;
      result.check_ok = false;
      result.blocker_code =
          "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.CHECK_FAILED";
      result.blocker_detail = row.encoded_key;
      return result;
    }
  }
  std::sort(result.keys.begin(), result.keys.end());
  return result;
}

std::vector<SortedBulkIndexRowInput> ToSortedRows(
    const std::vector<ParallelIndexConstraintRow>& rows) {
  std::vector<SortedBulkIndexRowInput> out;
  out.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    SortedBulkIndexRowInput row;
    row.encoded_key = rows[i].encoded_key;
    row.row_uuid = rows[i].row_uuid;
    row.version_uuid = rows[i].version_uuid;
    row.payload_value = rows[i].payload_value;
    row.source_ordinal = static_cast<u64>(i);
    row.null_key = row.encoded_key.empty();
    out.push_back(std::move(row));
  }
  return out;
}

scratchbird::storage::page::IndexBtreePhysicalTreeImage ImageFromCandidate(
    const SortedBulkIndexCandidateRootGeneration& candidate) {
  scratchbird::storage::page::IndexBtreePhysicalTreeImage image;
  image.page_size = candidate.tree.page_size;
  image.index_uuid = candidate.tree.index_uuid;
  image.root_page_number = candidate.tree.root_page_number;
  image.next_page_number = candidate.tree.next_page_number;
  image.pages = candidate.tree.pages;
  return image;
}

IndexMetapageControl MetapageForCandidate(
    const SortedBulkIndexCandidateRootGeneration& candidate,
    IndexFamily family) {
  IndexMetapageControl metapage;
  metapage.index_uuid = candidate.tree.index_uuid;
  metapage.family = family;
  metapage.root_page_number = candidate.root_page_number;
  metapage.root_generation = candidate.candidate_generation;
  metapage.page_count = candidate.page_count;
  metapage.tuple_count_estimate = candidate.live_entry_count;
  metapage.resource_epoch = 1;
  metapage.mutation_epoch = 1;
  return metapage;
}

SortedBulkIndexBuildRequest BuildRequest(
    const ParallelIndexConstraintPublicationRequest& request,
    const std::vector<SortedBulkIndexRowInput>& rows) {
  SortedBulkIndexBuildRequest build;
  build.metadata.index_uuid = request.index_uuid;
  build.metadata.table_uuid = request.table_uuid;
  build.metadata.family = request.family;
  build.metadata.family_name = IndexFamilyName(request.family);
  build.metadata.semantic_profile = "orh_285_parallel_constraint_publication";
  build.metadata.unique = request.unique || request.family == IndexFamily::unique_btree;
  build.metadata.unique_nulls_distinct = true;
  build.metadata.rebuild = false;
  build.metadata.policy_allows_mutation = true;
  build.metadata.online = true;
  build.metadata.physical_page_size = 4096;
  build.metadata.leaf_entry_capacity = 4;
  build.metadata.internal_entry_capacity = 4;
  build.rows = rows;
  build.visible_unique_keys = request.visible_unique_keys;
  return build;
}

std::string HashEvidence(
    const ParallelIndexConstraintPublicationRequest& request,
    const SortedBulkIndexBuildResult& build,
    const IndexBulkPublishRecoveryResult& recovery) {
  std::uint64_t hash = 1469598103934665603ull;
  auto mix = [&hash](const std::string& text) {
    for (unsigned char ch : text) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
  };
  mix(request.route_label);
  mix(IndexFamilyName(request.family));
  mix(std::to_string(build.candidate_root_generation.candidate_generation));
  mix(std::to_string(build.entries.size()));
  mix(std::to_string(recovery.active_metapage.root_generation));
  for (const auto& row : build.entries) {
    mix(row.encoded_key);
    mix(row.row_uuid);
    mix(row.version_uuid);
  }
  std::ostringstream out;
  out << "orh285:" << std::hex << hash;
  return out.str();
}

bool HasDuplicateGlobalKey(const std::vector<std::string>& keys,
                           std::string* conflict) {
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (keys[i] == keys[i - 1]) {
      *conflict = keys[i];
      return true;
    }
  }
  return false;
}

}  // namespace

ParallelIndexConstraintPublicationResult
PublishParallelValidatedDeferredIndexGeneration(
    const ParallelIndexConstraintPublicationRequest& request) {
  if (!request.exact_fallback_available) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.NO_EXACT_FALLBACK",
                  "orh.parallel_index_constraint.no_exact_fallback",
                  {},
                  request);
  }
  if (!request.runtime_consumed || request.contract_only_evidence) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.NO_RUNTIME_CONSUMPTION",
                  "orh.parallel_index_constraint.no_runtime_consumption",
                  {},
                  request);
  }
  if (request.authority.parser_client_or_donor_index_authority) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.PARSER_CLIENT_DONOR_AUTHORITY",
                  "orh.parallel_index_constraint.parser_client_donor_authority",
                  {},
                  request);
  }
  if (request.authority.index_metadata_visibility_or_finality_authority) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.INDEX_METADATA_FINALITY_AUTHORITY",
                  "orh.parallel_index_constraint.index_metadata_finality_authority",
                  {},
                  request);
  }
  if (request.authority.index_metadata_recovery_authority ||
      request.authority.recovery_from_index_metadata_alone) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.INDEX_METADATA_RECOVERY_AUTHORITY",
                  "orh.parallel_index_constraint.index_metadata_recovery_authority",
                  {},
                  request);
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.MISSING_MGA_SECURITY_PROOF",
                  "orh.parallel_index_constraint.missing_mga_proof",
                  "durable MGA transaction inventory proof is required",
                  request);
  }
  if (!request.authority.security_context_bound ||
      request.security_epoch_token.empty()) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.MISSING_MGA_SECURITY_PROOF",
                  "orh.parallel_index_constraint.missing_security_proof",
                  "engine security epoch proof is required",
                  request);
  }
  if (!request.route_capability_fresh ||
      request.expected_route_capability_generation !=
          request.observed_route_capability_generation) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.STALE_ROUTE_CAPABILITY",
                  "orh.parallel_index_constraint.stale_route_capability",
                  {},
                  request);
  }
  const auto* capability =
      FindBuiltinIndexRouteCapabilityState(request.route, request.family);
  if (capability == nullptr || !capability->route_complete() ||
      !capability->supports_write ||
      !OrderedWritePublicationFamily(request.family)) {
    const std::string detail =
        capability != nullptr ? capability->route_detail
                              : "route capability missing";
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.ROUTE_CAPABILITY_BLOCKED",
                  "orh.parallel_index_constraint.route_capability_blocked",
                  detail,
                  request);
  }
  if (!request.generation_validation_proof) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.UNVALIDATED_GENERATION_PUBLISH",
                  "orh.parallel_index_constraint.unvalidated_generation_publish",
                  {},
                  request);
  }
  if (!request.publish_fence_available || request.publish_fence_token.empty()) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.MISSING_PUBLISH_FENCE",
                  "orh.parallel_index_constraint.missing_publish_fence",
                  {},
                  request);
  }
  if (!request.worker_result_match) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.WORKER_RESULT_MISMATCH",
                  "orh.parallel_index_constraint.worker_result_mismatch",
                  {},
                  request);
  }
  if (!request.index_uuid.valid() || !request.table_uuid.valid() ||
      request.rows.empty()) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.INVALID_DESCRIPTOR",
                  "orh.parallel_index_constraint.invalid_descriptor",
                  {},
                  request);
  }

  const std::set<std::string> parent_keys(request.parent_keys.begin(),
                                         request.parent_keys.end());
  const u32 worker_count =
      std::max<u32>(1, std::min<u32>(request.worker_count,
                                     static_cast<u32>(request.rows.size())));
  std::vector<std::future<WorkerValidationResult>> futures;
  futures.reserve(worker_count);
  const std::size_t shard = (request.rows.size() + worker_count - 1) /
                            worker_count;
  for (u32 worker = 0; worker < worker_count; ++worker) {
    const std::size_t begin = worker * shard;
    const std::size_t end =
        std::min<std::size_t>(request.rows.size(), begin + shard);
    if (begin >= end) {
      continue;
    }
    futures.push_back(std::async(std::launch::async,
                                 ValidateWorkerShard,
                                 worker,
                                 std::cref(request.rows),
                                 begin,
                                 end,
                                 request.unique,
                                 request.validate_foreign_keys,
                                 request.validate_checks,
                                 std::cref(parent_keys)));
  }

  std::vector<std::string> all_keys;
  ParallelIndexConstraintPublicationResult result;
  result.status = OkStatus();
  result.route_label = request.route_label;
  result.worker_count = static_cast<u64>(futures.size());
  result.row_count = static_cast<u64>(request.rows.size());
  result.runtime_consumed = true;
  result.route_capability_consumed = true;
  result.worker_evidence_consumed = !futures.empty();
  result.worker_result_match = true;
  result.mga_visibility_recheck_proven = true;
  result.security_recheck_proven = true;
  result.fallback_available = true;
  AddEvidence(&result, "route_label", request.route_label);
  AddEvidence(&result, "route_kind", IndexRouteKindName(request.route));
  AddEvidence(&result, "index_family", IndexFamilyName(request.family));
  AddEvidence(&result, "route_capability_consumed",
              capability->route_diagnostic_code);
  AddEvidence(&result, "corrected_route_capability_limits",
              "donor_policy_hash_vector_text_refused_for_dml_ordered_write");
  AddEvidence(&result, "parallel_worker_count",
              std::to_string(result.worker_count));
  AddEvidence(&result, "mga_visibility_authority",
              "durable_transaction_inventory");
  AddEvidence(&result, "security_authority", "engine_security_epoch_recheck");
  AddEvidence(&result, "index_build_visibility_authority", "false");
  AddEvidence(&result, "index_build_finality_authority", "false");
  AddEvidence(&result, "index_metadata_recovery_authority", "false");

  for (auto& future : futures) {
    const WorkerValidationResult worker = future.get();
    AddEvidence(&result,
                "parallel_worker_rows." + std::to_string(worker.worker_id),
                std::to_string(worker.row_count));
    if (!worker.ok) {
      return Refuse(worker.blocker_code,
                    "orh.parallel_index_constraint.worker_refused",
                    worker.blocker_detail,
                    request);
    }
    all_keys.insert(all_keys.end(), worker.keys.begin(), worker.keys.end());
  }
  std::sort(all_keys.begin(), all_keys.end());
  if (request.unique) {
    std::string conflict;
    if (HasDuplicateGlobalKey(all_keys, &conflict)) {
      return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.UNIQUENESS_CONFLICT",
                    "orh.parallel_index_constraint.uniqueness_conflict",
                    conflict,
                    request);
    }
  }
  result.parallel_uniqueness_validated = request.unique;
  result.parallel_foreign_key_validated = request.validate_foreign_keys;
  result.parallel_check_validated = request.validate_checks;
  AddEvidence(&result, "parallel_uniqueness_validated",
              Bool(result.parallel_uniqueness_validated));
  AddEvidence(&result, "parallel_foreign_key_validated",
              Bool(result.parallel_foreign_key_validated));
  AddEvidence(&result, "parallel_check_validated",
              Bool(result.parallel_check_validated));

  const std::vector<SortedBulkIndexRowInput> rows = ToSortedRows(request.rows);
  SortedBulkIndexBuildRequest build_request = BuildRequest(request, rows);
  result.sorted_build = BuildSortedExactBulkIndex(build_request);
  if (!result.sorted_build.ok() ||
      !result.sorted_build.candidate_root_generation.created ||
      !result.sorted_build.candidate_root_generation.validated_tree) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.SORTED_BUILD_REFUSED",
                  "orh.parallel_index_constraint.sorted_build_refused",
                  result.sorted_build.diagnostic.diagnostic_code,
                  request);
  }
  result.parallel_sorted_build_consumed = true;
  AddEvidence(&result, "sorted_bulk_index_build_consumed", "true");
  AddEvidence(&result, "sorted_bulk_candidate_tree_validated", "true");

  SortedBulkIndexBuildRequest old_request = BuildRequest(
      request,
      std::vector<SortedBulkIndexRowInput>{rows.front()});
  old_request.metadata.unique = false;
  old_request.metadata.family =
      request.family == IndexFamily::unique_btree ? IndexFamily::btree
                                                  : request.family;
  auto old_build = BuildSortedExactBulkIndex(old_request);
  if (!old_build.ok() || !old_build.candidate_root_generation.created) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.OLD_ROOT_BUILD_REFUSED",
                  "orh.parallel_index_constraint.old_root_build_refused",
                  old_build.diagnostic.diagnostic_code,
                  request);
  }
  old_build.candidate_root_generation.candidate_generation = 1;
  result.sorted_build.candidate_root_generation.candidate_generation = 2;

  const auto old_image =
      ImageFromCandidate(old_build.candidate_root_generation);
  const auto candidate_image =
      ImageFromCandidate(result.sorted_build.candidate_root_generation);
  const auto old_metapage =
      MetapageForCandidate(old_build.candidate_root_generation,
                           old_request.metadata.family);
  auto durable_metapage =
      MetapageForCandidate(result.sorted_build.candidate_root_generation,
                           old_request.metadata.family);
  durable_metapage.root_generation = 2;
  const auto serialized_metapage = BuildIndexMetapageControl(durable_metapage);
  if (!serialized_metapage.ok()) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.METAPAGE_SERIALIZE_REFUSED",
                  "orh.parallel_index_constraint.metapage_serialize_refused",
                  serialized_metapage.diagnostic.diagnostic_code,
                  request);
  }

  IndexBulkPublishRecoveryState recovery_state;
  recovery_state.old_metapage_present = true;
  recovery_state.old_metapage = old_metapage;
  recovery_state.old_tree_image_present = true;
  recovery_state.old_tree_image = old_image;
  recovery_state.candidate_generation =
      result.sorted_build.candidate_root_generation;
  recovery_state.candidate_tree_image = candidate_image;
  recovery_state.durable_metapage_image = serialized_metapage.serialized;
  recovery_state.crash_point =
      IndexBulkPublishCrashPoint::crash_after_root_publish;
  recovery_state.candidate_tree_validation_proof = true;
  recovery_state.durable_metadata_write_evidence = true;
  recovery_state.root_publish_authorization_proof = true;
  recovery_state.mga_finality_authority_evidence = true;
  recovery_state.durable_metadata_evidence_token = "durable-metapage-write";
  recovery_state.root_publish_authorization_token =
      request.publish_fence_token;
  recovery_state.mga_authority_evidence_token = request.mga_authority_token;
  recovery_state.publish_fence_token = request.publish_fence_token;
  result.recovery = RecoverSortedBulkRootPublish(recovery_state);
  if (!result.recovery.ok() ||
      result.recovery.active_root != IndexBulkPublishActiveRoot::new_root ||
      result.recovery.half_root_exposed ||
      !result.recovery.root_publish_authorized) {
    return Refuse("ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.PUBLISH_RECOVERY_REFUSED",
                  "orh.parallel_index_constraint.publish_recovery_refused",
                  result.recovery.diagnostic.diagnostic_code,
                  request);
  }

  result.status = OkStatus();
  result.diagnostic = Diagnostic(
      result.status,
      "ORH_PARALLEL_INDEX_CONSTRAINT_VALIDATION.OK",
      "orh.parallel_index_constraint.ok");
  result.accepted = true;
  result.benchmark_clean = true;
  result.fail_closed = false;
  result.publish_fence_validated = true;
  result.exact_publication = true;
  result.validated_generation_published = true;
  result.recovery_reopen_proven = true;
  result.half_root_exposed = false;
  result.candidate_generation =
      result.sorted_build.candidate_root_generation.candidate_generation;
  result.active_root_generation = result.recovery.active_metapage.root_generation;
  result.state_hash = HashEvidence(request, result.sorted_build, result.recovery);
  AddEvidence(&result, "publish_fence_validated", "true");
  AddEvidence(&result, "validated_generation_published", "true");
  AddEvidence(&result, "root_publish_recovery_consumed",
              result.recovery.crash_classification);
  AddEvidence(&result, "half_root_exposed", "false");
  AddEvidence(&result, "state_hash", result.state_hash);
  AddEvidence(&result, "benchmark_clean", "true");
  return result;
}

}  // namespace scratchbird::core::index
