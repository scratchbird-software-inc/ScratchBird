// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_root_generation_publish.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::core::index {
namespace {

namespace page = scratchbird::storage::page;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

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
                        "core.index.root_generation_publish",
                        status.ok() ? "" : "refuse root generation publish");
}

void AddEvidence(IndexRootGenerationPublishResult* result,
                 std::string kind,
                 std::string id) {
  if (result != nullptr) {
    result->evidence.push_back({std::move(kind), std::move(id)});
  }
}

void AddNonAuthorityEvidence(IndexRootGenerationPublishResult* result) {
  AddEvidence(result, "root_publish_authorized",
              result->root_publish_authorized ? "true" : "false");
  AddEvidence(result, "physical_append_authorized", "false");
  AddEvidence(result, "transaction_finality_authority", "false");
  AddEvidence(result, "recovery_authority", "false");
  AddEvidence(result, "runtime_route_capability", "false");
  AddEvidence(result, "irc_042_crash_repair_classification", "pending");
}

IndexRootGenerationPublishResult Refuse(
    const IndexRootGenerationPublishRequest& request,
    std::string code,
    std::string key,
    std::string detail = {}) {
  IndexRootGenerationPublishResult result;
  result.status = ErrorStatus();
  result.old_metapage = request.current_metapage;
  result.root_publish_authorized = false;
  result.physical_append_authorized = false;
  result.transaction_finality_authority = false;
  result.recovery_authority = false;
  result.runtime_route_capability = false;
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(key),
                                 std::move(detail));
  AddNonAuthorityEvidence(&result);
  return result;
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool ContainsPage(const page::IndexBtreePhysicalTree& tree,
                  u64 page_number) {
  return std::any_of(tree.pages.begin(),
                     tree.pages.end(),
                     [page_number](const auto& image) {
                       return image.page_number == page_number;
                     });
}

u64 CandidatePageCount(const SortedBulkIndexCandidateRootGeneration& candidate,
                       const page::IndexBtreePhysicalTreeReport& report) {
  if (candidate.page_count != 0) {
    return candidate.page_count;
  }
  if (report.page_count != 0) {
    return report.page_count;
  }
  return static_cast<u64>(candidate.tree.pages.size());
}

bool MetapageMatchesExpected(const IndexMetapageControl& actual,
                             const IndexMetapageControl& expected) {
  return SameUuid(actual.index_uuid, expected.index_uuid) &&
         actual.family == expected.family &&
         actual.root_page_number == expected.root_page_number &&
         actual.resource_epoch == expected.resource_epoch &&
         actual.mutation_epoch == expected.mutation_epoch &&
         actual.root_generation == expected.root_generation &&
         actual.page_count == expected.page_count &&
         actual.tuple_count_estimate == expected.tuple_count_estimate &&
         actual.layout_version == expected.layout_version &&
         actual.flags == expected.flags &&
         actual.semantic_profile_id == expected.semantic_profile_id;
}

}  // namespace

