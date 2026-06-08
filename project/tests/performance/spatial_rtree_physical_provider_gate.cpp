// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "spatial_rtree_physical_provider.hpp"

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
  std::cerr << "spatial_rtree_physical_provider_gate: " << message << '\n';
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
  locator.row_uuid = UuidWithSuffix("19191919-1919-7919-8919-", row);
  locator.version_uuid = UuidWithSuffix("29292929-2929-7929-8929-", row);
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

idx::SpatialRTreeRecheckProof Proof() {
  idx::SpatialRTreeRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_geometry_available = true;
  proof.exact_predicate_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "spatial_exact_source_mga_security_recheck_contract";
  return proof;
}

idx::SpatialRTreeDescriptor Descriptor() {
  idx::SpatialRTreeDescriptor descriptor;
  descriptor.dimensions = 2;
  descriptor.descriptor_epoch = 31;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.supports_point = true;
  descriptor.supports_mbr = true;
  return descriptor;
}

idx::SpatialRTreeSridResource SridResource() {
  idx::SpatialRTreeSridResource resource;
  resource.resource_uuid = "39393939-3939-7939-8939-393939393939";
  resource.srid = 4326;
  resource.resource_epoch = 37;
  resource.coordinate_order = "xy";
  resource.deterministic = true;
  resource.safe = true;
  resource.cache_present = true;
  return resource;
}

std::vector<idx::SpatialRTreeSourceRow> BaseRows() {
  return {{Locator(10), Mbr(0.0, 0.0, 1.0, 1.0), "src10"},
          {Locator(20), Mbr(2.0, 2.0, 4.0, 4.0), "src20"},
          {Locator(30), Mbr(5.0, 5.0, 6.0, 6.0), "src30"},
          {Locator(40), Mbr(-1.0, -1.0, 0.5, 0.5), "src40"},
          {Locator(50), Mbr(3.0, 0.0, 5.0, 1.0), "src50"},
          {Locator(60), Mbr(8.0, 8.0, 9.0, 9.0), "src60"}};
}

idx::SpatialRTreeBuildRequest BuildRequest(
    idx::SpatialRTreeBuildMode mode =
        idx::SpatialRTreeBuildMode::incremental_insert) {
  idx::SpatialRTreeBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = Descriptor();
  request.srid_resource = SridResource();
  request.recheck_proof = Proof();
  request.build_mode = mode;
  request.max_entries = 3;
  request.min_entries = 1;
  request.rows = BaseRows();
  return request;
}

