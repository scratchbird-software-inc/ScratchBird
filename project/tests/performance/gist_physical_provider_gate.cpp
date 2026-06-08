// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "gist_physical_provider.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "gist_physical_provider_gate: " << message << '\n';
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
  locator.row_uuid = UuidWithSuffix("41414141-4141-7141-8141-", row);
  locator.version_uuid = UuidWithSuffix("51515151-5151-7151-8151-", row);
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

idx::GistExactRecheckProof Proof() {
  idx::GistExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_geometry_available = true;
  proof.exact_predicate_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "gist_exact_source_mga_security_recheck_contract";
  return proof;
}

idx::SpatialRTreeDescriptor SpatialDescriptor() {
  idx::SpatialRTreeDescriptor descriptor;
  descriptor.dimensions = 2;
  descriptor.descriptor_epoch = 41;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.supports_point = true;
  descriptor.supports_mbr = true;
  return descriptor;
}

idx::SpatialRTreeSridResource SridResource() {
  idx::SpatialRTreeSridResource resource;
  resource.resource_uuid = "61616161-6161-7161-8161-616161616161";
  resource.srid = 4326;
  resource.resource_epoch = 43;
  resource.coordinate_order = "xy";
  resource.deterministic = true;
  resource.safe = true;
  resource.cache_present = true;
  return resource;
}

idx::GistOpclassRuntime Opclass() {
  return idx::MakeSpatialMbrGistOpclass(47, 53, 2, 4326);
}

std::vector<idx::GistSourceRow> BaseRows() {
  return {{Locator(10), Mbr(0.0, 0.0, 1.0, 1.0), "src10"},
          {Locator(20), Mbr(2.0, 2.0, 4.0, 4.0), "src20"},
          {Locator(30), Mbr(5.0, 5.0, 6.0, 6.0), "src30"},
          {Locator(40), Mbr(-1.0, -1.0, 0.5, 0.5), "src40"},
          {Locator(50), Mbr(3.0, 0.0, 5.0, 1.0), "src50"},
          {Locator(60), Mbr(8.0, 8.0, 9.0, 9.0), "src60"}};
}

idx::GistBuildRequest BuildRequest(
    idx::SpatialRTreeBuildMode mode =
        idx::SpatialRTreeBuildMode::incremental_insert) {
  idx::GistBuildRequest request;
  request.relation_uuid = "71717171-7171-7171-8171-717171717171";
  request.index_uuid = "72727272-7272-7272-8272-727272727272";
  request.provider_uuid = "73737373-7373-7373-8373-737373737373";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.spatial_descriptor = SpatialDescriptor();
  request.srid_resource = SridResource();
  request.opclass = Opclass();
  request.recheck_proof = Proof();
  request.build_mode = mode;
  request.max_entries = 3;
  request.min_entries = 1;
  request.rows = BaseRows();
  return request;
}

