// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_bulk_publish_recovery.hpp"
#include "index_root_generation_publish.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_bulk_publish_recovery_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1810002000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

std::string UuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(GeneratedUuid(kind, salt).value);
}

std::string Key(char group, char suffix) {
  std::string key = "SBKO";
  key.push_back(static_cast<char>(0x7f));
  key.push_back(group);
  key.push_back(suffix);
  return key;
}

idx::SortedBulkIndexRowInput Row(char group,
                                 char suffix,
                                 platform::u64 salt) {
  idx::SortedBulkIndexRowInput row;
  row.encoded_key = Key(group, suffix);
  row.row_uuid = UuidText(platform::UuidKind::row, salt);
  row.version_uuid = UuidText(platform::UuidKind::row, salt + 1000);
  row.payload_value = "payload";
  row.source_ordinal = salt;
  return row;
}

idx::SortedBulkIndexCandidateRootGeneration BuildGeneration(
    platform::TypedUuid index_uuid,
    platform::u64 salt,
    std::string_view profile) {
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = index_uuid;
  request.metadata.table_uuid =
      GeneratedUuid(platform::UuidKind::object, salt + 30);
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = std::string(profile);
  request.metadata.physical_page_size = 1024;
  request.metadata.leaf_entry_capacity = 2;
  request.metadata.internal_entry_capacity = 2;
  request.rows = {Row('d', static_cast<char>('0' + salt % 7), salt + 1),
                  Row('a', static_cast<char>('0' + salt % 7), salt + 2),
                  Row('b', static_cast<char>('0' + salt % 7), salt + 3),
                  Row('c', static_cast<char>('0' + salt % 7), salt + 4),
                  Row('e', static_cast<char>('0' + salt % 7), salt + 5)};
  const auto built = idx::BuildSortedExactBulkIndex(request);
  Require(built.ok(), "sorted bulk generation build refused");
  Require(built.candidate_root_generation.created,
          "candidate root generation missing");
  Require(built.candidate_root_generation.validated_tree,
          "candidate tree validation flag missing");
  return built.candidate_root_generation;
}

page::IndexBtreePhysicalTreeImage ExportTree(
    const page::IndexBtreePhysicalTree& tree) {
  const auto exported = page::ExportIndexBtreePhysicalTreeImage(tree);
  Require(exported.ok(), "tree export failed");
  return exported.image;
}

idx::IndexMetapageControl OldMetapage(
    const idx::SortedBulkIndexCandidateRootGeneration& old_generation) {
  idx::IndexMetapageControl control;
  control.index_uuid = old_generation.tree.index_uuid;
  control.family = idx::IndexFamily::btree;
  control.root_page_number = old_generation.root_page_number;
  control.resource_epoch = 10;
  control.mutation_epoch = 12;
  control.root_generation = 0;
  control.page_count = old_generation.page_count;
  control.tuple_count_estimate = old_generation.live_entry_count;
  control.semantic_profile_id = "index_bulk_publish_recovery_gate";
  return control;
}

struct Fixture {
  idx::SortedBulkIndexCandidateRootGeneration old_generation;
  idx::SortedBulkIndexCandidateRootGeneration candidate;
  idx::IndexMetapageControl old_metapage;
  page::IndexBtreePhysicalTreeImage old_tree_image;
  idx::IndexRootGenerationPublishResult publish;
};

Fixture MakeFixture() {
  Fixture fixture;
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 10);
  fixture.old_generation =
      BuildGeneration(index_uuid, 100, "index_bulk_publish_old");
  fixture.candidate =
      BuildGeneration(index_uuid, 200, "index_bulk_publish_candidate");
  fixture.old_metapage = OldMetapage(fixture.old_generation);
  fixture.old_tree_image = ExportTree(fixture.old_generation.tree);

  idx::IndexRootGenerationPublishRequest request;
  request.current_metapage = fixture.old_metapage;
  request.candidate = fixture.candidate;
  request.candidate_tree_validation_proof = true;
  request.durable_metadata_write_evidence = true;
  request.mga_finality_authority_evidence = true;
  request.durable_metadata_evidence_token =
      "durable_metapage_write_fsync_evidence";
  request.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  request.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  fixture.publish = idx::PublishIndexRootGeneration(request);
  Require(fixture.publish.ok(), "IRC-041 publish fixture refused");
  return fixture;
}

