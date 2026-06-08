// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_root_generation_publish.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_root_generation_publish_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind,
                                                        1810001000000ull + salt);
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

idx::IndexMetapageControl CurrentMetapage(platform::TypedUuid index_uuid) {
  idx::IndexMetapageControl control;
  control.index_uuid = index_uuid;
  control.family = idx::IndexFamily::btree;
  control.root_page_number = 700;
  control.resource_epoch = 10;
  control.mutation_epoch = 12;
  control.root_generation = 0;
  control.page_count = 3;
  control.tuple_count_estimate = 17;
  control.semantic_profile_id = "index_root_generation_publish_gate";
  return control;
}

idx::SortedBulkIndexCandidateRootGeneration Candidate() {
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = GeneratedUuid(platform::UuidKind::object, 10);
  request.metadata.table_uuid = GeneratedUuid(platform::UuidKind::object, 11);
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = "index_root_generation_publish_gate";
  request.metadata.physical_page_size = 1024;
  request.metadata.leaf_entry_capacity = 2;
  request.metadata.internal_entry_capacity = 2;
  request.rows = {Row('d', '1', 101),
                  Row('a', '1', 102),
                  Row('b', '1', 103),
                  Row('c', '1', 104),
                  Row('e', '1', 105)};
  const auto built = idx::BuildSortedExactBulkIndex(request);
  Require(built.ok(), "sorted bulk candidate build refused");
  Require(built.candidate_root_generation.created,
          "candidate root generation missing");
  Require(built.candidate_root_generation.validated_tree,
          "candidate tree validation flag missing");
  return built.candidate_root_generation;
}

idx::IndexRootGenerationPublishRequest PublishRequest() {
  idx::IndexRootGenerationPublishRequest request;
  request.candidate = Candidate();
  request.current_metapage = CurrentMetapage(request.candidate.tree.index_uuid);
  request.candidate_tree_validation_proof = true;
  request.durable_metadata_write_evidence = true;
  request.mga_finality_authority_evidence = true;
  request.durable_metadata_evidence_token =
      "durable_metapage_write_fsync_evidence";
  request.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  request.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  return request;
}

