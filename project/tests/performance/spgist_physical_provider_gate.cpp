// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "spgist_physical_provider.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "spgist_physical_provider_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

void RequireNoRuntimeLeak(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    Require(item.find("docs" "/execution-plans") == std::string::npos &&
                item.find("public_release_evidence") == std::string::npos &&
                item.find("docs/reference") == std::string::npos &&
                item.find("IRC-") == std::string::npos &&
                item.find("IRC_") == std::string::npos &&
                item.find("execution_plan") == std::string::npos,
            "runtime evidence leaked planning/spec/reference artifact");
  }
}

std::string UuidWithSuffix(std::string prefix, std::uint64_t suffix) {
  std::ostringstream out;
  out << prefix << std::setw(12) << std::setfill('0') << suffix;
  return out.str();
}

idx::TextInvertedRowLocator Locator(std::uint64_t row) {
  idx::TextInvertedRowLocator locator;
  locator.row_ordinal = row;
  locator.row_uuid = UuidWithSuffix("81818181-8181-7181-8181-", row);
  locator.version_uuid = UuidWithSuffix("91919191-9191-7191-8191-", row);
  return locator;
}

idx::SpatialRTreeMbr Mbr(double min_x,
                         double min_y,
                         double max_x,
                         double max_y,
                         std::uint32_t srid = 4326) {
  idx::SpatialRTreeMbr mbr;
  mbr.dimensions = 2;
  mbr.srid = srid;
  mbr.min = {min_x, min_y};
  mbr.max = {max_x, max_y};
  return mbr;
}

idx::SpGistExactRecheckProof Proof() {
  idx::SpGistExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_geometry_available = true;
  proof.exact_predicate_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "spgist_exact_source_mga_security_recheck_contract";
  return proof;
}

idx::SpatialRTreeDescriptor SpatialDescriptor() {
  idx::SpatialRTreeDescriptor descriptor;
  descriptor.dimensions = 2;
  descriptor.descriptor_epoch = 61;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.supports_point = true;
  descriptor.supports_mbr = true;
  return descriptor;
}

idx::SpatialRTreeSridResource SridResource() {
  idx::SpatialRTreeSridResource resource;
  resource.resource_uuid = "a1a1a1a1-a1a1-71a1-81a1-a1a1a1a1a1a1";
  resource.srid = 4326;
  resource.resource_epoch = 67;
  resource.coordinate_order = "xy";
  resource.deterministic = true;
  resource.safe = true;
  resource.cache_present = true;
  return resource;
}

idx::SpGistOpclassRuntime Opclass() {
  return idx::MakeSpatialQuadMbrSpGistOpclass(71, 73, 4326);
}

std::vector<idx::SpGistSourceRow> BaseRows() {
  return {{Locator(10), Mbr(0.0, 0.0, 1.0, 1.0), "src10"},
          {Locator(20), Mbr(2.0, 2.0, 4.0, 4.0), "src20"},
          {Locator(30), Mbr(5.0, 5.0, 6.0, 6.0), "src30"},
          {Locator(40), Mbr(-1.0, -1.0, 0.5, 0.5), "src40"},
          {Locator(50), Mbr(3.0, 0.0, 5.0, 1.0), "src50"},
          {Locator(60), Mbr(8.0, 8.0, 9.0, 9.0), "src60"},
          {Locator(70), Mbr(-6.0, 7.0, -5.0, 8.0), "src70"},
          {Locator(80), Mbr(7.0, -6.0, 8.0, -5.0), "src80"}};
}

idx::SpGistBuildRequest BuildRequest() {
  idx::SpGistBuildRequest request;
  request.relation_uuid = "b1b1b1b1-b1b1-71b1-81b1-b1b1b1b1b1b1";
  request.index_uuid = "b2b2b2b2-b2b2-72b2-82b2-b2b2b2b2b2b2";
  request.provider_uuid = "b3b3b3b3-b3b3-73b3-83b3-b3b3b3b3b3b3";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.spatial_descriptor = SpatialDescriptor();
  request.srid_resource = SridResource();
  request.opclass = Opclass();
  request.recheck_proof = Proof();
  request.leaf_capacity = 2;
  request.max_depth = 12;
  request.rows = BaseRows();
  return request;
}