idx::IndexBulkPublishRecoveryState BaseState(const Fixture& fixture) {
  idx::IndexBulkPublishRecoveryState state;
  state.old_metapage_present = true;
  state.old_metapage = fixture.old_metapage;
  state.old_tree_image_present = true;
  state.old_tree_image = fixture.old_tree_image;
  state.candidate_generation = fixture.candidate;
  state.candidate_tree_image = fixture.publish.published_tree_image;
  state.durable_metapage_image = fixture.publish.published_metapage_image;
  state.candidate_tree_validation_proof = true;
  state.durable_metadata_write_evidence = true;
  state.root_publish_authorization_proof = true;
  state.mga_finality_authority_evidence = true;
  state.durable_metadata_evidence_token =
      "durable_metapage_write_fsync_evidence";
  state.root_publish_authorization_token =
      "irc_041_publish_proof_succeeded";
  state.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  state.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  return state;
}

bool HasEvidence(
    const std::vector<idx::IndexBulkPublishRecoveryEvidence>& evidence,
    std::string_view kind,
    std::string_view id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id == id;
                     });
}

void RequireNoDocRuntimeEvidence(
    const std::vector<idx::IndexBulkPublishRecoveryEvidence>& evidence) {
  for (const auto& item : evidence) {
    for (const auto token : {"docs" "/execution-plans",
                             "docs" "/findings",
                             "public_release_evidence",
                             "execution_plan",
                             "findings",
                             "contracts"}) {
      Require(item.evidence_kind.find(token) == std::string::npos &&
                  item.evidence_id.find(token) == std::string::npos,
              "runtime evidence leaked documentation path token");
    }
  }
}

void RequireCoreEvidence(const idx::IndexBulkPublishRecoveryResult& result,
                         std::string_view repair_classification,
                         bool root_publish_authorized) {
  Require(result.old_or_new_validated_root_only,
          "old/new root validation evidence missing");
  Require(!result.half_root_exposed, "half-root was exposed");
  Require(result.orphan_generation_classified,
          "orphan generation classification missing");
  Require(result.repair_classification == repair_classification,
          "repair classification mismatch");
  Require(result.root_publish_authorized == root_publish_authorized,
          "root publish authorization flag mismatch");
  Require(!result.transaction_finality_authority,
          "recovery claimed transaction finality authority");
  Require(!result.runtime_route_capability,
          "recovery claimed runtime route capability");
  Require(!result.benchmark_clean, "recovery claimed benchmark-clean status");
  Require(HasEvidence(result.evidence,
                      "old_or_new_validated_root_only",
                      "true"),
          "old/new root evidence missing");
  Require(HasEvidence(result.evidence, "half_root_exposed", "false"),
          "half-root evidence missing");
  Require(HasEvidence(result.evidence,
                      "orphan_generation_classified",
                      "true"),
          "orphan classification evidence missing");
  Require(HasEvidence(result.evidence,
                      "repair_classification",
                      repair_classification),
          "exact repair classification evidence missing");
  Require(HasEvidence(result.evidence,
                      "root_publish_authorized",
                      root_publish_authorized ? "true" : "false"),
          "root publish authorization evidence missing");
  Require(HasEvidence(result.evidence,
                      "transaction_finality_authority",
                      "false"),
          "transaction finality evidence missing");
  Require(HasEvidence(result.evidence, "runtime_route_capability", "false"),
          "runtime route non-capability evidence missing");
  Require(HasEvidence(result.evidence, "benchmark_clean", "false"),
          "benchmark non-capability evidence missing");
  Require(HasEvidence(result.evidence,
                      "recovery_classification_authority",
                      "engine_owned"),
          "engine-owned recovery classification evidence missing");
  Require(HasEvidence(result.evidence,
                      "tree_repair_authority",
                      "structural_only"),
          "structural-only repair evidence missing");
  RequireNoDocRuntimeEvidence(result.evidence);
}

page::IndexBtreePhysicalPageImage BuildValidOrphanPage(
    const page::IndexBtreePhysicalTree& tree) {
  for (const auto& image : tree.pages) {
    auto fetched = page::FetchIndexBtreePhysicalPage(tree, image.page_number);
    Require(fetched.ok(), "source page fetch failed");
    if (fetched.body.tree_level != 0) {
      continue;
    }
    auto body = fetched.body;
    body.page_number = tree.next_page_number + 77;
    body.parent_page_number = 0;
    body.left_sibling_page_number = 0;
    body.right_sibling_page_number = 0;
    body.page_kind = page::IndexBtreePageKind::leaf;
    const auto built = page::BuildIndexBtreePageBody(body, tree.page_size);
    Require(built.ok(), "orphan page build failed");
    return {body.page_number, built.serialized};
  }
  Fail("no leaf page available for orphan fixture");
}

