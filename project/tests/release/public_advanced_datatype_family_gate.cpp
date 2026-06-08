// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_index_optimizer_admission_api.hpp"
#include "datatype_advanced_family.hpp"
#include "datatype_binary.hpp"
#include "query/expression_api.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

using scratchbird::core::platform::byte;

bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

bool Contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

std::vector<byte> Bytes(const std::string& value) {
  return {value.begin(), value.end()};
}

dt::DatatypeDescriptorRecord Record(std::string name, std::string value) {
  dt::DatatypeDescriptorRecord record;
  record.field_name = std::move(name);
  record.payload = Bytes(value);
  return record;
}

bool EnvelopeRoundTrips(const dt::AdvancedDatatypeFamilyRequest& request,
                        const dt::AdvancedDatatypeFamilyResult& result) {
  dt::DatatypeDescriptorEnvelope envelope;
  envelope.kind = dt::DatatypeDescriptorEnvelopeKind::advanced_family_descriptor;
  envelope.integrity_profile = dt::DatatypeDescriptorIntegrityProfile::strong;
  envelope.records.push_back(Record("family", dt::AdvancedDatatypeFamilyName(result.family)));
  envelope.records.push_back(Record("type", dt::CanonicalTypeName(request.type_id)));
  envelope.records.push_back(Record("canonical_descriptor_profile",
                                    result.canonical_descriptor_profile));
  envelope.records.push_back(Record("optimizer_support_path", result.optimizer_support_path));
  const auto encoded = dt::EncodeDatatypeDescriptorEnvelope(envelope);
  if (!encoded.ok()) { return false; }
  const auto decoded = dt::DecodeDatatypeDescriptorEnvelope(encoded.encoded);
  return decoded.ok() &&
         decoded.envelope.kind == dt::DatatypeDescriptorEnvelopeKind::advanced_family_descriptor &&
         decoded.envelope.records.size() == envelope.records.size();
}

struct AdvancedFamilyCase {
  dt::CanonicalTypeId type_id;
  dt::AdvancedDatatypeOperationKind operation;
  dt::AdvancedDatatypeIndexKind index_kind;
  std::string descriptor_profile;
  std::string expected_family;
  std::string canonical_fragment;
  scratchbird::core::platform::u32 vector_dimension = 0;
};

api::EngineDescriptor Descriptor(std::string canonical_type_name,
                                 std::string encoded_descriptor = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = std::move(encoded_descriptor);
  return descriptor;
}

