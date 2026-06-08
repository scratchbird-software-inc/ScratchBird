// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_bulk_publish_recovery.hpp"

// SB-INDEX-BULK-PUBLISH-RECOVERY-ANCHOR

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
                        "core.index.bulk_publish_recovery",
                        status.ok() ? "" : "refuse unsafe bulk root reopen");
}

void AddEvidence(IndexBulkPublishRecoveryResult* result,
                 std::string kind,
                 std::string id) {
  if (result != nullptr) {
    result->evidence.push_back({std::move(kind), std::move(id)});
  }
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

const char* CrashPointName(IndexBulkPublishCrashPoint point) {
  switch (point) {
    case IndexBulkPublishCrashPoint::crash_before_root_publish:
      return "crash_before_root_publish";
    case IndexBulkPublishCrashPoint::crash_during_root_publish:
      return "crash_during_root_publish";
    case IndexBulkPublishCrashPoint::crash_after_root_publish:
      return "crash_after_root_publish";
  }
  return "unknown";
}

void AddRequiredEvidence(IndexBulkPublishRecoveryResult* result) {
  AddEvidence(result,
              "old_or_new_validated_root_only",
              result->old_or_new_validated_root_only ? "true" : "false");
  AddEvidence(result, "half_root_exposed", "false");
  AddEvidence(result,
              "orphan_generation_classified",
              result->orphan_generation_classified ? "true" : "false");
  AddEvidence(result, "repair_classification", result->repair_classification);
  AddEvidence(result,
              "root_publish_authorized",
              result->root_publish_authorized ? "true" : "false");
  AddEvidence(result, "visibility_authority", "engine_owned_not_tree_repair");
  AddEvidence(result, "authorization_authority", "engine_owned_not_tree_repair");
  AddEvidence(result, "transaction_finality_authority", "false");
  AddEvidence(result, "recovery_classification_authority", "engine_owned");
  AddEvidence(result, "tree_repair_authority", "structural_only");
  AddEvidence(result, "runtime_route_capability", "false");
  AddEvidence(result, "benchmark_clean", "false");
}

IndexBulkPublishRecoveryResult Refuse(
    std::string code,
    std::string key,
    std::string detail,
    IndexBulkPublishActiveRoot active_root,
    const IndexMetapageControl& active_metapage,
    const page::IndexBtreePhysicalTreeImage& active_tree_image,
    std::string crash_classification,
    std::string repair_classification,
    bool old_or_new_validated_root_only) {
  IndexBulkPublishRecoveryResult result;
  result.status = ErrorStatus();
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(key),
                                 std::move(detail));
  result.active_root = active_root;
  result.active_metapage = active_metapage;
  result.active_tree_image = active_tree_image;
  result.crash_classification = std::move(crash_classification);
  result.repair_classification = std::move(repair_classification);
  result.recovered = false;
  result.fail_closed = true;
  result.old_or_new_validated_root_only = old_or_new_validated_root_only;
  result.half_root_exposed = false;
  result.orphan_generation_classified =
      !result.repair_classification.empty() &&
      result.repair_classification != "unclassified";
  result.root_publish_authorized = false;
  result.transaction_finality_authority = false;
  result.runtime_route_capability = false;
  result.benchmark_clean = false;
  AddEvidence(&result, "crash_point", "fail_closed");
  AddEvidence(&result, "crash_classification", result.crash_classification);
  AddRequiredEvidence(&result);
  return result;
}

IndexBulkPublishRecoveryResult Ok(
    IndexBulkPublishActiveRoot active_root,
    const IndexMetapageControl& active_metapage,
    page::IndexBtreePhysicalTreeImage active_tree_image,
    page::IndexBtreePhysicalTreeImage repaired_tree_image,
    std::string crash_classification,
    std::string repair_classification,
    bool root_publish_authorized) {
  IndexBulkPublishRecoveryResult result;
  result.status = OkStatus();
  result.diagnostic = Diagnostic(result.status,
                                 "SB-INDEX-BULK-PUBLISH-RECOVERY-OK",
                                 "index.bulk_publish_recovery.ok");
  result.active_root = active_root;
  result.active_metapage = active_metapage;
  result.active_tree_image = std::move(active_tree_image);
  result.repaired_tree_image = std::move(repaired_tree_image);
  result.crash_classification = std::move(crash_classification);
  result.repair_classification = std::move(repair_classification);
  result.recovered = true;
  result.fail_closed = false;
  result.old_or_new_validated_root_only = true;
  result.half_root_exposed = false;
  result.orphan_generation_classified = true;
  result.root_publish_authorized = root_publish_authorized;
  result.transaction_finality_authority = false;
  result.runtime_route_capability = false;
  result.benchmark_clean = false;
  AddEvidence(&result, "crash_classification", result.crash_classification);
  AddRequiredEvidence(&result);
  return result;
}