IndexRootGenerationPublishResult PublishIndexRootGeneration(
    const IndexRootGenerationPublishRequest& request) {
  const auto& candidate = request.candidate;
  if (!request.current_metapage.index_uuid.valid() ||
      request.current_metapage.family == IndexFamily::unknown) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CURRENT-METAPAGE-INVALID",
                  "index.root_publish.current_metapage_invalid");
  }
  if (!candidate.created || candidate.candidate_generation == 0) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-GENERATION-MISSING",
                  "index.root_publish.candidate_generation_missing");
  }
  if (!candidate.physical_leaf_pack ||
      !candidate.candidate_root_page_present) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-PHYSICAL-PROOF-MISSING",
                  "index.root_publish.candidate_physical_proof_missing");
  }
  if (!request.candidate_tree_validation_proof ||
      !candidate.validated_tree || !candidate.report.valid) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-TREE-UNVALIDATED",
                  "index.root_publish.candidate_tree_unvalidated");
  }
  if (candidate.root_page_number == 0) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-ROOT-MISSING",
                  "index.root_publish.candidate_root_missing");
  }
  if (candidate.root_page_number != candidate.tree.root_page_number) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-ROOT-PAGE-MISMATCH",
                  "index.root_publish.root_page_mismatch");
  }
  if (!ContainsPage(candidate.tree, candidate.root_page_number)) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-ROOT-MISSING",
                  "index.root_publish.candidate_root_missing");
  }
  if (!SameUuid(request.current_metapage.index_uuid,
                candidate.tree.index_uuid)) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-INDEX-UUID-MISMATCH",
                  "index.root_publish.index_uuid_mismatch");
  }
  if (candidate.candidate_generation <=
      request.current_metapage.root_generation) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-STALE-GENERATION",
                  "index.root_publish.stale_generation");
  }
  if (!request.durable_metadata_write_evidence ||
      request.durable_metadata_evidence_token.empty()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-DURABLE-METADATA-EVIDENCE-MISSING",
                  "index.root_publish.durable_metadata_evidence_missing");
  }
  if (request.publish_fence_token.empty()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-FENCE-MISSING",
                  "index.root_publish.fence_missing");
  }
  if (!request.mga_finality_authority_evidence ||
      request.mga_authority_evidence_token.empty()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-MGA-AUTHORITY-PROOF-MISSING",
                  "index.root_publish.mga_authority_proof_missing");
  }

  const auto validation = page::ValidateIndexBtreePhysicalTree(candidate.tree);
  if (!validation.ok()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-TREE-UNVALIDATED",
                  "index.root_publish.candidate_tree_unvalidated",
                  validation.diagnostic.diagnostic_code);
  }
  if (validation.live_entry_count != candidate.live_entry_count) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-REPORT-MISMATCH",
                  "index.root_publish.candidate_report_mismatch",
                  "live_entry_count");
  }

  const auto report = page::BuildIndexBtreePhysicalTreeReport(candidate.tree);
  if (!report.ok() || !report.report.valid) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-TREE-UNVALIDATED",
                  "index.root_publish.candidate_tree_unvalidated",
                  report.diagnostic.diagnostic_code);
  }
  if (report.report.root_page_number != candidate.root_page_number) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-ROOT-PAGE-MISMATCH",
                  "index.root_publish.root_page_mismatch");
  }
  if (candidate.report.root_page_number != report.report.root_page_number ||
      candidate.report.page_count != report.report.page_count ||
      candidate.report.tuple_live_entry_estimate !=
          report.report.tuple_live_entry_estimate ||
      (candidate.page_count != 0 &&
       candidate.page_count != report.report.page_count) ||
      candidate.live_entry_count != report.report.tuple_live_entry_estimate) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-CANDIDATE-REPORT-MISMATCH",
                  "index.root_publish.candidate_report_mismatch");
  }

  IndexMetapageControl expected = request.current_metapage;
  expected.root_page_number = candidate.root_page_number;
  expected.root_generation = candidate.candidate_generation;
  expected.page_count = CandidatePageCount(candidate, report.report);
  expected.tuple_count_estimate = report.report.tuple_live_entry_estimate;
  expected.resource_epoch =
      std::max(request.current_metapage.resource_epoch + 1,
               candidate.candidate_generation);
  expected.mutation_epoch =
      std::max(request.current_metapage.mutation_epoch + 1,
               candidate.candidate_generation);

  const auto built_metapage = BuildIndexMetapageControl(expected);
  if (!built_metapage.ok()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-METAPAGE-BUILD-REFUSED",
                  "index.root_publish.metapage_build_refused",
                  built_metapage.diagnostic.diagnostic_code);
  }

  const auto& durable_image =
      request.durable_metapage_image.empty() ? built_metapage.serialized
                                             : request.durable_metapage_image;
  const auto parsed_metapage = ParseIndexMetapageControl(durable_image);
  if (!parsed_metapage.ok()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-METAPAGE-REOPEN-INVALID",
                  "index.root_publish.metapage_reopen_invalid",
                  parsed_metapage.diagnostic.diagnostic_code);
  }
  if (!MetapageMatchesExpected(parsed_metapage.control, expected)) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-METAPAGE-REOPEN-MISMATCH",
                  "index.root_publish.metapage_reopen_mismatch");
  }

  const auto exported = page::ExportIndexBtreePhysicalTreeImage(candidate.tree);
  if (!exported.ok()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-TREE-IMAGE-EXPORT-REFUSED",
                  "index.root_publish.tree_image_export_refused",
                  exported.diagnostic.diagnostic_code);
  }
  const auto imported = page::ImportIndexBtreePhysicalTreeImage(exported.image);
  if (!imported.ok()) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-TREE-IMAGE-REOPEN-INVALID",
                  "index.root_publish.tree_image_reopen_invalid",
                  imported.diagnostic.diagnostic_code);
  }
  const auto reopened_validation =
      page::ValidateIndexBtreePhysicalTree(imported.tree);
  if (!reopened_validation.ok() ||
      imported.tree.root_page_number != expected.root_page_number) {
    return Refuse(request,
                  "SB-INDEX-ROOT-PUBLISH-TREE-IMAGE-REOPEN-INVALID",
                  "index.root_publish.tree_image_reopen_invalid",
                  reopened_validation.diagnostic.diagnostic_code);
  }

  IndexRootGenerationPublishResult result;
  result.status = OkStatus();
  result.diagnostic = Diagnostic(result.status,
                                 "SB-INDEX-ROOT-PUBLISH-OK",
                                 "index.root_publish.ok");
  result.old_metapage = request.current_metapage;
  result.published_metapage = expected;
  result.reopened_metapage = parsed_metapage.control;
  result.published_metapage_image = durable_image;
  result.published_tree_image = exported.image;
  result.published = true;
  result.root_publish_authorized = true;
  result.physical_append_authorized = false;
  result.transaction_finality_authority = false;
  result.recovery_authority = false;
  result.runtime_route_capability = false;
  result.rollback_safe_metadata_contract = true;
  result.reopen_safe_metadata_contract = true;
  result.old_root_metadata_preserved = true;

  AddEvidence(&result, "candidate_tree_valid", "true");
  AddEvidence(&result, "candidate_root_page_present", "true");
  AddEvidence(&result, "candidate_generation",
              std::to_string(candidate.candidate_generation));
  AddEvidence(&result, "previous_root_generation",
              std::to_string(request.current_metapage.root_generation));
  AddEvidence(&result, "previous_root_page_number",
              std::to_string(request.current_metapage.root_page_number));
  AddEvidence(&result, "published_root_page_number",
              std::to_string(expected.root_page_number));
  AddEvidence(&result, "published_page_count",
              std::to_string(expected.page_count));
  AddEvidence(&result, "published_tuple_count_estimate",
              std::to_string(expected.tuple_count_estimate));
  AddEvidence(&result, "durable_metadata_write_evidence", "true");
  AddEvidence(&result,
              "durable_metadata_evidence_token",
              request.durable_metadata_evidence_token);
  AddEvidence(&result, "mga_finality_authority_evidence", "true");
  AddEvidence(&result,
              "mga_authority_evidence_token",
              request.mga_authority_evidence_token);
  AddEvidence(&result, "publish_fence_token", request.publish_fence_token);
  AddEvidence(&result, "metapage_reopen_validated", "true");
  AddEvidence(&result, "tree_image_reopen_validated", "true");
  AddEvidence(&result, "old_root_metadata_preserved_until_mga_finality", "true");
  AddEvidence(&result, "rollback_safe_metadata_contract", "true");
  AddEvidence(&result, "reopen_safe_metadata_contract", "true");
  AddNonAuthorityEvidence(&result);
  return result;
}

}  // namespace scratchbird::core::index