std::vector<std::uint64_t> Rows(
    const std::vector<idx::GistCandidate>& candidates) {
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

idx::GistQueryResult Query(const idx::GistPhysicalProvider& provider,
                           idx::GistPredicateStrategy strategy,
                           idx::SpatialRTreeMbr mbr) {
  idx::GistQueryRequest request;
  request.provider = provider;
  request.opclass = Opclass();
  request.strategy = strategy;
  request.query_mbr = std::move(mbr);
  request.recheck_proof = Proof();
  return idx::QueryGistPhysicalProvider(request);
}

void VerifyBuildQueryNearestSerializeAndReopen() {
  const auto built = idx::BuildGistPhysicalProvider(BuildRequest());
  Require(built.ok() &&
              built.used_spatial_rtree_provider &&
              built.all_methods_exercised,
          "GiST build did not exercise all required opclass methods");
  Require(built.provider.opclass_required_for_runtime &&
              built.provider.spatial_rtree_provider_consumed &&
              built.provider.consistent_call_count > 0 &&
              built.provider.union_call_count > 0 &&
              built.provider.compress_call_count > 0 &&
              built.provider.decompress_call_count > 0 &&
              built.provider.penalty_call_count > 0 &&
              built.provider.picksplit_call_count > 0 &&
              built.provider.same_call_count > 0 &&
              built.provider.distance_call_count > 0,
          "GiST provider did not record concrete method use");
  Require(HasEvidence(built.provider.evidence, idx::kGistPhysicalProviderSearchKey),
          "neutral GiST runtime evidence missing");
  Require(HasEvidence(built.provider.physical_tree.evidence,
                      idx::kSpatialRTreePhysicalProviderSearchKey),
          "GiST did not consume the spatial R-tree provider path");
  RequireNoRuntimeLeak(built.provider.evidence);

  const auto point = Query(built.provider,
                           idx::GistPredicateStrategy::point,
                           Mbr(0.25, 0.25, 0.25, 0.25));
  Require(point.ok() &&
              point.spatial_rtree_provider_used &&
              point.opclass_consistent_used &&
              Rows(point.candidates) == std::vector<std::uint64_t>({10, 40}),
          "GiST point consistent query changed");
  const auto intersects = Query(built.provider,
                                idx::GistPredicateStrategy::intersects,
                                Mbr(3.5, 0.5, 5.5, 5.5));
  Require(intersects.ok() &&
              Rows(intersects.candidates) ==
                  std::vector<std::uint64_t>({20, 30, 50}),
          "GiST intersects query changed");
  const auto contains = Query(built.provider,
                              idx::GistPredicateStrategy::contains,
                              Mbr(2.5, 2.5, 3.0, 3.0));
  Require(contains.ok() &&
              Rows(contains.candidates) == std::vector<std::uint64_t>({20}),
          "GiST contains query changed");
  for (const auto& result : {point, intersects, contains}) {
    Require(result.candidate_rows_only &&
                !result.final_rows_authorized &&
                result.nodes_visited > 0 &&
                result.entries_examined > 0,
            "GiST query omitted candidate-only traversal evidence");
    for (const auto& candidate : result.candidates) {
      Require(candidate.from_spatial_rtree_provider &&
                  candidate.opclass_consistent &&
                  candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted &&
                  !candidate.source_recheck_evidence_ref.empty(),
              "GiST candidate leaked authority or omitted recheck proof");
    }
    RequireNoRuntimeLeak(result.evidence);
  }

  idx::GistQueryRequest nearest;
  nearest.provider = built.provider;
  nearest.opclass = Opclass();
  nearest.strategy = idx::GistPredicateStrategy::nearest;
  nearest.query_point = {4.9, 0.9};
  nearest.top_k = 2;
  nearest.recheck_proof = Proof();
  const auto nearest_result = idx::QueryGistPhysicalProvider(nearest);
  Require(nearest_result.ok() &&
              nearest_result.priority_queue_used &&
              nearest_result.opclass_distance_used &&
              Rows(nearest_result.candidates) ==
                  std::vector<std::uint64_t>({50, 20}),
          "GiST nearest distance query changed");

  auto custom_distance = Opclass();
  const auto decompress = custom_distance.methods.decompress;
  custom_distance.methods.distance =
      [decompress](const idx::GistCompressedKey& key,
                   const std::vector<double>&) {
        const auto mbr = decompress(key);
        return mbr.dimensions == 0
                   ? std::numeric_limits<double>::infinity()
                   : 100.0 - mbr.min[0];
      };
  nearest.opclass = custom_distance;
  nearest.top_k = 2;
  const auto custom_nearest = idx::QueryGistPhysicalProvider(nearest);
  Require(custom_nearest.ok() &&
              custom_nearest.opclass_distance_used &&
              Rows(custom_nearest.candidates) ==
                  std::vector<std::uint64_t>({60, 30}),
          "GiST nearest did not apply opclass distance before top-k trimming");

  const auto serialized = idx::SerializeGistPhysicalProvider(built.provider);
  Require(serialized.ok(), "GiST serialization failed");
  const auto serialized_again =
      idx::SerializeGistPhysicalProvider(built.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "GiST serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      ("scratchbird_gist_physical_provider_gate_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".sbgist");
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "persisted GiST bytes changed");

  idx::GistOpenRequest open;
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
      built.provider.physical_tree.descriptor.descriptor_epoch;
  open.expected_srid_resource_epoch_present = true;
  open.expected_srid_resource_epoch =
      built.provider.physical_tree.srid_resource.resource_epoch;
  open.expected_srid_present = true;
  open.expected_srid = built.provider.opclass.srid;
  open.recheck_proof = Proof();
  const auto opened = idx::OpenGistPhysicalProvider(open);
  Require(opened.ok(), "clean GiST reopen failed");
  Require(idx::SerializeGistPhysicalProvider(opened.provider).bytes ==
              serialized.bytes,
          "GiST reopen serialization equivalence failed");

  auto corrupt = serialized.bytes;
  corrupt.back() ^= 0x33;
  open.bytes = corrupt;
  const auto corrupt_result = idx::OpenGistPhysicalProvider(open);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class == idx::GistOpenClass::bad_checksum,
          "corrupt GiST artifact did not fail closed");
}

