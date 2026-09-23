// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_ordered_ingest.hpp"
#include "ordered_ingest.hpp"
#include "uuid.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace api = scratchbird::engine::internal_api;
namespace dml = api::dml;
namespace page = scratchbird::storage::page;
static void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(EXIT_FAILURE); }
}

int main() {
  const auto relation = scratchbird::tests::FixtureUuid(6047, 10);
  const auto policy = scratchbird::tests::FixtureUuid(6047, 11);
  page::OrderedIngestPhysicalClusteringRequest request;
  request.current_descriptor = {relation, "city", policy, 7, true};
  request.requested_placement_key_column = "city";
  request.physical_clustering_requested = true;
  request.explicit_policy_present = true;
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = policy;
    changed.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
    request.requested_policy_uuid = changed;
    const auto result = page::ResolveOrderedIngestPhysicalClustering(request);
    Check(result.ok && result.descriptor_updated &&
              result.descriptor.policy_uuid == changed &&
              result.descriptor.relation_uuid == relation &&
              result.descriptor.descriptor_generation == 8,
          "clustering policy identity lost bits or failed to advance generation");
  }
  request.requested_policy_uuid = {};
  auto result = page::ResolveOrderedIngestPhysicalClustering(request);
  Check(result.ok && !result.descriptor_updated && result.descriptor.policy_uuid == policy,
        "absent replacement policy changed current identity");
  request.requested_placement_key_column = "other";
  result = page::ResolveOrderedIngestPhysicalClustering(request);
  Check(!result.ok && result.descriptor.policy_uuid == policy &&
            result.descriptor.relation_uuid == relation &&
            result.descriptor.descriptor_generation == 7,
        "refused key change mutated binary descriptor");

  dml::DirectPhysicalBulkAppendRequest append;
  append.target_table.uuid = relation;
  append.option_envelopes = {"ordered_ingest=enabled", "ordered_ingest.placement_key=city",
      "physical_clustering=enabled", "physical_clustering.policy_uuid=" +
      scratchbird::core::uuid::UuidToString(policy)};
  api::CrudRowVersionRecord first, second;
  first.row_uuid = scratchbird::tests::FixtureUuid(6047, 20);
  second.row_uuid = first.row_uuid;
  second.row_uuid.bytes[0] = 0;
  second.row_uuid.bytes[1] = '\n';
  second.row_uuid.bytes[2] = '|';
  second.row_uuid.bytes[15] ^= 1;
  const std::vector<api::CrudRowVersionRecord> original{first, second};
  const std::vector<std::vector<std::pair<std::string, std::string>>> values{
      {{"city", "zurich"}}, {{"city", "berlin"}}};
  auto staged = original;
  auto logical = values;
  auto selected = dml::detail::ApplyDirectOrderedIngestPlan(append, &staged, &logical);
  Check(selected.ok && selected.selected && staged.size() == 2 &&
            staged[0].row_uuid == second.row_uuid && staged[1].row_uuid == first.row_uuid &&
            logical[0] == values[1] && logical[1] == values[0],
        "placement permutation changed binary row identity or logical alignment");
  for (const char* invalid : {"policy-old", "00000000-0000-0000-0000-000000000000",
                             "018f1234-5678-4abc-8def-0123456789ab"}) {
    append.option_envelopes.back() = std::string("physical_clustering.policy_uuid=") + invalid;
    staged = original;
    logical = values;
    selected = dml::detail::ApplyDirectOrderedIngestPlan(append, &staged, &logical);
    Check(!selected.ok && selected.failure_reason == "physical_clustering_policy_uuid_invalid" &&
              staged[0].row_uuid == first.row_uuid && staged[1].row_uuid == second.row_uuid &&
              logical == values,
          "invalid policy text was accepted or changed staged input");
  }
}