struct ValidatedImage {
  page::IndexBtreePhysicalTreeImage image;
  page::IndexBtreePhysicalTree tree;
  page::IndexBtreePhysicalTreeReport report;
};

std::optional<IndexBulkPublishRecoveryResult> ValidateMetapagePresent(
    const IndexBulkPublishRecoveryState& state) {
  if (!state.old_metapage_present ||
      !state.old_metapage.index_uuid.valid() ||
      state.old_metapage.family == IndexFamily::unknown) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-OLD-METAPAGE-MISSING",
                  "index.bulk_publish_recovery.old_metapage_missing",
                  {},
                  IndexBulkPublishActiveRoot::none,
                  {},
                  {},
                  "fail_closed_missing_old_metapage",
                  "unclassified",
                  false);
  }
  return std::nullopt;
}

std::optional<IndexBulkPublishRecoveryResult> ValidateTreeImage(
    const page::IndexBtreePhysicalTreeImage& image,
    const IndexMetapageControl& metapage,
    std::string diagnostic_prefix,
    std::string message_prefix,
    bool require_all_pages_reachable,
    ValidatedImage* out) {
  const auto imported = page::ImportIndexBtreePhysicalTreeImage(image);
  if (!imported.ok()) {
    return Refuse(diagnostic_prefix + "-TREE-IMAGE-INVALID",
                  message_prefix + ".tree_image_invalid",
                  imported.diagnostic.diagnostic_code,
                  IndexBulkPublishActiveRoot::none,
                  {},
                  {},
                  "fail_closed_tree_image_invalid",
                  "unclassified",
                  false);
  }
  if (!SameUuid(metapage.index_uuid, imported.tree.index_uuid) ||
      metapage.root_page_number != imported.tree.root_page_number) {
    return Refuse(diagnostic_prefix + "-METAPAGE-TREE-MISMATCH",
                  message_prefix + ".metapage_tree_mismatch",
                  {},
                  IndexBulkPublishActiveRoot::none,
                  {},
                  {},
                  "fail_closed_metapage_tree_mismatch",
                  "unclassified",
                  false);
  }
  const auto report = page::BuildIndexBtreePhysicalTreeReport(imported.tree);
  if (!report.ok() || !report.report.valid) {
    return Refuse(diagnostic_prefix + "-TREE-INVALID",
                  message_prefix + ".tree_invalid",
                  report.diagnostic.diagnostic_code,
                  IndexBulkPublishActiveRoot::none,
                  {},
                  {},
                  "fail_closed_tree_invalid",
                  "unclassified",
                  false);
  }
  if (require_all_pages_reachable &&
      report.report.reachable_page_count != report.report.page_count) {
    return Refuse(diagnostic_prefix + "-TREE-ORPHAN-PAGES-UNSAFE",
                  message_prefix + ".tree_orphan_pages_unsafe",
                  {},
                  IndexBulkPublishActiveRoot::none,
                  {},
                  {},
                  "fail_closed_old_tree_invalid",
                  "unclassified",
                  false);
  }
  if (metapage.page_count != 0 &&
      metapage.page_count != report.report.reachable_page_count) {
    return Refuse(diagnostic_prefix + "-METAPAGE-PAGE-COUNT-MISMATCH",
                  message_prefix + ".metapage_page_count_mismatch",
                  {},
                  IndexBulkPublishActiveRoot::none,
                  {},
                  {},
                  "fail_closed_metapage_tree_mismatch",
                  "unclassified",
                  false);
  }

  out->image = image;
  out->tree = imported.tree;
  out->report = report.report;
  return std::nullopt;
}

std::optional<IndexBulkPublishRecoveryResult> ValidateOldTree(
    const IndexBulkPublishRecoveryState& state,
    ValidatedImage* old_image) {
  if (!state.old_tree_image_present) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-OLD-TREE-MISSING",
                  "index.bulk_publish_recovery.old_tree_missing",
                  {},
                  IndexBulkPublishActiveRoot::none,
                  state.old_metapage,
                  {},
                  "fail_closed_missing_old_tree",
                  "unclassified",
                  false);
  }
  auto invalid = ValidateTreeImage(
      state.old_tree_image,
      state.old_metapage,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-OLD",
      "index.bulk_publish_recovery.old",
      true,
      old_image);
  if (invalid.has_value()) {
    invalid->active_metapage = state.old_metapage;
    invalid->crash_classification = "fail_closed_invalid_old_tree";
    return invalid;
  }
  return std::nullopt;
}