void VerifyMutationMaintenance() {
  auto request = BuildRequest();
  request.rows = {BaseRows()[0], BaseRows()[1], BaseRows()[2]};
  const auto built = idx::BuildGistPhysicalProvider(request);
  Require(built.ok(), "small GiST fixture build failed");

  idx::GistMutation insert;
  insert.kind = idx::GistMutationKind::insert_row;
  insert.expected_provider_generation_present = true;
  insert.expected_provider_generation = built.provider.provider_generation;
  insert.expected_opclass_epoch_present = true;
  insert.expected_opclass_epoch = built.provider.opclass.opclass_epoch;
  insert.expected_resource_epoch_present = true;
  insert.expected_resource_epoch = built.provider.opclass.resource_epoch;
  insert.expected_descriptor_epoch_present = true;
  insert.expected_descriptor_epoch =
      built.provider.physical_tree.descriptor.descriptor_epoch;
  insert.expected_srid_resource_epoch_present = true;
  insert.expected_srid_resource_epoch =
      built.provider.physical_tree.srid_resource.resource_epoch;
  insert.after_row_present = true;
  insert.after_row = BaseRows()[3];
  insert.recheck_proof = Proof();
  const auto inserted =
      idx::ApplyGistPhysicalMutation(built.provider, Opclass(), insert);
  Require(inserted.ok() &&
              inserted.spatial_rtree_provider_used &&
              inserted.split_performed &&
              inserted.provider.provider_generation ==
                  built.provider.provider_generation + 1,
          "GiST insert did not route through spatial split maintenance");

  idx::GistMutation stale = insert;
  stale.expected_provider_generation = built.provider.provider_generation;
  stale.after_row = BaseRows()[4];
  Require(!idx::ApplyGistPhysicalMutation(inserted.provider, Opclass(), stale)
               .ok(),
          "GiST stale generation mutation was accepted");

  idx::GistMutation erase;
  erase.kind = idx::GistMutationKind::delete_row;
  erase.expected_provider_generation_present = true;
  erase.expected_provider_generation = inserted.provider.provider_generation;
  erase.expected_opclass_epoch_present = true;
  erase.expected_opclass_epoch = inserted.provider.opclass.opclass_epoch;
  erase.expected_resource_epoch_present = true;
  erase.expected_resource_epoch = inserted.provider.opclass.resource_epoch;
  erase.expected_descriptor_epoch_present = true;
  erase.expected_descriptor_epoch =
      inserted.provider.physical_tree.descriptor.descriptor_epoch;
  erase.expected_srid_resource_epoch_present = true;
  erase.expected_srid_resource_epoch =
      inserted.provider.physical_tree.srid_resource.resource_epoch;
  erase.before_row_present = true;
  erase.before_row = BaseRows()[1];
  erase.recheck_proof = Proof();
  auto wrong_source = erase;
  wrong_source.before_row.exact_source_recheck_evidence_ref = "wrong_source";
  Require(!idx::ApplyGistPhysicalMutation(inserted.provider,
                                          Opclass(),
                                          wrong_source)
               .ok(),
          "GiST delete accepted stale exact source evidence reference");
  const auto erased =
      idx::ApplyGistPhysicalMutation(inserted.provider, Opclass(), erase);
  Require(erased.ok() &&
              erased.merge_performed &&
              erased.tombstone_written,
          "GiST delete/tombstone maintenance failed");
  const auto deleted_lookup = Query(erased.provider,
                                    idx::GistPredicateStrategy::contains,
                                    Mbr(2.5, 2.5, 3.0, 3.0));
  Require(deleted_lookup.ok() && deleted_lookup.candidates.empty(),
          "deleted GiST row remained visible as candidate evidence");
}