bool RequireAcceptedFamilies() {
  bool ok = true;
  const std::vector<AdvancedFamilyCase> cases = {
      {dt::CanonicalTypeId::geometry,
       dt::AdvancedDatatypeOperationKind::intersects,
       dt::AdvancedDatatypeIndexKind::rtree,
       "format=wkb;srid=4326",
       "spatial",
       "family=spatial;type=geometry;format=wkb;srid=4326"},
      {dt::CanonicalTypeId::dense_vector,
       dt::AdvancedDatatypeOperationKind::nearest_neighbor,
       dt::AdvancedDatatypeIndexKind::hnsw,
       "dimension=128;element_type=real32",
       "vector",
       "family=vector;type=dense_vector;dimension=128;element_type=real32",
       128},
      {dt::CanonicalTypeId::search_query,
       dt::AdvancedDatatypeOperationKind::search_match,
       dt::AdvancedDatatypeIndexKind::inverted,
       "language=en;tokenizer=unicode_v1",
       "search",
       "family=search;type=search_query;language=en;tokenizer=unicode_v1"},
      {dt::CanonicalTypeId::graph_path,
       dt::AdvancedDatatypeOperationKind::path_match,
       dt::AdvancedDatatypeIndexKind::adjacency,
       "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000430001",
       "graph",
       "family=graph;type=graph_path;direction=directed;schema_uuid=019f0000-0000-7000-8000-000000430001"},
      {dt::CanonicalTypeId::time_series_value,
       dt::AdvancedDatatypeOperationKind::aggregate_window,
       dt::AdvancedDatatypeIndexKind::time_partition,
       "timestamp_type=timestamp;value_type=real64",
       "time_series",
       "family=time_series;type=time_series_value;timestamp_type=timestamp;value_type=real64"},
      {dt::CanonicalTypeId::hll_sketch,
       dt::AdvancedDatatypeOperationKind::estimate,
       dt::AdvancedDatatypeIndexKind::sketch_summary,
       "algorithm=hll;precision=14",
       "sketch",
       "family=sketch;type=hll_sketch;algorithm=hll;precision=14"},
      {dt::CanonicalTypeId::aggregate_state,
       dt::AdvancedDatatypeOperationKind::merge,
       dt::AdvancedDatatypeIndexKind::aggregate_state,
       "aggregate_function_uuid=019f0000-0000-7000-8000-000000430002;state_version=1",
       "aggregate_state",
       "family=aggregate_state;type=aggregate_state;aggregate_function_uuid=019f0000-0000-7000-8000-000000430002;state_version=1"},
      {dt::CanonicalTypeId::remote_object_locator,
       dt::AdvancedDatatypeOperationKind::resolve_locator,
       dt::AdvancedDatatypeIndexKind::locator_exact,
       "scope=remote;target_kind=object",
       "locator",
       "family=locator;type=remote_object_locator;scope=remote;target_kind=object"},
  };

  for (const auto& item : cases) {
    dt::AdvancedDatatypeFamilyRequest request;
    request.type_id = item.type_id;
    request.operation = item.operation;
    request.index_kind = item.index_kind;
    request.descriptor_profile = item.descriptor_profile;
    request.vector_dimension = item.vector_dimension;

    const auto result = dt::EvaluateAdvancedDatatypeFamily(request);
    if (!result.ok()) {
      std::cerr << item.expected_family << ':' << result.diagnostic_detail << '\n';
    }
    ok = Expect(result.ok(), "advanced datatype family was rejected") && ok;
    ok = Expect(dt::AdvancedDatatypeFamilyName(result.family) == item.expected_family,
                "advanced datatype family classification drifted") &&
         ok;
    ok = Expect(result.descriptor_supported && result.operation_supported &&
                    result.index_supported && result.optimizer_admitted,
                "advanced datatype admission flags drifted") &&
         ok;
    ok = Expect(result.canonical_descriptor_profile == item.canonical_fragment,
                "advanced datatype canonical descriptor profile drifted") &&
         ok;
    ok = Expect(result.required_descriptor_fields.size() == 2,
                "advanced datatype required descriptor fields were not exposed") &&
         ok;
    ok = Expect(!result.compare_supported && !result.hash_supported,
                "advanced datatype compare/hash support overclaimed") &&
         ok;
    ok = Expect(!result.compare_hash_refusal_detail.empty(),
                "advanced datatype compare/hash refusal was not explicit") &&
         ok;
    ok = Expect(Contains(result.optimizer_support_path,
                         dt::AdvancedDatatypeIndexKindName(item.index_kind)),
                "advanced datatype optimizer support path did not name the index") &&
         ok;
    ok = Expect(EnvelopeRoundTrips(request, result),
                "advanced datatype descriptor envelope did not round trip") &&
         ok;
  }
  return ok;
}

bool RequireFailures() {
  bool ok = true;

  dt::AdvancedDatatypeFamilyRequest missing_profile;
  missing_profile.type_id = dt::CanonicalTypeId::dense_vector;
  missing_profile.operation = dt::AdvancedDatatypeOperationKind::nearest_neighbor;
  missing_profile.index_kind = dt::AdvancedDatatypeIndexKind::hnsw;
  auto rejected = dt::EvaluateAdvancedDatatypeFamily(missing_profile);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail == "descriptor_profile_required:vector",
              "advanced family accepted a missing descriptor profile") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest unknown_field = missing_profile;
  unknown_field.descriptor_profile = "dimension=128;element_type=real32;extra=1";
  rejected = dt::EvaluateAdvancedDatatypeFamily(unknown_field);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail == "descriptor_profile_unknown_field:extra",
              "advanced family accepted an unknown descriptor field") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest mismatched_vector = missing_profile;
  mismatched_vector.descriptor_profile = "dimension=128;element_type=real32";
  mismatched_vector.vector_dimension = 64;
  rejected = dt::EvaluateAdvancedDatatypeFamily(mismatched_vector);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail == "vector_dimension_mismatch",
              "advanced family accepted a mismatched vector dimension") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest compare = mismatched_vector;
  compare.vector_dimension = 128;
  compare.operation = dt::AdvancedDatatypeOperationKind::compare;
  compare.index_kind = dt::AdvancedDatatypeIndexKind::none;
  rejected = dt::EvaluateAdvancedDatatypeFamily(compare);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail ==
                      "advanced_compare_refused:family_specific_operator_required" &&
                  !rejected.canonical_descriptor_profile.empty(),
              "advanced family compare operation did not fail closed") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest hash = compare;
  hash.operation = dt::AdvancedDatatypeOperationKind::hash;
  rejected = dt::EvaluateAdvancedDatatypeFamily(hash);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail ==
                      "advanced_hash_refused:family_specific_operator_required" &&
                  !rejected.canonical_descriptor_profile.empty(),
              "advanced family hash operation did not fail closed") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest bad_index = mismatched_vector;
  bad_index.vector_dimension = 128;
  bad_index.index_kind = dt::AdvancedDatatypeIndexKind::btree;
  rejected = dt::EvaluateAdvancedDatatypeFamily(bad_index);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail == "index_not_supported_for_family:btree",
              "advanced family accepted an invalid index kind") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest bad_sketch;
  bad_sketch.type_id = dt::CanonicalTypeId::hll_sketch;
  bad_sketch.operation = dt::AdvancedDatatypeOperationKind::estimate;
  bad_sketch.index_kind = dt::AdvancedDatatypeIndexKind::sketch_summary;
  bad_sketch.descriptor_profile = "algorithm=bloom;precision=14";
  rejected = dt::EvaluateAdvancedDatatypeFamily(bad_sketch);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail == "sketch_algorithm_mismatch:bloom",
              "advanced family accepted a mismatched sketch algorithm") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest bad_locator;
  bad_locator.type_id = dt::CanonicalTypeId::remote_object_locator;
  bad_locator.operation = dt::AdvancedDatatypeOperationKind::resolve_locator;
  bad_locator.index_kind = dt::AdvancedDatatypeIndexKind::locator_exact;
  bad_locator.descriptor_profile = "scope=local;target_kind=object";
  rejected = dt::EvaluateAdvancedDatatypeFamily(bad_locator);
  ok = Expect(!rejected.ok() &&
                  rejected.diagnostic_detail == "locator_scope_mismatch:local",
              "advanced family accepted a mismatched locator scope") &&
       ok;

  return ok;
}