page::IndexBtreePhysicalTreeImage CandidateImage(
    const SortedBulkIndexCandidateRootGeneration& candidate,
    const std::optional<page::IndexBtreePhysicalTreeImage>& image) {
  if (image.has_value()) {
    return *image;
  }
  page::IndexBtreePhysicalTreeImage candidate_image;
  candidate_image.page_size = candidate.tree.page_size;
  candidate_image.index_uuid = candidate.tree.index_uuid;
  candidate_image.root_page_number = candidate.tree.root_page_number;
  candidate_image.next_page_number = candidate.tree.next_page_number;
  candidate_image.pages = candidate.tree.pages;
  return candidate_image;
}

std::optional<IndexBulkPublishRecoveryResult> ValidateCandidate(
    const IndexBulkPublishRecoveryState& state,
    const ValidatedImage& old_image,
    ValidatedImage* candidate_image) {
  if (!state.candidate_generation.has_value()) {
    return std::nullopt;
  }
  const auto& candidate = *state.candidate_generation;
  if (!candidate.created || candidate.candidate_generation == 0 ||
      !candidate.physical_leaf_pack ||
      !candidate.candidate_root_page_present ||
      !state.candidate_tree_validation_proof ||
      !candidate.validated_tree || !candidate.report.valid) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-TREE-INVALID",
                  "index.bulk_publish_recovery.candidate_tree_invalid",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_invalid_candidate_tree",
                  "unclassified",
                  true);
  }
  if (!SameUuid(state.old_metapage.index_uuid, candidate.tree.index_uuid) ||
      candidate.root_page_number == 0 ||
      candidate.root_page_number != candidate.tree.root_page_number) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-TREE-MISMATCH",
                  "index.bulk_publish_recovery.candidate_tree_mismatch",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_invalid_candidate_tree",
                  "unclassified",
                  true);
  }
  if (candidate.candidate_generation <= state.old_metapage.root_generation) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-STALE-GENERATION",
                  "index.bulk_publish_recovery.stale_generation",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_stale_generation",
                  "unclassified",
                  true);
  }

  IndexMetapageControl candidate_metapage = state.old_metapage;
  candidate_metapage.root_page_number = candidate.root_page_number;
  candidate_metapage.root_generation = candidate.candidate_generation;
  candidate_metapage.page_count = candidate.page_count;
  candidate_metapage.tuple_count_estimate = candidate.live_entry_count;

  auto image = CandidateImage(candidate, state.candidate_tree_image);
  auto invalid = ValidateTreeImage(
      image,
      candidate_metapage,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE",
      "index.bulk_publish_recovery.candidate",
      false,
      candidate_image);
  if (invalid.has_value()) {
    return Refuse(invalid->diagnostic.diagnostic_code,
                  invalid->diagnostic.message_key,
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_invalid_candidate_tree",
                  "unclassified",
                  true);
  }
  if (candidate_image->report.reachable_page_count != candidate.page_count ||
      candidate_image->report.tuple_live_entry_estimate !=
          candidate.live_entry_count) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-REPORT-MISMATCH",
                  "index.bulk_publish_recovery.candidate_report_mismatch",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_invalid_candidate_tree",
                  "unclassified",
                  true);
  }
  return std::nullopt;
}

bool HasPublishProofs(const IndexBulkPublishRecoveryState& state) {
  return state.durable_metadata_write_evidence &&
         state.root_publish_authorization_proof &&
         state.mga_finality_authority_evidence &&
         !state.durable_metadata_evidence_token.empty() &&
         !state.root_publish_authorization_token.empty() &&
         !state.mga_authority_evidence_token.empty() &&
         !state.publish_fence_token.empty();
}