void VerifyEmptyProvider() {
  auto request = BuildRequest(idx::SpatialRTreeBuildMode::str_bulk);
  request.rows.clear();
  const auto built = idx::BuildGistPhysicalProvider(request);
  Require(built.ok() &&
              built.used_spatial_rtree_provider &&
              built.provider.physical_tree.nodes.empty(),
          "empty GiST provider did not build through spatial provider");
  const auto empty_query = Query(built.provider,
                                 idx::GistPredicateStrategy::intersects,
                                 Mbr(0.0, 0.0, 1.0, 1.0));
  Require(empty_query.ok() &&
              empty_query.spatial_rtree_provider_used &&
              empty_query.candidates.empty(),
          "empty GiST query did not return an empty candidate set");
  const auto serialized = idx::SerializeGistPhysicalProvider(built.provider);
  Require(serialized.ok(), "empty GiST provider serialization failed");
}

void VerifyFailClosedDiagnostics() {
  auto missing_opclass = BuildRequest();
  missing_opclass.opclass.methods.distance = {};
  const auto missing_opclass_result =
      idx::BuildGistPhysicalProvider(missing_opclass);
  Require(!missing_opclass_result.ok() &&
              missing_opclass_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
          "missing GiST distance method did not fail closed");

  auto nondeterministic = BuildRequest();
  nondeterministic.opclass.descriptor.deterministic = false;
  const auto nondeterministic_result =
      idx::BuildGistPhysicalProvider(nondeterministic);
  Require(!nondeterministic_result.ok() &&
              nondeterministic_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
          "nondeterministic GiST opclass was accepted");

  auto unregistered = BuildRequest();
  unregistered.opclass.descriptor.registered = false;
  const auto unregistered_result =
      idx::BuildGistPhysicalProvider(unregistered);
  Require(!unregistered_result.ok() &&
              unregistered_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
          "unregistered GiST opclass was accepted");

  auto bad_compress = BuildRequest();
  bad_compress.opclass.methods.compress =
      [](const idx::SpatialRTreeMbr& mbr) {
        idx::GistCompressedKey key;
        key.dimensions = mbr.dimensions;
        key.srid = mbr.srid;
        key.bytes = {0x01, 0x02};
        key.deterministic = true;
        key.valid = true;
        return key;
      };
  const auto bad_compress_result =
      idx::BuildGistPhysicalProvider(bad_compress);
  Require(!bad_compress_result.ok() &&
              bad_compress_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
          "invalid GiST compressed key was accepted");

  auto incompatible = BuildRequest();
  incompatible.opclass = idx::MakeSpatialMbrGistOpclass(47, 53, 3, 4326);
  const auto incompatible_result =
      idx::BuildGistPhysicalProvider(incompatible);
  Require(!incompatible_result.ok() &&
              incompatible_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.INCOMPATIBLE_SPATIAL_PROFILE",
          "GiST incompatible dimensions were accepted");

  auto incompatible_srid = BuildRequest();
  incompatible_srid.opclass =
      idx::MakeSpatialMbrGistOpclass(47, 53, 2, 3857);
  const auto incompatible_srid_result =
      idx::BuildGistPhysicalProvider(incompatible_srid);
  Require(!incompatible_srid_result.ok() &&
              incompatible_srid_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.INCOMPATIBLE_SPATIAL_PROFILE",
          "GiST incompatible SRID was accepted");

  auto authority = BuildRequest();
  authority.opclass.descriptor.provider_finality_authority_claimed = true;
  const auto authority_result =
      idx::BuildGistPhysicalProvider(authority);
  Require(!authority_result.ok() &&
              authority_result.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
          "unsafe GiST authority claim was accepted");

  const auto built = idx::BuildGistPhysicalProvider(BuildRequest());
  Require(built.ok(), "GiST fixture build failed for runtime refusals");

  idx::GistQueryRequest query;
  query.provider = built.provider;
  query.opclass = Opclass();
  query.strategy = idx::GistPredicateStrategy::intersects;
  query.query_mbr = Mbr(0.0, 0.0, 1.0, 1.0);
  query.recheck_proof = Proof();
  query.descriptor_store_scan = true;
  Require(!idx::QueryGistPhysicalProvider(query).ok(),
          "GiST descriptor scan fallback did not fail closed");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QueryGistPhysicalProvider(query).ok(),
          "GiST behavior scan fallback did not fail closed");
  query.behavior_store_scan = false;
  query.contract_only_fallback = true;
  Require(!idx::QueryGistPhysicalProvider(query).ok(),
          "GiST contract-only fallback did not fail closed");
  query.contract_only_fallback = false;
  query.provider_only_fallback = true;
  Require(!idx::QueryGistPhysicalProvider(query).ok(),
          "GiST provider-only fallback did not fail closed");
  query.provider_only_fallback = false;
  query.resource_epoch_current = false;
  const auto stale_resource = idx::QueryGistPhysicalProvider(query);
  Require(!stale_resource.ok() &&
              stale_resource.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.STALE_RESOURCE_EPOCH",
          "stale GiST resource epoch query did not fail closed");
  query.resource_epoch_current = true;
  query.opclass_epoch_current = false;
  const auto stale_opclass = idx::QueryGistPhysicalProvider(query);
  Require(!stale_opclass.ok() &&
              stale_opclass.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.STALE_OPCLASS_EPOCH",
          "stale GiST opclass epoch query did not fail closed");
  query.opclass_epoch_current = true;
  query.srid_resource_epoch_current = false;
  const auto stale_srid = idx::QueryGistPhysicalProvider(query);
  Require(!stale_srid.ok() &&
              stale_srid.diagnostic.diagnostic_code ==
                  "INDEX.GIST_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
          "stale GiST SRID resource epoch query did not fail closed");

  const auto serialized = idx::SerializeGistPhysicalProvider(built.provider);
  Require(serialized.ok(), "serialization failed for GiST reopen refusals");
  idx::GistOpenRequest open;
  open.bytes = serialized.bytes;
  open.opclass = Opclass();
  open.recheck_proof = Proof();
  open.expected_opclass_epoch_present = true;
  open.expected_opclass_epoch = 999;
  const auto stale_open = idx::OpenGistPhysicalProvider(open);
  Require(!stale_open.ok() &&
              stale_open.open_class ==
                  idx::GistOpenClass::stale_opclass_epoch,
          "stale GiST opclass epoch reopen did not fail closed");
  open.expected_opclass_epoch_present = false;
  open.expected_resource_epoch_present = true;
  open.expected_resource_epoch = 999;
  const auto stale_resource_open = idx::OpenGistPhysicalProvider(open);
  Require(!stale_resource_open.ok() &&
              stale_resource_open.open_class ==
                  idx::GistOpenClass::stale_resource_epoch,
          "stale GiST resource epoch reopen did not fail closed");
  open.expected_resource_epoch_present = false;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = 999;
  const auto stale_descriptor_open = idx::OpenGistPhysicalProvider(open);
  Require(!stale_descriptor_open.ok() &&
              stale_descriptor_open.open_class ==
                  idx::GistOpenClass::stale_descriptor_epoch,
          "stale GiST embedded descriptor epoch reopen did not fail closed");
  open.expected_descriptor_epoch_present = false;
  open.expected_srid_resource_epoch_present = true;
  open.expected_srid_resource_epoch = 999;
  const auto stale_srid_open = idx::OpenGistPhysicalProvider(open);
  Require(!stale_srid_open.ok() &&
              stale_srid_open.open_class ==
                  idx::GistOpenClass::stale_srid_resource_epoch,
          "stale GiST embedded SRID resource epoch reopen did not fail closed");
  idx::GistOpenRequest corrupt;
  corrupt.bytes = {0x00, 0x01, 0x02};
  corrupt.opclass = Opclass();
  corrupt.recheck_proof = Proof();
  const auto corrupt_result = idx::OpenGistPhysicalProvider(corrupt);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class == idx::GistOpenClass::corrupt_payload,
          "short corrupt GiST payload did not fail closed");
}

}  // namespace

int main() {
  VerifyBuildQueryNearestSerializeAndReopen();
  VerifyMutationMaintenance();
  VerifyEmptyProvider();
  VerifyFailClosedDiagnostics();
  std::cout << "gist_physical_provider_gate=passed\n";
  return EXIT_SUCCESS;
}
