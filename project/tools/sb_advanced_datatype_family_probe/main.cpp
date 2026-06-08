// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_advanced_family.hpp"

#include <iostream>

using namespace scratchbird::core::datatypes;

namespace {

bool Expect(bool condition, const char* name) {
  std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
  return condition;
}

AdvancedDatatypeFamilyResult Eval(CanonicalTypeId type_id,
                                  AdvancedDatatypeOperationKind operation,
                                  AdvancedDatatypeIndexKind index_kind,
                                  unsigned dimension = 0) {
  AdvancedDatatypeFamilyRequest request;
  request.type_id = type_id;
  request.operation = operation;
  request.index_kind = index_kind;
  request.vector_dimension = dimension;
  request.descriptor_profile = "probe";
  return EvaluateAdvancedDatatypeFamily(request);
}

}  // namespace

int main() {
  const auto spatial = Eval(CanonicalTypeId::geometry,
                            AdvancedDatatypeOperationKind::intersects,
                            AdvancedDatatypeIndexKind::rtree);
  const auto spatial_bad_index = Eval(CanonicalTypeId::geometry,
                                      AdvancedDatatypeOperationKind::intersects,
                                      AdvancedDatatypeIndexKind::hnsw);
  const auto vector = Eval(CanonicalTypeId::dense_vector,
                           AdvancedDatatypeOperationKind::nearest_neighbor,
                           AdvancedDatatypeIndexKind::hnsw,
                           768);
  const auto vector_missing_dim = Eval(CanonicalTypeId::dense_vector,
                                       AdvancedDatatypeOperationKind::nearest_neighbor,
                                       AdvancedDatatypeIndexKind::hnsw);
  const auto vector_bad_operation = Eval(CanonicalTypeId::dense_vector,
                                         AdvancedDatatypeOperationKind::tokenize,
                                         AdvancedDatatypeIndexKind::hnsw,
                                         768);
  const auto search = Eval(CanonicalTypeId::token_stream,
                           AdvancedDatatypeOperationKind::search_match,
                           AdvancedDatatypeIndexKind::inverted);
  const auto search_bad_index = Eval(CanonicalTypeId::search_query,
                                     AdvancedDatatypeOperationKind::rank,
                                     AdvancedDatatypeIndexKind::btree);
  const auto graph = Eval(CanonicalTypeId::graph_path,
                          AdvancedDatatypeOperationKind::path_match,
                          AdvancedDatatypeIndexKind::adjacency);
  const auto graph_bad_operation = Eval(CanonicalTypeId::graph_node,
                                        AdvancedDatatypeOperationKind::distance,
                                        AdvancedDatatypeIndexKind::adjacency);
  const auto time_series = Eval(CanonicalTypeId::time_series_value,
                                AdvancedDatatypeOperationKind::aggregate_window,
                                AdvancedDatatypeIndexKind::time_partition);
  const auto time_series_bad_index = Eval(CanonicalTypeId::time_series_value,
                                          AdvancedDatatypeOperationKind::append_point,
                                          AdvancedDatatypeIndexKind::inverted);
  const auto unsupported = Eval(CanonicalTypeId::character,
                                AdvancedDatatypeOperationKind::search_match,
                                AdvancedDatatypeIndexKind::inverted);

  const bool ok = spatial.ok() && spatial.family == AdvancedDatatypeFamily::spatial &&
                  spatial.optimizer_admitted && !spatial_bad_index.ok() &&
                  vector.ok() && vector.family == AdvancedDatatypeFamily::vector &&
                  vector.optimizer_admitted && !vector_missing_dim.ok() && !vector_bad_operation.ok() &&
                  search.ok() && search.family == AdvancedDatatypeFamily::search &&
                  search.optimizer_admitted && !search_bad_index.ok() &&
                  graph.ok() && graph.family == AdvancedDatatypeFamily::graph &&
                  graph.optimizer_admitted && !graph_bad_operation.ok() &&
                  time_series.ok() && time_series.family == AdvancedDatatypeFamily::time_series &&
                  time_series.optimizer_admitted && !time_series_bad_index.ok() && !unsupported.ok();

  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(spatial.ok() && spatial.optimizer_admitted, "spatial_rtree_admitted");
  Expect(!spatial_bad_index.ok(), "spatial_wrong_index_rejected");
  Expect(vector.ok() && vector.optimizer_admitted, "vector_hnsw_admitted");
  Expect(!vector_missing_dim.ok(), "vector_dimension_required");
  Expect(!vector_bad_operation.ok(), "vector_wrong_operation_rejected");
  Expect(search.ok() && search.optimizer_admitted, "search_inverted_admitted");
  Expect(!search_bad_index.ok(), "search_wrong_index_rejected");
  Expect(graph.ok() && graph.optimizer_admitted, "graph_adjacency_admitted");
  Expect(!graph_bad_operation.ok(), "graph_wrong_operation_rejected");
  Expect(time_series.ok() && time_series.optimizer_admitted, "time_series_partition_admitted");
  Expect(!time_series_bad_index.ok(), "time_series_wrong_index_rejected");
  std::cout << "  \"unsupported_type_rejected\": " << (!unsupported.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