void CrashBeforePublishKeepsOldRootAndDiscardsOrphan() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_before_root_publish;
  state.durable_metapage_image.reset();
  state.durable_metadata_write_evidence = false;
  state.root_publish_authorization_proof = false;
  state.mga_finality_authority_evidence = false;
  state.durable_metadata_evidence_token.clear();
  state.root_publish_authorization_token.clear();
  state.mga_authority_evidence_token.clear();
  state.publish_fence_token.clear();

  const auto result = idx::RecoverSortedBulkRootPublish(state);
  Require(result.ok(), "crash-before recovery refused");
  Require(result.active_root == idx::IndexBulkPublishActiveRoot::old_root,
          "crash-before did not keep old root active");
  Require(result.active_metapage.root_generation ==
              fixture.old_metapage.root_generation,
          "crash-before active generation drifted");
  Require(result.active_tree_image.root_page_number ==
              fixture.old_metapage.root_page_number,
          "crash-before active root page drifted");
  RequireCoreEvidence(result,
                      "orphan_unpublished_generation_discarded",
                      false);
}

void CrashBeforePublishRepairsOrphanGeneration() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_before_root_publish;
  state.durable_metapage_image.reset();
  state.root_publish_authorization_proof = false;
  state.root_publish_authorization_token.clear();
  auto image = fixture.publish.published_tree_image;
  image.pages.push_back(BuildValidOrphanPage(fixture.candidate.tree));
  image.next_page_number = image.pages.back().page_number + 1;
  state.candidate_tree_image = image;

  const auto result = idx::RecoverSortedBulkRootPublish(state);
  Require(result.ok(), "crash-before orphan repair refused");
  Require(result.active_root == idx::IndexBulkPublishActiveRoot::old_root,
          "orphan repair exposed candidate root");
  Require(!result.repaired_tree_image.pages.empty(),
          "repaired candidate image missing");
  Require(result.repaired_tree_image.pages.size() <
              state.candidate_tree_image->pages.size(),
          "orphan repair did not remove stale page images");
  RequireCoreEvidence(
      result,
      "orphan_unpublished_generation_rebuilt_from_reachable_live_cells",
      false);
}

void CrashDuringPublishWithoutDurableMetapageKeepsOldRoot() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_during_root_publish;
  state.durable_metapage_image.reset();

  const auto result = idx::RecoverSortedBulkRootPublish(state);
  Require(result.ok(), "crash-during absent durable metapage refused");
  Require(result.active_root == idx::IndexBulkPublishActiveRoot::old_root,
          "crash-during absent durable metapage did not keep old root");
  RequireCoreEvidence(result,
                      "orphan_unpublished_generation_discarded",
                      false);
}

void CrashDuringPublishWithDurableMetapageOpensNewRoot() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_during_root_publish;

  const auto result = idx::RecoverSortedBulkRootPublish(state);
  Require(result.ok(), "crash-during durable metapage refused");
  Require(result.active_root == idx::IndexBulkPublishActiveRoot::new_root,
          "crash-during durable metapage did not activate new root");
  Require(result.active_metapage.root_generation ==
              fixture.candidate.candidate_generation,
          "crash-during new generation mismatch");
  Require(result.active_tree_image.root_page_number ==
              fixture.candidate.root_page_number,
          "crash-during new tree root mismatch");
  RequireCoreEvidence(
      result,
      "no_orphan_generation_new_root_validated_old_root_retained",
      true);
}

void CrashAfterPublishOpensValidatedNewRoot() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_after_root_publish;

  const auto result = idx::RecoverSortedBulkRootPublish(state);
  Require(result.ok(), "crash-after durable metapage refused");
  Require(result.active_root == idx::IndexBulkPublishActiveRoot::new_root,
          "crash-after did not activate new root");
  Require(result.active_metapage.root_page_number ==
              fixture.candidate.root_page_number,
          "crash-after root page mismatch");
  RequireCoreEvidence(
      result,
      "no_orphan_generation_new_root_validated_old_root_retained",
      true);
}

void CrashAfterPublishRepairsPublishedOrphanGeneration() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_after_root_publish;
  auto image = fixture.publish.published_tree_image;
  image.pages.push_back(BuildValidOrphanPage(fixture.candidate.tree));
  image.next_page_number = image.pages.back().page_number + 1;
  state.candidate_tree_image = image;

  const auto result = idx::RecoverSortedBulkRootPublish(state);
  Require(result.ok(), "crash-after published orphan repair refused");
  Require(result.active_root == idx::IndexBulkPublishActiveRoot::new_root,
          "published orphan repair did not keep new root active");
  Require(!result.repaired_tree_image.pages.empty(),
          "published orphan repair did not return a repaired image");
  Require(result.repaired_tree_image.pages.size() <
              state.candidate_tree_image->pages.size(),
          "published orphan repair did not remove stale page images");
  Require(result.active_tree_image.root_page_number ==
              result.active_metapage.root_page_number,
          "published orphan repair left metapage/tree root mismatch");
  Require(result.active_metapage.root_generation ==
              fixture.candidate.candidate_generation,
          "published orphan repair changed active generation");
  RequireCoreEvidence(result,
                      "published_generation_rebuilt_from_reachable_live_cells",
                      true);
}

