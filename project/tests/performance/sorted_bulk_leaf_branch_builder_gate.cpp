// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sorted_bulk_index_build.hpp"
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
  std::cerr << "sorted_bulk_leaf_branch_builder_gate: " << message << '\n';
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
                                                        1810000000000ull + salt);
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
                                 platform::u64 salt,
                                 std::string payload = "payload") {
  idx::SortedBulkIndexRowInput row;
  row.encoded_key = Key(group, suffix);
  row.row_uuid = UuidText(platform::UuidKind::row, salt);
  row.version_uuid = UuidText(platform::UuidKind::row, salt + 1000);
  row.payload_value = std::move(payload);
  row.source_ordinal = salt;
  return row;
}

idx::SortedBulkIndexBuildRequest Request(
    std::vector<idx::SortedBulkIndexRowInput> rows) {
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = GeneratedUuid(platform::UuidKind::object, 10);
  request.metadata.table_uuid = GeneratedUuid(platform::UuidKind::object, 11);
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = "sorted_bulk_leaf_branch_builder_gate";
  request.metadata.physical_page_size = 1024;
  request.metadata.leaf_entry_capacity = 2;
  request.metadata.internal_entry_capacity = 2;
  request.rows = std::move(rows);
  return request;
}

bool HasEvidence(const std::vector<idx::SortedBulkIndexBuildEvidence>& evidence,
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
    const std::vector<idx::SortedBulkIndexBuildEvidence>& evidence) {
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

page::IndexBtreePageBody Fetch(const page::IndexBtreePhysicalTree& tree,
                               platform::u64 page_number) {
  const auto fetched = page::FetchIndexBtreePhysicalPage(tree, page_number);
  if (!fetched.ok()) {
    std::cerr << "fetch failed diagnostic="
              << fetched.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  return fetched.body;
}

page::IndexBtreeCell LastCell(const page::IndexBtreePageBody& body) {
  Require(!body.cells.empty(), "fence source page had no cells");
  return body.cells.back();
}

void ValidateFenceTree(const page::IndexBtreePhysicalTree& tree,
                       const page::IndexBtreePageBody& node,
                       platform::u64 expected_parent,
                       std::vector<page::IndexBtreePageBody>* leaves) {
  Require(node.parent_page_number == expected_parent, "parent metadata mismatch");
  if (node.tree_level == 0) {
    Require(node.page_kind == page::IndexBtreePageKind::leaf ||
                node.page_kind == page::IndexBtreePageKind::root,
            "leaf kind mismatch");
    leaves->push_back(node);
    return;
  }
  Require(node.page_kind == page::IndexBtreePageKind::internal ||
              node.page_kind == page::IndexBtreePageKind::root,
          "branch kind mismatch");
  for (const auto& fence : node.cells) {
    Require(fence.high_key, "branch cell is not a stored fence key");
    Require(fence.child_page_number != 0, "fence child page missing");
    const auto child = Fetch(tree, fence.child_page_number);
    Require(child.tree_level + 1 == node.tree_level, "child level mismatch");
    const auto child_high = LastCell(child);
    Require(fence.encoded_key == child_high.encoded_key,
            "fence key did not match child high key");
    Require(fence.row_uuid.value == child_high.row_uuid.value,
            "fence row uuid did not match child high key");
    Require(fence.version_uuid.value == child_high.version_uuid.value,
            "fence version uuid did not match child high key");
    ValidateFenceTree(tree, child, node.page_number, leaves);
  }
}

void AcceptedPhysicalLeafAndBranchBuild() {
  auto request = Request({Row('d', '1', 101),
                          Row('a', '1', 102),
                          Row('b', '1', 103),
                          Row('c', '1', 104),
                          Row('e', '1', 105),
                          Row('f', '1', 106),
                          Row('g', '1', 107),
                          Row('h', '1', 108),
                          Row('i', '1', 109)});
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(result.ok(), "accepted physical sorted bulk build was refused");
  Require(result.candidate_root_generation.created,
          "candidate root generation missing");
  Require(result.candidate_root_generation.physical_leaf_pack,
          "physical leaf pack flag missing");
  Require(result.candidate_root_generation.branch_levels_built,
          "branch levels were not built");
  Require(result.candidate_root_generation.fence_keys_stored,
          "fence keys were not stored");
  Require(!result.candidate_root_generation.root_publish_authorized,
          "candidate root generation authorized root publish");
  Require(!result.candidate_root_generation.physical_append_authorized,
          "candidate root generation authorized append");
  Require(result.candidate_root_generation.branch_level_count >= 2,
          "small-capacity build did not create multiple branch levels");
  Require(result.candidate_root_generation.report.valid,
          "candidate physical tree report was invalid");
  Require(result.candidate_root_generation.report.tuple_live_entry_estimate == 9,
          "candidate physical tree live entry count drifted");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_physical_leaf_pack",
                      "true"),
          "physical leaf pack evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_branch_levels_built",
                      "true"),
          "branch build evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_fence_keys_stored",
                      "true"),
          "fence key evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_candidate_root_generation_created",
                      "true"),
          "candidate root evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_root_publish_authorized",
                      "false"),
          "root publish non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_transaction_mga_finality_engine_owned",
                      "true"),
          "MGA finality ownership evidence missing");
  const auto root = Fetch(result.candidate_root_generation.tree,
                          result.candidate_root_generation.root_page_number);
  std::vector<page::IndexBtreePageBody> leaves;
  ValidateFenceTree(result.candidate_root_generation.tree, root, 0, &leaves);
  Require(leaves.size() == result.candidate_root_generation.leaf_page_count,
          "reachable leaf count drifted");
  RequireNoDocRuntimeEvidence(result.evidence);
}