std::optional<IndexBulkPublishRecoveryResult> DurableMetapageForCandidate(
    const IndexBulkPublishRecoveryState& state,
    const ValidatedImage& old_image,
    const ValidatedImage& candidate_image,
    IndexMetapageControl* durable) {
  if (!state.candidate_generation.has_value()) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-MISSING",
                  "index.bulk_publish_recovery.candidate_missing",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_missing_candidate_generation",
                  "unclassified",
                  true);
  }
  if (!state.durable_metapage_image.has_value()) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-DURABLE-METAPAGE-MISSING",
                  "index.bulk_publish_recovery.durable_metapage_missing",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_missing_durable_metapage",
                  "unclassified",
                  true);
  }
  const auto parsed = ParseIndexMetapageControl(*state.durable_metapage_image);
  if (!parsed.ok()) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-DURABLE-METAPAGE-INVALID",
                  "index.bulk_publish_recovery.durable_metapage_invalid",
                  parsed.diagnostic.diagnostic_code,
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_bad_durable_metapage_parse",
                  "unclassified",
                  true);
  }

  const auto& candidate = *state.candidate_generation;
  if (!SameUuid(parsed.control.index_uuid, state.old_metapage.index_uuid) ||
      parsed.control.family != state.old_metapage.family ||
      parsed.control.root_page_number != candidate.root_page_number ||
      parsed.control.root_generation != candidate.candidate_generation ||
      parsed.control.root_page_number != candidate_image.tree.root_page_number ||
      parsed.control.page_count != candidate_image.report.reachable_page_count ||
      parsed.control.tuple_count_estimate !=
          candidate_image.report.tuple_live_entry_estimate) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-METAPAGE-TREE-MISMATCH",
                  "index.bulk_publish_recovery.metapage_tree_mismatch",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_metapage_tree_root_mismatch",
                  "unclassified",
                  true);
  }
  if (parsed.control.root_generation <= state.old_metapage.root_generation) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-STALE-GENERATION",
                  "index.bulk_publish_recovery.stale_generation",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_stale_generation",
                  "unclassified",
                  true);
  }
  if (!HasPublishProofs(state)) {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-PUBLISH-EVIDENCE-MISSING",
                  "index.bulk_publish_recovery.publish_evidence_missing",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_missing_publish_fence_or_mga_evidence",
                  "unclassified",
                  true);
  }
  *durable = parsed.control;
  return std::nullopt;
}

page::IndexBtreePhysicalTree TreeFromImage(
    const page::IndexBtreePhysicalTreeImage& image) {
  page::IndexBtreePhysicalTree tree;
  tree.page_size = image.page_size;
  tree.index_uuid = image.index_uuid;
  tree.root_page_number = image.root_page_number;
  tree.next_page_number = image.next_page_number;
  tree.pages = image.pages;
  return tree;
}

std::string ClassifyRepair(
    IndexBulkPublishActiveRoot active_root,
    const ValidatedImage* candidate_image,
    page::IndexBtreePhysicalTreeImage* repaired_image) {
  if (candidate_image == nullptr) {
    return "no_candidate_generation_to_classify";
  }
  const bool has_orphan_pages =
      candidate_image->report.reachable_page_count <
      candidate_image->report.page_count;
  if (!has_orphan_pages) {
    if (active_root == IndexBulkPublishActiveRoot::new_root) {
      return "no_orphan_generation_new_root_validated_old_root_retained";
    }
    return "orphan_unpublished_generation_discarded";
  }

  const auto repair =
      page::RepairIndexBtreePhysicalTree(TreeFromImage(candidate_image->image));
  if (repair.ok() && repair.repaired) {
    *repaired_image = repair.image;
    if (active_root == IndexBulkPublishActiveRoot::new_root) {
      return "published_generation_rebuilt_from_reachable_live_cells";
    }
    return "orphan_unpublished_generation_rebuilt_from_reachable_live_cells";
  }
  if (active_root == IndexBulkPublishActiveRoot::new_root) {
    return "published_generation_orphan_repair_refused";
  }
  return "orphan_unpublished_generation_repair_refused";
}

}  // namespace