std::vector<std::uint64_t> Rows(
    const std::vector<idx::SpatialRTreeCandidate>& candidates) {
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

idx::SpatialRTreeQueryResult Query(const idx::SpatialRTreePhysicalProvider& p,
                                   idx::SpatialRTreeQueryKind kind,
                                   idx::SpatialRTreeMbr mbr) {
  idx::SpatialRTreeQueryRequest request;
  request.provider = p;
  request.kind = kind;
  request.query_mbr = std::move(mbr);
  request.recheck_proof = Proof();
  return idx::QuerySpatialRTreePhysicalProvider(request);
}

void VerifySearchNearestSerializeAndReopen() {
  const auto built = idx::BuildSpatialRTreePhysicalProvider(BuildRequest());
  Require(built.ok(), "incremental R-tree build failed");
  Require(built.provider.mbr_encoding_present &&
              built.provider.insert_search_split_merge_present &&
              built.provider.nearest_neighbor_priority_queue_present &&
              built.provider.srid_epoch_cache_present &&
              built.provider.str_bulk_build_present,
          "spatial R-tree physical surfaces missing");
  Require(built.provider.tree_height > 1 && built.provider.split_count > 0,
          "R-tree insertion did not split into internal pages");
  Require(HasEvidence(built.provider.evidence,
                      idx::kSpatialRTreePhysicalProviderSearchKey),
          "neutral runtime evidence missing");
  RequireNoRuntimeLeak(built.provider.evidence);

  const auto encoded = idx::EncodeSpatialRTreeMbr(Mbr(0.0, 0.0, 1.0, 1.0));
  const auto decoded = idx::DecodeSpatialRTreeMbr(encoded, 2, 4326);
  Require(decoded.dimensions == 2 &&
              decoded.srid == 4326 &&
              decoded.min == std::vector<double>({0.0, 0.0}) &&
              decoded.max == std::vector<double>({1.0, 1.0}),
          "MBR encode/decode path changed");

  const auto point = Query(built.provider,
                           idx::SpatialRTreeQueryKind::point,
                           Mbr(0.25, 0.25, 0.25, 0.25));
  Require(point.ok() &&
              Rows(point.candidates) == std::vector<std::uint64_t>({10, 40}),
          "point MBR search changed");
  const auto intersects = Query(built.provider,
                                idx::SpatialRTreeQueryKind::intersects,
                                Mbr(3.5, 0.5, 5.5, 5.5));
  Require(intersects.ok() &&
              Rows(intersects.candidates) ==
                  std::vector<std::uint64_t>({20, 30, 50}),
          "intersects MBR search changed");
  const auto contains = Query(built.provider,
                              idx::SpatialRTreeQueryKind::contains,
                              Mbr(2.5, 2.5, 3.0, 3.0));
  Require(contains.ok() &&
              Rows(contains.candidates) == std::vector<std::uint64_t>({20}),
          "contains MBR search changed");
  const auto range = Query(built.provider,
                           idx::SpatialRTreeQueryKind::range,
                           Mbr(0.0, 0.0, 3.1, 0.2));
  Require(range.ok() &&
              Rows(range.candidates) ==
                  std::vector<std::uint64_t>({10, 40, 50}),
          "range MBR search changed");
  for (const auto& result : {point, intersects, contains, range}) {
    Require(result.mbr_predicate_evaluated &&
                result.candidate_rows_only &&
                !result.final_rows_authorized &&
                result.nodes_visited > 0 &&
                result.entries_examined > 0,
            "MBR query omitted candidate-only traversal evidence");
    for (const auto& candidate : result.candidates) {
      Require(candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted &&
                  !candidate.source_recheck_evidence_ref.empty(),
              "candidate leaked finality or omitted exact recheck proof");
    }
    RequireNoRuntimeLeak(result.evidence);
  }

  idx::SpatialRTreeQueryRequest nearest;
  nearest.provider = built.provider;
  nearest.kind = idx::SpatialRTreeQueryKind::nearest;
  nearest.query_point = {4.9, 0.9};
  nearest.top_k = 2;
  nearest.recheck_proof = Proof();
  const auto nearest_result = idx::QuerySpatialRTreePhysicalProvider(nearest);
  Require(nearest_result.ok() &&
              nearest_result.priority_queue_used &&
              Rows(nearest_result.candidates) ==
                  std::vector<std::uint64_t>({50, 20}),
          "nearest-neighbor priority queue result changed");

  const auto serialized =
      idx::SerializeSpatialRTreePhysicalProvider(built.provider);
  Require(serialized.ok(), "serialization failed");
  const auto serialized_again =
      idx::SerializeSpatialRTreePhysicalProvider(built.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      ("scratchbird_spatial_rtree_physical_provider_gate_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".sbrtr");
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "persisted bytes changed");

  idx::SpatialRTreeOpenRequest open;
  open.bytes = serialized.bytes;
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
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
  open.expected_srid_resource_epoch_present = true;
  open.expected_srid_resource_epoch =
      built.provider.srid_resource.resource_epoch;
  open.expected_srid_present = true;
  open.expected_srid = built.provider.srid_resource.srid;
  open.recheck_proof = Proof();
  const auto opened = idx::OpenSpatialRTreePhysicalProvider(open);
  Require(opened.ok(), "clean reopen failed");
  Require(idx::SerializeSpatialRTreePhysicalProvider(opened.provider).bytes ==
              serialized.bytes,
          "reopen serialization equivalence failed");

  auto corrupt = serialized.bytes;
  corrupt.back() ^= 0x33;
  open.bytes = corrupt;
  const auto corrupt_result = idx::OpenSpatialRTreePhysicalProvider(open);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class ==
                  idx::SpatialRTreeOpenClass::bad_checksum,
          "corrupt serialized artifact did not fail closed");
}

void VerifyStrBulkBuild() {
  auto request = BuildRequest(idx::SpatialRTreeBuildMode::str_bulk);
  request.rows.clear();
  for (std::uint64_t row = 1; row <= 18; ++row) {
    const double x = static_cast<double>(row % 6);
    const double y = static_cast<double>(row / 6);
    request.rows.push_back(
        {Locator(100 + row), Mbr(x, y, x + 0.25, y + 0.25), "bulk"});
  }
  const auto built = idx::BuildSpatialRTreePhysicalProvider(request);
  Require(built.ok() &&
              built.used_str_bulk_build &&
              built.provider.tree_height > 1 &&
              built.provider.nodes.size() >= 6,
          "STR bulk build did not create packed physical R-tree nodes");
  const auto found = Query(built.provider,
                           idx::SpatialRTreeQueryKind::intersects,
                           Mbr(1.0, 1.0, 2.1, 2.1));
  Require(found.ok() && Rows(found.candidates).size() >= 4,
          "STR bulk-built tree was not searchable");
}

void VerifyEmptyAndTopologyValidation() {
  auto empty_request = BuildRequest(idx::SpatialRTreeBuildMode::str_bulk);
  empty_request.rows.clear();
  const auto empty = idx::BuildSpatialRTreePhysicalProvider(empty_request);
  Require(empty.ok() &&
              empty.provider.nodes.empty() &&
              empty.provider.tree_height == 0,
          "empty spatial R-tree provider did not build as an empty provider");
  const auto empty_query = Query(empty.provider,
                                 idx::SpatialRTreeQueryKind::intersects,
                                 Mbr(0.0, 0.0, 1.0, 1.0));
  Require(empty_query.ok() && empty_query.candidates.empty(),
          "empty spatial R-tree provider query did not return empty result");
  const auto empty_serialized =
      idx::SerializeSpatialRTreePhysicalProvider(empty.provider);
  Require(empty_serialized.ok(), "empty provider serialization failed");

  const auto built = idx::BuildSpatialRTreePhysicalProvider(BuildRequest());
  Require(built.ok(), "fixture build failed for topology validation");
  auto orphaned = built.provider;
  auto orphan_node = orphaned.nodes.front();
  orphan_node.node_id = static_cast<std::uint32_t>(orphaned.nodes.size());
  orphaned.nodes.push_back(std::move(orphan_node));
  Require(!idx::SerializeSpatialRTreePhysicalProvider(orphaned).ok(),
          "orphan R-tree node was accepted by provider validation");

  auto child_cover_drift = built.provider;
  const auto root = child_cover_drift.root_node_id;
  Require(!child_cover_drift.nodes[root].leaf,
          "fixture root should be internal for child-cover drift test");
  child_cover_drift.nodes[root].entries.front().mbr.min[0] -= 0.5;
  Require(!idx::SerializeSpatialRTreePhysicalProvider(child_cover_drift).ok(),
          "child cover drift was accepted by provider validation");
}

void VerifyMutationMaintenance() {
  auto request = BuildRequest();
  request.rows = {BaseRows()[0], BaseRows()[1], BaseRows()[2]};
  const auto built = idx::BuildSpatialRTreePhysicalProvider(request);
  Require(built.ok() && built.provider.split_count == 0,
          "small fixture should start without split");

  idx::SpatialRTreeMutation insert;
  insert.kind = idx::SpatialRTreeMutationKind::insert_row;
  insert.expected_provider_generation_present = true;
  insert.expected_provider_generation = built.provider.provider_generation;
  insert.expected_descriptor_epoch_present = true;
  insert.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
  insert.expected_srid_resource_epoch_present = true;
  insert.expected_srid_resource_epoch =
      built.provider.srid_resource.resource_epoch;
  insert.after_row_present = true;
  insert.after_row = BaseRows()[3];
  insert.recheck_proof = Proof();
  const auto inserted =
      idx::ApplySpatialRTreePhysicalMutation(built.provider, insert);
  Require(inserted.ok() &&
              inserted.split_performed &&
              inserted.provider.provider_generation ==
                  built.provider.provider_generation + 1,
          "insert maintenance did not split or advance generation");
  const auto point = Query(inserted.provider,
                           idx::SpatialRTreeQueryKind::point,
                           Mbr(0.25, 0.25, 0.25, 0.25));
  Require(point.ok() &&
              Rows(point.candidates) == std::vector<std::uint64_t>({10, 40}),
          "inserted MBR was not searchable");

  idx::SpatialRTreeMutation stale = insert;
  stale.expected_provider_generation = built.provider.provider_generation;
  stale.after_row = BaseRows()[4];
  Require(!idx::ApplySpatialRTreePhysicalMutation(inserted.provider, stale).ok(),
          "stale generation mutation was accepted");

  idx::SpatialRTreeMutation erase;
  erase.kind = idx::SpatialRTreeMutationKind::delete_row;
  erase.expected_provider_generation_present = true;
  erase.expected_provider_generation = inserted.provider.provider_generation;
  erase.expected_descriptor_epoch_present = true;
  erase.expected_descriptor_epoch =
      inserted.provider.descriptor.descriptor_epoch;
  erase.expected_srid_resource_epoch_present = true;
  erase.expected_srid_resource_epoch =
      inserted.provider.srid_resource.resource_epoch;
  erase.before_row_present = true;
  erase.before_row = BaseRows()[1];
  erase.recheck_proof = Proof();
  auto wrong_source = erase;
  wrong_source.before_row.exact_source_recheck_evidence_ref = "wrong_source";
  Require(!idx::ApplySpatialRTreePhysicalMutation(inserted.provider,
                                                  wrong_source)
               .ok(),
          "delete accepted stale exact source evidence reference");
  const auto erased =
      idx::ApplySpatialRTreePhysicalMutation(inserted.provider, erase);
  Require(erased.ok() &&
              erased.merge_performed &&
              erased.tombstone_written &&
              erased.provider.merge_count > inserted.provider.merge_count,
          "delete/tombstone merge maintenance failed");
  const auto deleted_lookup = Query(erased.provider,
                                    idx::SpatialRTreeQueryKind::contains,
                                    Mbr(2.5, 2.5, 3.0, 3.0));
  Require(deleted_lookup.ok() && deleted_lookup.candidates.empty(),
          "deleted row remained visible as candidate evidence");
}

void VerifyFailClosedDiagnostics() {
  auto missing = BuildRequest();
  missing.recheck_proof = {};
  const auto missing_result =
      idx::BuildSpatialRTreePhysicalProvider(missing);
  Require(!missing_result.ok() &&
              missing_result.diagnostic.diagnostic_code ==
                  "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
          "missing exact recheck proof did not fail closed");

  auto invalid_mbr = BuildRequest();
  invalid_mbr.rows[0].mbr.min[0] = 9.0;
  const auto invalid_mbr_result =
      idx::BuildSpatialRTreePhysicalProvider(invalid_mbr);
  Require(!invalid_mbr_result.ok() &&
              invalid_mbr_result.diagnostic.diagnostic_code ==
                  "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.INVALID_MBR",
          "invalid MBR did not fail closed");

  auto unsupported = BuildRequest();
  unsupported.descriptor.dimensions = 5;
  const auto unsupported_result =
      idx::BuildSpatialRTreePhysicalProvider(unsupported);
  Require(!unsupported_result.ok() &&
              unsupported_result.diagnostic.diagnostic_code ==
                  "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UNSUPPORTED_GEOMETRY_PROFILE",
          "unsupported geometry dimension profile did not fail closed");

  auto authority = BuildRequest();
  authority.srid_resource.provider_finality_authority_claimed = true;
  const auto authority_result =
      idx::BuildSpatialRTreePhysicalProvider(authority);
  Require(!authority_result.ok() &&
              authority_result.diagnostic.diagnostic_code ==
                  "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
          "unsafe authority claim was accepted");

  const auto built = idx::BuildSpatialRTreePhysicalProvider(BuildRequest());
  Require(built.ok(), "fixture build failed for runtime refusals");

  idx::SpatialRTreeQueryRequest query;
  query.provider = built.provider;
  query.kind = idx::SpatialRTreeQueryKind::intersects;
  query.query_mbr = Mbr(0.0, 0.0, 1.0, 1.0);
  query.recheck_proof = Proof();
  query.descriptor_store_scan = true;
  Require(!idx::QuerySpatialRTreePhysicalProvider(query).ok(),
          "descriptor scan fallback did not fail closed");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QuerySpatialRTreePhysicalProvider(query).ok(),
          "behavior scan fallback did not fail closed");
  query.behavior_store_scan = false;
  query.contract_only_fallback = true;
  Require(!idx::QuerySpatialRTreePhysicalProvider(query).ok(),
          "contract-only fallback did not fail closed");
  query.contract_only_fallback = false;
  query.provider_only_fallback = true;
  Require(!idx::QuerySpatialRTreePhysicalProvider(query).ok(),
          "provider-only fallback did not fail closed");
  query.provider_only_fallback = false;
  query.srid_resource_epoch_current = false;
  const auto stale_srid = idx::QuerySpatialRTreePhysicalProvider(query);
  Require(!stale_srid.ok() &&
              stale_srid.diagnostic.diagnostic_code ==
                  "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
          "stale SRID resource epoch query did not fail closed");
  query.srid_resource_epoch_current = true;
  query.descriptor_epoch_current = false;
  const auto stale_descriptor = idx::QuerySpatialRTreePhysicalProvider(query);
  Require(!stale_descriptor.ok() &&
              stale_descriptor.diagnostic.diagnostic_code ==
                  "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
          "stale descriptor query did not fail closed");

  const auto serialized =
      idx::SerializeSpatialRTreePhysicalProvider(built.provider);
  Require(serialized.ok(), "serialization failed for reopen refusals");
  idx::SpatialRTreeOpenRequest open;
  open.bytes = serialized.bytes;
  open.recheck_proof = Proof();
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = 999;
  Require(!idx::OpenSpatialRTreePhysicalProvider(open).ok(),
          "stale descriptor epoch reopen did not fail closed");
  open.expected_descriptor_epoch_present = false;
  open.expected_srid_resource_epoch_present = true;
  open.expected_srid_resource_epoch = 999;
  const auto stale_open = idx::OpenSpatialRTreePhysicalProvider(open);
  Require(!stale_open.ok() &&
              stale_open.open_class ==
                  idx::SpatialRTreeOpenClass::stale_srid_resource_epoch,
          "stale SRID resource epoch reopen did not fail closed");
  idx::SpatialRTreeOpenRequest corrupt;
  corrupt.bytes = {0x00, 0x01, 0x02};
  corrupt.recheck_proof = Proof();
  const auto corrupt_result = idx::OpenSpatialRTreePhysicalProvider(corrupt);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class ==
                  idx::SpatialRTreeOpenClass::corrupt_payload,
          "short corrupt payload did not fail closed");
}

}  // namespace

int main() {
  VerifySearchNearestSerializeAndReopen();
  VerifyStrBulkBuild();
  VerifyEmptyAndTopologyValidation();
  VerifyMutationMaintenance();
  VerifyFailClosedDiagnostics();
  std::cout << "spatial_rtree_physical_provider_gate=passed\n";
  return EXIT_SUCCESS;
}