void CoveringPayloadLayoutConsumedButNotRouted() {
  auto request = Request({Row('a', '1', 201, "covering-payload-a"),
                          Row('b', '1', 202, "covering-payload-b"),
                          Row('c', '1', 203, "covering-payload-c")});
  request.metadata.family = idx::IndexFamily::covering;
  request.metadata.family_name = "covering";
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(result.ok(), "covering physical sorted bulk build was refused");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_covering_payload_layout_consumed",
                      "true"),
          "covering payload layout evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_runtime_route_capability",
                      "true"),
          "covering build did not advertise completed runtime route capability");
}

void UniqueProofStillRequiredForUniqueFamily() {
  auto request = Request({Row('a', '1', 301), Row('a', '1', 302)});
  request.metadata.family = idx::IndexFamily::unique_btree;
  request.metadata.family_name = "unique_btree";
  request.metadata.unique = false;
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(!result.ok(), "unique_btree duplicate build skipped unique proof");
  Require(result.uniqueness_refused, "unique_btree duplicate was not refused");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNIQUE-DUPLICATE-BATCH",
          "unique_btree duplicate diagnostic drifted");
}

void FailClosedInvalidInputs() {
  auto unsafe = Request({Row('a', '1', 401)});
  unsafe.rows[0].encoded_key = "SBK1legacy";
  auto result = idx::BuildSortedExactBulkIndex(unsafe);
  Require(!result.ok(), "unsafe legacy key was accepted");
  Require(result.unsafe_key_refused, "unsafe key refusal flag missing");

  auto invalid_uuid = Request({Row('a', '1', 402)});
  invalid_uuid.rows[0].row_uuid = "not-a-uuid";
  result = idx::BuildSortedExactBulkIndex(invalid_uuid);
  Require(!result.ok(), "invalid row uuid was accepted");
  Require(result.invalid_descriptor_refused,
          "invalid descriptor refusal flag missing");

  auto invalid_order = Request({Row('b', '1', 403), Row('a', '1', 404)});
  invalid_order.metadata.input_presorted = true;
  invalid_order.metadata.order_proof_valid = true;
  result = idx::BuildSortedExactBulkIndex(invalid_order);
  Require(!result.ok(), "invalid order proof was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-ORDER-PROOF-INVALID",
          "invalid order proof diagnostic drifted");

  auto overflow = Request({Row('a', '1', 405)});
  overflow.metadata.physical_page_size = 170;
  overflow.rows[0].encoded_key = std::string("SBKO") + std::string(128, 'x');
  result = idx::BuildSortedExactBulkIndex(overflow);
  Require(!result.ok(), "oversized physical leaf cell was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-PHYSICAL-VALIDATION-REFUSED",
          "overflow diagnostic drifted");

  auto unsupported = Request({Row('a', '1', 406)});
  unsupported.metadata.family = idx::IndexFamily::hash;
  result = idx::BuildSortedExactBulkIndex(unsupported);
  Require(!result.ok(), "unsupported sorted family was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-FAMILY-REFUSED",
          "unsupported family diagnostic drifted");

  auto covering_missing = Request({Row('a', '1', 407, "")});
  covering_missing.metadata.family = idx::IndexFamily::covering;
  result = idx::BuildSortedExactBulkIndex(covering_missing);
  Require(!result.ok(), "covering row without physical payload was accepted");
  Require(result.invalid_descriptor_refused,
          "covering missing payload refusal flag missing");
}

}  // namespace

int main() {
  AcceptedPhysicalLeafAndBranchBuild();
  CoveringPayloadLayoutConsumedButNotRouted();
  UniqueProofStillRequiredForUniqueFamily();
  FailClosedInvalidInputs();
  std::cout << "sorted_bulk_leaf_branch_builder_gate=passed\n";
  return 0;
}