bool RequireEngineApiAndOptimizerAdmission() {
  bool ok = true;

  api::EngineEvaluateAdvancedDatatypeFamilyRequest api_request;
  api_request.descriptor = Descriptor("hll_sketch");
  api_request.operation_kind = "sketch.estimate";
  api_request.index_kind = "sketch-summary";
  api_request.descriptor_profile = "algorithm=hll;precision=14";
  const auto api_result = api::EngineEvaluateAdvancedDatatypeFamily(api_request);
  ok = Expect(api_result.ok, "engine advanced datatype API rejected sketch aliases") && ok;
  ok = Expect(api_result.family == "sketch", "engine advanced datatype API family drifted") && ok;
  ok = Expect(api_result.optimizer_admitted &&
                  api_result.canonical_descriptor_profile ==
                      "family=sketch;type=hll_sketch;algorithm=hll;precision=14",
              "engine advanced datatype API lost canonical descriptor evidence") &&
       ok;
  ok = Expect(!api_result.compare_supported && !api_result.hash_supported,
              "engine advanced datatype API overclaimed compare/hash support") &&
       ok;

  api::EngineDatatypeIndexOptimizerAdmissionRequest admission;
  admission.type_group = "advanced";
  admission.descriptor = Descriptor("dense_vector",
                                    "family=vector;type=dense_vector;dimension=128;element_type=real32");
  admission.support_path = "advanced_family:descriptor_schema_canonicalized:hnsw";
  admission.index_stats_status = "validated";
  admission.donor_label = "pgvector";
  const auto admitted = api::EvaluateDatatypeIndexOptimizerAdmission(admission);
  ok = Expect(admitted.ok && admitted.index_admitted && admitted.statistics_admitted,
              "advanced datatype optimizer admission did not use canonical descriptor") &&
       ok;
  ok = Expect(admitted.optimizer_uses_canonical_descriptor &&
                  admitted.canonical_descriptor_used == "dense_vector",
              "advanced datatype optimizer admission used donor label authority") &&
       ok;

  admission.index_stats_status = "blocked";
  const auto blocked = api::EvaluateDatatypeIndexOptimizerAdmission(admission);
  ok = Expect(blocked.ok && !blocked.index_admitted && !blocked.statistics_admitted &&
                  blocked.diagnostic_detail == "index_statistics_blocked_by_required_subsystem",
              "advanced datatype blocked optimizer admission did not fail closed") &&
       ok;

  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = RequireAcceptedFamilies() && ok;
  ok = RequireFailures() && ok;
  ok = RequireEngineApiAndOptimizerAdmission() && ok;
  if (!ok) {
    return 1;
  }
  std::cout << "public_advanced_datatype_family_gate=passed\n";
  return 0;
}