void RequireDiagnostic(idx::IndexBulkPublishRecoveryState state,
                       std::string_view diagnostic_code,
                       bool old_root_expected) {
  const auto result = idx::RecoverSortedBulkRootPublish(state);
  if (result.ok() || result.diagnostic.diagnostic_code != diagnostic_code) {
    std::cerr << "expected=" << diagnostic_code
              << " observed=" << result.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(result.fail_closed, "refusal did not fail closed");
  Require(!result.half_root_exposed, "refusal exposed a half-root");
  Require(!result.root_publish_authorized,
          "refusal authorized root publish");
  Require(!result.transaction_finality_authority,
          "refusal claimed transaction finality authority");
  Require(!result.runtime_route_capability,
          "refusal claimed runtime route capability");
  Require(!result.benchmark_clean, "refusal claimed benchmark-clean status");
  if (old_root_expected) {
    Require(result.active_root == idx::IndexBulkPublishActiveRoot::old_root,
            "refusal did not retain old root");
    Require(HasEvidence(result.evidence,
                        "old_or_new_validated_root_only",
                        "true"),
            "refusal old/new root evidence missing");
  }
  Require(HasEvidence(result.evidence, "half_root_exposed", "false"),
          "refusal half-root evidence missing");
  RequireNoDocRuntimeEvidence(result.evidence);
}

void FailClosedProofMatrix() {
  const auto fixture = MakeFixture();
  auto state = BaseState(fixture);

  state.old_metapage_present = false;
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-OLD-METAPAGE-MISSING",
      false);

  state = BaseState(fixture);
  state.old_tree_image.root_page_number += 1000;
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-OLD-TREE-IMAGE-INVALID",
      false);

  state = BaseState(fixture);
  state.candidate_generation->validated_tree = false;
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-TREE-INVALID",
      true);

  state = BaseState(fixture);
  state.candidate_generation->page_count += 1;
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-METAPAGE-PAGE-COUNT-MISMATCH",
      true);

  state = BaseState(fixture);
  state.candidate_generation->live_entry_count += 1;
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-REPORT-MISMATCH",
      true);

  state = BaseState(fixture);
  state.old_metapage.root_generation =
      fixture.candidate.candidate_generation;
  RequireDiagnostic(state,
                    "SB-INDEX-BULK-PUBLISH-RECOVERY-STALE-GENERATION",
                    true);

  state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_during_root_publish;
  state.durable_metapage_image = std::vector<platform::byte>{'b', 'a', 'd'};
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-DURABLE-METAPAGE-INVALID",
      true);

  state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_during_root_publish;
  auto wrong_metapage = fixture.publish.published_metapage;
  wrong_metapage.root_page_number += 44;
  const auto wrong_image = idx::BuildIndexMetapageControl(wrong_metapage);
  Require(wrong_image.ok(), "wrong metapage fixture failed to serialize");
  state.durable_metapage_image = wrong_image.serialized;
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-METAPAGE-TREE-MISMATCH",
      true);

  state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_after_root_publish;
  state.publish_fence_token.clear();
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-PUBLISH-EVIDENCE-MISSING",
      true);

  state = BaseState(fixture);
  state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_after_root_publish;
  state.durable_metapage_image.reset();
  RequireDiagnostic(
      state,
      "SB-INDEX-BULK-PUBLISH-RECOVERY-DURABLE-METAPAGE-MISSING",
      true);
}

}  // namespace

int main() {
  CrashBeforePublishKeepsOldRootAndDiscardsOrphan();
  CrashBeforePublishRepairsOrphanGeneration();
  CrashDuringPublishWithoutDurableMetapageKeepsOldRoot();
  CrashDuringPublishWithDurableMetapageOpensNewRoot();
  CrashAfterPublishOpensValidatedNewRoot();
  CrashAfterPublishRepairsPublishedOrphanGeneration();
  FailClosedProofMatrix();
  std::cout << "index_bulk_publish_recovery_gate=passed\n";
  return 0;
}