std::vector<std::uint64_t> Rows(
    const std::vector<idx::SpGistCandidate>& candidates) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<platform::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "could not write persistence fixture");
}

std::vector<platform::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "could not read persistence fixture");
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

idx::SpGistQueryResult Query(const idx::SpGistPhysicalProvider& provider,
                             idx::SpGistPredicateStrategy strategy,
                             idx::SpatialRTreeMbr mbr) {
  idx::SpGistQueryRequest request;
  request.provider = provider;
  request.opclass = Opclass();
  request.strategy = strategy;
  request.query_mbr = std::move(mbr);
  request.recheck_proof = Proof();
  return idx::QuerySpGistPhysicalProvider(request);
}

void VerifyBuildQuerySerializeAndReopen() {
  const auto built = idx::BuildSpGistPhysicalProvider(BuildRequest());
  Require(built.ok() &&
              built.used_bulk_spatial_prefix_build &&
              built.all_methods_exercised,
          "SP-GiST build did not exercise all opclass methods");
  Require(built.provider.physical_inner_tuple_layout_present &&
              built.provider.physical_leaf_tuple_layout_present &&
              built.provider.partitioned_search_tree_present &&
              built.provider.choose_present &&
              built.provider.inner_consistent_present &&
              built.provider.leaf_consistent_present &&
              built.provider.compress_present &&
              built.provider.decompress_present &&
              built.provider.bulk_spatial_prefix_build_present,
          "SP-GiST physical surfaces missing");
  Require(built.provider.tree_height > 1 &&
              built.provider.split_partition_count > 0 &&
              built.provider.choose_call_count > 0 &&
              built.provider.inner_consistent_call_count > 0 &&
              built.provider.leaf_consistent_call_count > 0,
          "SP-GiST partitioned tree did not split or use opclass callbacks");
  Require(HasEvidence(built.provider.evidence,
                      idx::kSpGistPhysicalProviderSearchKey),
          "neutral SP-GiST runtime evidence missing");
  RequireNoRuntimeLeak(built.provider.evidence);

  const auto point = Query(built.provider,
                           idx::SpGistPredicateStrategy::point,
                           Mbr(0.25, 0.25, 0.25, 0.25));
  Require(point.ok() &&
              point.partitioned_search_tree_used &&
              point.inner_consistent_used &&
              point.leaf_consistent_used &&
              Rows(point.candidates) == std::vector<std::uint64_t>({10, 40}),
          "SP-GiST point query changed");
  const auto intersects = Query(built.provider,
                                idx::SpGistPredicateStrategy::intersects,
                                Mbr(3.5, 0.5, 5.5, 5.5));
  Require(intersects.ok() &&
              Rows(intersects.candidates) ==
                  std::vector<std::uint64_t>({20, 30, 50}),
          "SP-GiST intersects query changed");
  const auto contains = Query(built.provider,
                              idx::SpGistPredicateStrategy::contains,
                              Mbr(2.5, 2.5, 3.0, 3.0));
  Require(contains.ok() &&
              Rows(contains.candidates) == std::vector<std::uint64_t>({20}),
          "SP-GiST contains query changed");
  const auto within = Query(built.provider,
                            idx::SpGistPredicateStrategy::within,
                            Mbr(-2.0, -2.0, 2.0, 2.0));
  Require(within.ok() &&
              Rows(within.candidates) == std::vector<std::uint64_t>({10, 40}),
          "SP-GiST within query changed");
  for (const auto& result : {point, intersects, contains, within}) {
    Require(result.candidate_rows_only &&
                !result.final_rows_authorized &&
                result.nodes_visited > 0 &&
                result.leaf_tuples_examined > 0,
            "SP-GiST query omitted candidate-only traversal evidence");
    for (const auto& candidate : result.candidates) {
      Require(candidate.from_spgist_leaf_tuple &&
                  candidate.opclass_leaf_consistent &&
                  candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted &&
                  !candidate.source_recheck_evidence_ref.empty(),
              "SP-GiST candidate leaked authority or omitted recheck proof");
    }
    RequireNoRuntimeLeak(result.evidence);
  }

  idx::SpGistQueryRequest nearest;
  nearest.provider = built.provider;
  nearest.opclass = Opclass();
  nearest.strategy = idx::SpGistPredicateStrategy::nearest;
  nearest.query_point = {4.9, 0.9};
  nearest.top_k = 2;
  nearest.recheck_proof = Proof();
  const auto nearest_result = idx::QuerySpGistPhysicalProvider(nearest);
  Require(nearest_result.ok() &&
              nearest_result.priority_queue_used &&
              Rows(nearest_result.candidates) ==
                  std::vector<std::uint64_t>({50, 20}),
          "SP-GiST nearest query changed");

  const auto serialized =
      idx::SerializeSpGistPhysicalProvider(built.provider);
  Require(serialized.ok(), "SP-GiST serialization failed");
  const auto serialized_again =
      idx::SerializeSpGistPhysicalProvider(built.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "SP-GiST serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      ("scratchbird_spgist_physical_provider_gate_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".sbspg");
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "persisted SP-GiST bytes changed");

  idx::SpGistOpenRequest open;
  open.bytes = serialized.bytes;
  open.opclass = Opclass();
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = built.provider.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = built.provider.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = built.provider.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = built.provider.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = built.provider.provider_generation;
  open.expected_opclass_epoch_present = true;
  open.expected_opclass_epoch = built.provider.opclass.opclass_epoch;
  open.expected_resource_epoch_present = true;
  open.expected_resource_epoch = built.provider.opclass.resource_epoch;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch =
      built.provider.spatial_descriptor.descriptor_epoch;
  open.expected_srid_resource_epoch_present = true;
  open.expected_srid_resource_epoch =
      built.provider.srid_resource.resource_epoch;
  open.expected_srid_present = true;
  open.expected_srid = built.provider.srid_resource.srid;
  open.recheck_proof = Proof();
  const auto opened = idx::OpenSpGistPhysicalProvider(open);
  Require(opened.ok(), "clean SP-GiST reopen failed");
  Require(idx::SerializeSpGistPhysicalProvider(opened.provider).bytes ==
              serialized.bytes,
          "SP-GiST reopen serialization equivalence failed");

  auto tampered = built.provider;
  bool tampered_leaf = false;
  for (auto& node : tampered.nodes) {
    if (node.leaf && !node.leaf_tuples.empty()) {
      node.leaf_tuples.front().key = Mbr(100.0, 100.0, 101.0, 101.0);
      tampered_leaf = true;
      break;
    }
  }
  Require(tampered_leaf, "SP-GiST tamper fixture did not find a leaf tuple");
  Require(!idx::SerializeSpGistPhysicalProvider(tampered).ok(),
          "SP-GiST serialized a leaf tuple outside its node prefix");
}