bool HasEvidence(
    const std::vector<idx::IndexRootGenerationPublishEvidence>& evidence,
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
    const std::vector<idx::IndexRootGenerationPublishEvidence>& evidence) {
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

void RequireDiagnostic(
    const idx::IndexRootGenerationPublishRequest& request,
    std::string_view diagnostic_code) {
  const auto result = idx::PublishIndexRootGeneration(request);
  if (result.ok() || result.diagnostic.diagnostic_code != diagnostic_code) {
    std::cerr << "expected=" << diagnostic_code
              << " observed=" << result.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(!result.root_publish_authorized,
          "refusal authorized root publish");
  Require(HasEvidence(result.evidence, "root_publish_authorized", "false"),
          "refusal root publish evidence missing");
}

void SuccessfulPublishIsGenerationMetadataAndMgaBound() {
  const auto request = PublishRequest();
  const auto result = idx::PublishIndexRootGeneration(request);
  Require(result.ok(), "valid root generation publish refused");
  Require(result.published, "publish flag missing");
  Require(result.root_publish_authorized,
          "root publish authorization missing after proofs");
  Require(!result.physical_append_authorized,
          "publish result incorrectly authorized physical append");
  Require(!result.transaction_finality_authority,
          "publish result claimed transaction finality authority");
  Require(!result.recovery_authority,
          "publish result claimed recovery authority");
  Require(!result.runtime_route_capability,
          "publish result claimed runtime route capability");
  Require(result.old_root_metadata_preserved,
          "old root metadata was not preserved");
  Require(result.rollback_safe_metadata_contract,
          "rollback metadata contract proof missing");
  Require(result.reopen_safe_metadata_contract,
          "reopen metadata contract proof missing");
  Require(result.old_metapage.root_page_number ==
              request.current_metapage.root_page_number,
          "old root metadata drifted");
  Require(result.published_metapage.root_page_number ==
              request.candidate.root_page_number,
          "published root page mismatch");
  Require(result.published_metapage.root_generation ==
              request.candidate.candidate_generation,
          "published root generation mismatch");
  Require(result.published_metapage.root_generation >
              request.current_metapage.root_generation,
          "published generation did not advance");
  Require(result.published_metapage.page_count ==
              request.candidate.page_count,
          "published page count mismatch");
  Require(result.published_metapage.tuple_count_estimate ==
              request.candidate.live_entry_count,
          "published tuple estimate mismatch");
  Require(result.reopened_metapage.root_page_number ==
              result.published_metapage.root_page_number,
          "reopened metapage root mismatch");
  Require(result.reopened_metapage.root_generation ==
              result.published_metapage.root_generation,
          "reopened metapage generation mismatch");
  Require(result.published_tree_image.root_page_number ==
              request.candidate.root_page_number,
          "published tree image root mismatch");
  Require(HasEvidence(result.evidence, "root_publish_authorized", "true"),
          "authorized publish evidence missing");
  Require(HasEvidence(result.evidence, "physical_append_authorized", "false"),
          "physical append non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "transaction_finality_authority",
                      "false"),
          "transaction finality non-authority evidence missing");
  Require(HasEvidence(result.evidence, "recovery_authority", "false"),
          "recovery non-authority evidence missing");
  Require(HasEvidence(result.evidence, "runtime_route_capability", "false"),
          "runtime route non-capability evidence missing");
  Require(HasEvidence(result.evidence,
                      "irc_042_crash_repair_classification",
                      "pending"),
          "IRC-042 pending evidence missing");
  RequireNoDocRuntimeEvidence(result.evidence);
}

void FailClosedProofMatrix() {
  auto request = PublishRequest();
  request.candidate.created = false;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-CANDIDATE-GENERATION-MISSING");

  request = PublishRequest();
  request.candidate_tree_validation_proof = false;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-CANDIDATE-TREE-UNVALIDATED");

  request = PublishRequest();
  request.candidate.physical_leaf_pack = false;
  RequireDiagnostic(
      request,
      "SB-INDEX-ROOT-PUBLISH-CANDIDATE-PHYSICAL-PROOF-MISSING");

  request = PublishRequest();
  request.candidate.candidate_root_page_present = false;
  RequireDiagnostic(
      request,
      "SB-INDEX-ROOT-PUBLISH-CANDIDATE-PHYSICAL-PROOF-MISSING");

  request = PublishRequest();
  request.candidate.root_page_number += 1;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-ROOT-PAGE-MISMATCH");

  request = PublishRequest();
  request.candidate.tree.root_page_number = 9999;
  request.candidate.root_page_number = 9999;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-CANDIDATE-ROOT-MISSING");

  request = PublishRequest();
  request.current_metapage.root_generation =
      request.candidate.candidate_generation;
  RequireDiagnostic(request, "SB-INDEX-ROOT-PUBLISH-STALE-GENERATION");

  request = PublishRequest();
  request.candidate.page_count += 1;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-CANDIDATE-REPORT-MISMATCH");

  request = PublishRequest();
  request.candidate.report.page_count += 1;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-CANDIDATE-REPORT-MISMATCH");

  request = PublishRequest();
  request.candidate.live_entry_count += 1;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-CANDIDATE-REPORT-MISMATCH");

  request = PublishRequest();
  request.durable_metadata_write_evidence = false;
  RequireDiagnostic(
      request,
      "SB-INDEX-ROOT-PUBLISH-DURABLE-METADATA-EVIDENCE-MISSING");

  request = PublishRequest();
  request.publish_fence_token.clear();
  RequireDiagnostic(request, "SB-INDEX-ROOT-PUBLISH-FENCE-MISSING");

  request = PublishRequest();
  request.mga_finality_authority_evidence = false;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-MGA-AUTHORITY-PROOF-MISSING");

  request = PublishRequest();
  request.durable_metapage_image = {'b', 'a', 'd'};
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-METAPAGE-REOPEN-INVALID");

  request = PublishRequest();
  auto wrong_metapage = request.current_metapage;
  wrong_metapage.root_page_number = request.candidate.root_page_number + 55;
  wrong_metapage.root_generation = request.candidate.candidate_generation;
  wrong_metapage.page_count = request.candidate.page_count;
  wrong_metapage.tuple_count_estimate = request.candidate.live_entry_count;
  wrong_metapage.resource_epoch = request.current_metapage.resource_epoch + 1;
  wrong_metapage.mutation_epoch = request.current_metapage.mutation_epoch + 1;
  const auto wrong_image = idx::BuildIndexMetapageControl(wrong_metapage);
  Require(wrong_image.ok(), "wrong metapage fixture failed to serialize");
  request.durable_metapage_image = wrong_image.serialized;
  RequireDiagnostic(request,
                    "SB-INDEX-ROOT-PUBLISH-METAPAGE-REOPEN-MISMATCH");
}

void MetapageRootGenerationRoundTrips() {
  const auto request = PublishRequest();
  auto control = request.current_metapage;
  control.root_generation = 42;
  const auto built = idx::BuildIndexMetapageControl(control);
  Require(built.ok(), "metapage with root generation refused");
  const auto parsed = idx::ParseIndexMetapageControl(built.serialized);
  Require(parsed.ok(), "metapage with root generation failed parse");
  Require(parsed.control.root_generation == 42,
          "metapage root generation did not round trip");
}

}  // namespace

int main() {
  MetapageRootGenerationRoundTrips();
  SuccessfulPublishIsGenerationMetadataAndMgaBound();
  FailClosedProofMatrix();
  std::cout << "index_root_generation_publish_gate=passed\n";
  return 0;
}
