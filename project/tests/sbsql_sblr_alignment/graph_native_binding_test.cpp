// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/graph_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  using scratchbird::tests::FixtureUuid;
  auto first = FixtureUuid(1143, 1); first.bytes[9] = 0; first.bytes[15] = 0xff;
  const auto second = FixtureUuid(1143, 2);
  const auto edge = FixtureUuid(1143, 3);
  const auto reverse = FixtureUuid(1143, 4);
  const auto encoded = a::GraphUuidBytes(first);
  Check(encoded.size() == 16);
  Check(a::DecodeGraphUuid(&encoded) == first);
  const std::string legacy = "018f0000-0000-7000-8000-000000000001";
  Check(!a::DecodeGraphUuid(&legacy));
  auto malformed = encoded; malformed[8] = 0;
  Check(!a::DecodeGraphUuid(&malformed));
  a::EngineGraphQueryRequest request;
  request.max_depth = 3; request.maximum_output_rows = 16;
  request.seed_vertex_ids = {first, first};
  request.vertices = {{first, {"start"}, {}}, {second, {"end"}, {}}};
  request.edges = {{edge, first, second, "link", {}, 1.0},
                   {reverse, second, first, "back", {}, 1.0}};
  Check(a::ValidateDirectGraphCorpus(request).ok);
  Check(a::ResolveSeedVertices(request) == std::vector<a::EngineUuid>{first});
  a::EngineApiU64 batches = 0, reads = 0;
  const auto traversal = a::TraverseFrontiers(request, 1 << 20, &batches, &reads);
  Check(!traversal.resource_exhausted && !traversal.cancelled);
  Check(traversal.rows.size() == 2); // native visited set rejects the cycle
  Check(traversal.rows[0].edge_id.is_nil());
  Check(traversal.rows[1].vertex_id == second && traversal.rows[1].edge_id == edge);
  Check(traversal.rows[1].path == std::vector<a::EngineUuid>({first, second}));
  Check(traversal.rows[1].path_identity == std::vector<a::EngineUuid>({first, edge, second}));
  const auto path = a::EncodeGraphPath(traversal.rows[1].path);
  Check(path.substr(0, 9) == "SBGRPATH2" && path.size() == 9 + 8 + 32);
  Check(path.substr(17, 16) == encoded);
  const auto built = a::BuildPathRows({first, second}, {edge}, a::SortedEdges(request.edges));
  Check(built.size() == 2 && built[1].path_identity == traversal.rows[1].path_identity);
  request.vertices[1].vertex_id = first;
  Check(!a::ValidateDirectGraphCorpus(request).ok);
}