IndexBulkPublishRecoveryResult RecoverSortedBulkRootPublish(
    const IndexBulkPublishRecoveryState& state) {
  if (auto invalid = ValidateMetapagePresent(state); invalid.has_value()) {
    return *invalid;
  }

  ValidatedImage old_image;
  if (auto invalid = ValidateOldTree(state, &old_image); invalid.has_value()) {
    return *invalid;
  }

  ValidatedImage candidate_image;
  ValidatedImage* candidate_ptr = nullptr;
  if (state.candidate_generation.has_value()) {
    if (auto invalid = ValidateCandidate(state, old_image, &candidate_image);
        invalid.has_value()) {
      return *invalid;
    }
    candidate_ptr = &candidate_image;
  }

  page::IndexBtreePhysicalTreeImage repaired_image;
  const std::string crash_point = CrashPointName(state.crash_point);

  if (state.crash_point ==
      IndexBulkPublishCrashPoint::crash_before_root_publish) {
    const auto repair_classification =
        ClassifyRepair(IndexBulkPublishActiveRoot::old_root,
                       candidate_ptr,
                       &repaired_image);
    if (repair_classification ==
        "orphan_unpublished_generation_repair_refused") {
      return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-ORPHAN-REPAIR-REFUSED",
                    "index.bulk_publish_recovery.orphan_repair_refused",
                    {},
                    IndexBulkPublishActiveRoot::old_root,
                    state.old_metapage,
                    old_image.image,
                    "fail_closed_orphan_repair_refused",
                    repair_classification,
                    true);
    }
    return Ok(IndexBulkPublishActiveRoot::old_root,
              state.old_metapage,
              old_image.image,
              repaired_image,
              crash_point + ":old_root_active_candidate_orphan_classified",
              repair_classification,
              false);
  }

  if (state.crash_point ==
      IndexBulkPublishCrashPoint::crash_during_root_publish &&
      !state.durable_metapage_image.has_value()) {
    const auto repair_classification =
        ClassifyRepair(IndexBulkPublishActiveRoot::old_root,
                       candidate_ptr,
                       &repaired_image);
    if (repair_classification ==
        "orphan_unpublished_generation_repair_refused") {
      return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-ORPHAN-REPAIR-REFUSED",
                    "index.bulk_publish_recovery.orphan_repair_refused",
                    {},
                    IndexBulkPublishActiveRoot::old_root,
                    state.old_metapage,
                    old_image.image,
                    "fail_closed_orphan_repair_refused",
                    repair_classification,
                    true);
    }
    return Ok(IndexBulkPublishActiveRoot::old_root,
              state.old_metapage,
              old_image.image,
              repaired_image,
              crash_point + ":durable_metapage_absent_old_root_active",
              repair_classification,
              false);
  }

  IndexMetapageControl durable_metapage;
  if (auto invalid = DurableMetapageForCandidate(state,
                                                 old_image,
                                                 candidate_image,
                                                 &durable_metapage);
      invalid.has_value()) {
    return *invalid;
  }

  const auto repair_classification =
      ClassifyRepair(IndexBulkPublishActiveRoot::new_root,
                     candidate_ptr,
                     &repaired_image);
  if (repair_classification == "published_generation_orphan_repair_refused") {
    return Refuse("SB-INDEX-BULK-PUBLISH-RECOVERY-ORPHAN-REPAIR-REFUSED",
                  "index.bulk_publish_recovery.orphan_repair_refused",
                  {},
                  IndexBulkPublishActiveRoot::old_root,
                  state.old_metapage,
                  old_image.image,
                  "fail_closed_orphan_repair_refused",
                  repair_classification,
                  true);
  }
  page::IndexBtreePhysicalTreeImage active_image = candidate_image.image;
  IndexMetapageControl active_metapage = durable_metapage;
  if (!repaired_image.pages.empty()) {
    active_image = repaired_image;
    const auto repaired_import =
        page::ImportIndexBtreePhysicalTreeImage(active_image);
    if (!repaired_import.ok()) {
      return Refuse(
          "SB-INDEX-BULK-PUBLISH-RECOVERY-REPAIRED-IMAGE-INVALID",
          "index.bulk_publish_recovery.repaired_image_invalid",
          repaired_import.diagnostic.diagnostic_code,
          IndexBulkPublishActiveRoot::old_root,
          state.old_metapage,
          old_image.image,
          "fail_closed_repaired_image_invalid",
          repair_classification,
          true);
    }
    const auto repaired_report =
        page::BuildIndexBtreePhysicalTreeReport(repaired_import.tree);
    if (!repaired_report.ok() || !repaired_report.report.valid) {
      return Refuse(
          "SB-INDEX-BULK-PUBLISH-RECOVERY-REPAIRED-IMAGE-INVALID",
          "index.bulk_publish_recovery.repaired_image_invalid",
          repaired_report.diagnostic.diagnostic_code,
          IndexBulkPublishActiveRoot::old_root,
          state.old_metapage,
          old_image.image,
          "fail_closed_repaired_image_invalid",
          repair_classification,
          true);
    }
    active_metapage.root_page_number = repaired_import.tree.root_page_number;
    active_metapage.page_count = repaired_report.report.reachable_page_count;
    active_metapage.tuple_count_estimate =
        repaired_report.report.tuple_live_entry_estimate;
  }
  return Ok(IndexBulkPublishActiveRoot::new_root,
            active_metapage,
            std::move(active_image),
            repaired_image,
            crash_point + ":new_root_active_validated",
            repair_classification,
            true);
}

}  // namespace scratchbird::core::index