void VerifyBoundaryCrossingRowsRemainReachable() {
  auto request = BuildRequest();
  request.rows.push_back(
      {Locator(100), Mbr(1.0, -0.5, 2.0, 0.5), "src100"});
  const auto built = idx::BuildSpGistPhysicalProvider(request);
  Require(built.ok(), "SP-GiST boundary-crossing build failed");

  const auto left_edge = Query(built.provider,
                               idx::SpGistPredicateStrategy::intersects,
                               Mbr(1.05, -0.25, 1.10, 0.25));
  Require(left_edge.ok() &&
              Rows(left_edge.candidates) == std::vector<std::uint64_t>({100}),
          "SP-GiST boundary-crossing row was missed on the left edge");

  const auto right_edge = Query(built.provider,
                                idx::SpGistPredicateStrategy::intersects,
                                Mbr(1.90, -0.25, 1.95, 0.25));
  Require(right_edge.ok() &&
              Rows(right_edge.candidates) == std::vector<std::uint64_t>({100}),
          "SP-GiST boundary-crossing row was missed on the right edge");

  for (const auto& result : {left_edge, right_edge}) {
    Require(result.candidate_rows_only &&
                !result.final_rows_authorized &&
                result.leaf_tuples_examined > 0,
            "SP-GiST boundary-crossing query leaked authority");
    for (const auto& candidate : result.candidates) {
      Require(candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted,
              "SP-GiST boundary-crossing candidate omitted recheck proof");
    }
  }
}

void VerifyMutationsAndFailClosed() {
  const auto built = idx::BuildSpGistPhysicalProvider(BuildRequest());
  Require(built.ok(), "SP-GiST mutation baseline build failed");

  idx::SpGistMutation insert;
  insert.kind = idx::SpGistMutationKind::insert_row;
  insert.expected_provider_generation_present = true;
  insert.expected_provider_generation = built.provider.provider_generation;
  insert.expected_opclass_epoch_present = true;
  insert.expected_opclass_epoch = built.provider.opclass.opclass_epoch;
  insert.expected_resource_epoch_present = true;
  insert.expected_resource_epoch = built.provider.opclass.resource_epoch;
  insert.expected_descriptor_epoch_present = true;
  insert.expected_descriptor_epoch =
      built.provider.spatial_descriptor.descriptor_epoch;
  insert.expected_srid_resource_epoch_present = true;
  insert.expected_srid_resource_epoch =
      built.provider.srid_resource.resource_epoch;
  insert.after_row_present = true;
  insert.after_row = {Locator(90), Mbr(3.8, 0.2, 4.2, 0.8), "src90"};
  insert.recheck_proof = Proof();
  const auto inserted =
      idx::ApplySpGistPhysicalMutation(built.provider, Opclass(), insert);
  Require(inserted.ok() &&
              inserted.provider.provider_generation ==
                  built.provider.provider_generation + 1,
          "SP-GiST insert mutation failed");
  const auto after_insert = Query(inserted.provider,
                                  idx::SpGistPredicateStrategy::intersects,
                                  Mbr(3.5, 0.5, 5.5, 5.5));
  Require(after_insert.ok() &&
              Rows(after_insert.candidates) ==
                  std::vector<std::uint64_t>({20, 30, 50, 90}),
          "SP-GiST insert did not update exact locator candidates");

  idx::SpGistMutation update;
  update.kind = idx::SpGistMutationKind::update_row;
  update.expected_provider_generation_present = true;
  update.expected_provider_generation = inserted.provider.provider_generation;
  update.before_row_present = true;
  update.before_row = {Locator(20), Mbr(2.0, 2.0, 4.0, 4.0), "src20"};
  update.after_row_present = true;
  update.after_row = {Locator(21), Mbr(-4.0, -4.0, -3.0, -3.0), "src21"};
  update.recheck_proof = Proof();
  const auto updated =
      idx::ApplySpGistPhysicalMutation(inserted.provider, Opclass(), update);
  Require(updated.ok() && updated.tombstone_written,
          "SP-GiST update mutation failed");
  const auto old_window = Query(updated.provider,
                                idx::SpGistPredicateStrategy::intersects,
                                Mbr(3.5, 0.5, 5.5, 5.5));
  Require(old_window.ok() &&
              Rows(old_window.candidates) ==
                  std::vector<std::uint64_t>({30, 50, 90}),
          "SP-GiST update left stale row locator candidate");
  const auto new_window = Query(updated.provider,
                                idx::SpGistPredicateStrategy::intersects,
                                Mbr(-4.5, -4.5, -2.5, -2.5));
  Require(new_window.ok() &&
              Rows(new_window.candidates) == std::vector<std::uint64_t>({21}),
          "SP-GiST update did not publish replacement locator");

  idx::SpGistMutation erase;
  erase.kind = idx::SpGistMutationKind::delete_row;
  erase.expected_provider_generation_present = true;
  erase.expected_provider_generation = updated.provider.provider_generation;
  erase.before_row_present = true;
  erase.before_row = {Locator(50), Mbr(3.0, 0.0, 5.0, 1.0), "src50"};
  erase.recheck_proof = Proof();
  const auto erased =
      idx::ApplySpGistPhysicalMutation(updated.provider, Opclass(), erase);
  Require(erased.ok() && erased.tombstone_written,
          "SP-GiST delete mutation failed");
  const auto after_delete = Query(erased.provider,
                                  idx::SpGistPredicateStrategy::intersects,
                                  Mbr(3.5, 0.5, 5.5, 5.5));
  Require(after_delete.ok() &&
              Rows(after_delete.candidates) ==
                  std::vector<std::uint64_t>({30, 90}),
          "SP-GiST delete left stale locator candidate");

  auto missing_method = Opclass();
  missing_method.methods.leaf_consistent = {};
  auto bad_request = BuildRequest();
  bad_request.opclass = missing_method;
  Require(!idx::BuildSpGistPhysicalProvider(bad_request).ok(),
          "SP-GiST admitted opclass with missing leaf-consistent");

  auto nondeterministic = BuildRequest();
  nondeterministic.opclass.descriptor.deterministic = false;
  Require(!idx::BuildSpGistPhysicalProvider(nondeterministic).ok(),
          "SP-GiST admitted nondeterministic opclass");

  auto wrong_srid = BuildRequest();
  wrong_srid.rows.front().key.srid = 3857;
  Require(!idx::BuildSpGistPhysicalProvider(wrong_srid).ok(),
          "SP-GiST admitted incompatible SRID");

  auto authority = BuildRequest();
  authority.recheck_proof.provider_finality_authority_claimed = true;
  Require(!idx::BuildSpGistPhysicalProvider(authority).ok(),
          "SP-GiST admitted provider authority claim");

  auto fallback_query = Query(built.provider,
                              idx::SpGistPredicateStrategy::intersects,
                              Mbr(0.0, 0.0, 1.0, 1.0));
  Require(fallback_query.ok(), "SP-GiST fallback baseline query failed");
  idx::SpGistQueryRequest refused;
  refused.provider = built.provider;
  refused.opclass = Opclass();
  refused.strategy = idx::SpGistPredicateStrategy::intersects;
  refused.query_mbr = Mbr(0.0, 0.0, 1.0, 1.0);
  refused.recheck_proof = Proof();
  refused.contract_only_fallback = true;
  Require(!idx::QuerySpGistPhysicalProvider(refused).ok(),
          "SP-GiST admitted contract-only fallback mode");
  refused.contract_only_fallback = false;
  refused.provider_only_fallback = true;
  Require(!idx::QuerySpGistPhysicalProvider(refused).ok(),
          "SP-GiST admitted provider-only fallback mode");
  refused.provider_only_fallback = false;
  refused.opclass_epoch_current = false;
  Require(!idx::QuerySpGistPhysicalProvider(refused).ok(),
          "SP-GiST admitted stale opclass epoch");

  const auto serialized =
      idx::SerializeSpGistPhysicalProvider(built.provider);
  Require(serialized.ok(), "SP-GiST fail-closed serialization failed");
  auto corrupt = serialized.bytes;
  corrupt.back() ^= static_cast<platform::byte>(0x1);
  idx::SpGistOpenRequest open;
  open.bytes = corrupt;
  open.opclass = Opclass();
  open.recheck_proof = Proof();
  Require(idx::OpenSpGistPhysicalProvider(open).open_class ==
              idx::SpGistOpenClass::bad_checksum,
          "SP-GiST corrupt artifact did not fail closed with checksum");

  open.bytes = serialized.bytes;
  open.expected_opclass_epoch_present = true;
  open.expected_opclass_epoch = built.provider.opclass.opclass_epoch + 1;
  Require(idx::OpenSpGistPhysicalProvider(open).open_class ==
              idx::SpGistOpenClass::stale_opclass_epoch,
          "SP-GiST stale opclass epoch was not refused on open");
}

}  // namespace

int main() {
  VerifyBuildQuerySerializeAndReopen();
  VerifyBoundaryCrossingRowsRemainReachable();
  VerifyMutationsAndFailClosed();
  std::cout << "spgist_physical_provider_gate=passed\n";
  return 0;
}
