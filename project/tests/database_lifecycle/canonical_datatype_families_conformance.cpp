// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint16_t seed) {
  engine::Uuid uuid;
  uuid.bytes[0] = static_cast<std::uint8_t>(seed & 0xffu);
  uuid.bytes[1] = static_cast<std::uint8_t>((seed >> 8) & 0xffu);
  for (std::size_t index = 2; index < 16; ++index) {
    uuid.bytes[index] =
        static_cast<std::uint8_t>((seed + (index * 61u)) & 0xffu);
  }
  if (engine::ExecutionDataPacketUuidIsNil(uuid)) {
    uuid.bytes[0] = 1;
  }
  return uuid;
}

std::vector<engine::CanonicalDatatypeFamilyGroup> FamilyGroups() {
  using Group = engine::CanonicalDatatypeFamilyGroup;
  return {Group::text_binary_bit,
          Group::temporal_interval,
          Group::identity_network_lob,
          Group::structured_container_range,
          Group::document_search_spatial_vector,
          Group::graph_timeseries_columnar,
          Group::aggregate_sketch_locator_opaque};
}

std::vector<std::pair<engine::CanonicalDatatypeFamilyGroup, std::string>>
RequiredFamilies() {
  using Group = engine::CanonicalDatatypeFamilyGroup;
  return {{Group::text_binary_bit, "text"},
          {Group::text_binary_bit, "binary_string"},
          {Group::text_binary_bit, "bit_string"},
          {Group::temporal_interval, "date"},
          {Group::temporal_interval, "time"},
          {Group::temporal_interval, "time_with_zone"},
          {Group::temporal_interval, "timestamp"},
          {Group::temporal_interval, "timestamp_with_zone"},
          {Group::temporal_interval, "instant"},
          {Group::temporal_interval, "year_domain"},
          {Group::temporal_interval, "year_month_interval"},
          {Group::temporal_interval, "day_time_interval"},
          {Group::temporal_interval, "mixed_interval"},
          {Group::temporal_interval, "fixed_duration"},
          {Group::temporal_interval, "reference_duration_domain"},
          {Group::identity_network_lob, "uuid"},
          {Group::identity_network_lob, "guid_domain"},
          {Group::identity_network_lob, "object_id_domain"},
          {Group::identity_network_lob, "timeuuid_domain"},
          {Group::identity_network_lob, "system_object_id"},
          {Group::identity_network_lob, "ip_address"},
          {Group::identity_network_lob, "ip_network"},
          {Group::identity_network_lob, "ip_range"},
          {Group::identity_network_lob, "mac_address"},
          {Group::identity_network_lob, "lob_oversized_value"},
          {Group::identity_network_lob, "external_value_handle"},
          {Group::identity_network_lob, "stream_descriptor"},
          {Group::structured_container_range, "enum"},
          {Group::structured_container_range, "set"},
          {Group::structured_container_range, "array"},
          {Group::structured_container_range, "list"},
          {Group::structured_container_range, "map"},
          {Group::structured_container_range, "row"},
          {Group::structured_container_range, "composite"},
          {Group::structured_container_range, "variant"},
          {Group::structured_container_range, "range"},
          {Group::structured_container_range, "multirange"},
          {Group::structured_container_range, "object_compatible_domain"},
          {Group::document_search_spatial_vector, "document"},
          {Group::document_search_spatial_vector, "token_stream"},
          {Group::document_search_spatial_vector, "search_query"},
          {Group::document_search_spatial_vector, "search_rank_feature"},
          {Group::document_search_spatial_vector, "search_completion"},
          {Group::document_search_spatial_vector, "search_percolator"},
          {Group::document_search_spatial_vector, "geometry"},
          {Group::document_search_spatial_vector, "geography"},
          {Group::document_search_spatial_vector, "point"},
          {Group::document_search_spatial_vector, "shape"},
          {Group::document_search_spatial_vector, "vector_similarity"},
          {Group::graph_timeseries_columnar, "graph_node"},
          {Group::graph_timeseries_columnar, "graph_edge"},
          {Group::graph_timeseries_columnar, "graph_path"},
          {Group::graph_timeseries_columnar, "graph_label"},
          {Group::graph_timeseries_columnar, "graph_property_map"},
          {Group::graph_timeseries_columnar, "measurement"},
          {Group::graph_timeseries_columnar, "tag_set"},
          {Group::graph_timeseries_columnar, "field_value"},
          {Group::graph_timeseries_columnar, "series_key"},
          {Group::graph_timeseries_columnar, "time_bucket"},
          {Group::graph_timeseries_columnar, "rollup_state"},
          {Group::graph_timeseries_columnar, "nullable_wrapper"},
          {Group::graph_timeseries_columnar, "dictionary_encoded"},
          {Group::graph_timeseries_columnar, "low_cardinality"},
          {Group::graph_timeseries_columnar, "nested_column"},
          {Group::graph_timeseries_columnar, "column_segment_value"},
          {Group::graph_timeseries_columnar, "compressed_column_value"},
          {Group::graph_timeseries_columnar, "vectorized_batch_value"},
          {Group::aggregate_sketch_locator_opaque, "aggregate_state"},
          {Group::aggregate_sketch_locator_opaque, "hyperloglog_sketch"},
          {Group::aggregate_sketch_locator_opaque, "bloom_filter"},
          {Group::aggregate_sketch_locator_opaque, "quantile_sketch"},
          {Group::aggregate_sketch_locator_opaque, "histogram_sketch"},
          {Group::aggregate_sketch_locator_opaque, "ranking_summary"},
          {Group::aggregate_sketch_locator_opaque, "vector_search_summary"},
          {Group::aggregate_sketch_locator_opaque, "lob_locator"},
          {Group::aggregate_sketch_locator_opaque, "external_file_locator"},
          {Group::aggregate_sketch_locator_opaque, "remote_object_locator"},
          {Group::aggregate_sketch_locator_opaque, "bridge_handle"},
          {Group::aggregate_sketch_locator_opaque, "cursor_table_handle"},
          {Group::aggregate_sketch_locator_opaque, "system_pseudotype"},
          {Group::aggregate_sketch_locator_opaque, "opaque_extension"}};
}

std::vector<std::string> RequiredDiagnostics() {
  return {"CTB.TEXT.DESCRIPTOR_INVALID",
          "CTB.TEXT.INVALID_ENCODING",
          "CTB.TEXT.COLLATION_RESOURCE_MISSING",
          "CTB.TEXT.LENGTH_EXCEEDED",
          "CTB.TEXT_BINARY.CAST_ENCODING_MISSING",
          "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
          "CTB.BINARY.DESCRIPTOR_INVALID",
          "CTB.BINARY.LENGTH_EXCEEDED",
          "CTB.BINARY.ORDERING_REFUSED",
          "CTB.BIT.DESCRIPTOR_INVALID",
          "CTB.BIT.LENGTH_EXCEEDED",
          "CTB.BIT.PADDING_REFUSED",
          "CTB.BIT.OPERATION_INCOMPATIBLE",
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "CTI.INTERVAL.DESCRIPTOR_INVALID",
          "CTI.TEMPORAL.INVALID_LITERAL",
          "CTI.TEMPORAL.PRECISION_LOSS",
          "CTI.TEMPORAL.TIMEZONE_RESOURCE_MISSING",
          "CTI.TEMPORAL.AMBIGUOUS_LOCAL_TIME",
          "CTI.TEMPORAL.NONEXISTENT_LOCAL_TIME",
          "CTI.TEMPORAL.ZERO_DATE_REFUSED",
          "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
          "CTI.INTERVAL.SUBTYPE_MISMATCH",
          "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
          "CINL.IDENTITY.DESCRIPTOR_INVALID",
          "CINL.IDENTITY.INVALID_LITERAL",
          "CINL.IDENTITY.REFERENCE_ORDERING_MISMATCH",
          "CINL.IDENTITY.GENERATION_FENCED",
          "CINL.NETWORK.DESCRIPTOR_INVALID",
          "CINL.NETWORK.INVALID_ADDRESS",
          "CINL.NETWORK.PREFIX_INVALID",
          "CINL.LOB.DESCRIPTOR_INVALID",
          "CINL.LOB.HANDLE_EXPIRED",
          "CINL.LOB.CHUNK_MISSING",
          "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED",
          "CINL.LOCATOR.DEREFERENCE_REFUSED",
          "CINL.LOCATOR.EXPIRED",
          "CINL.LOCATOR.REVOKED",
          "CSCR.DESCRIPTOR.INVALID",
          "CSCR.ENUM.LABEL_UNKNOWN",
          "CSCR.SET.DUPLICATE_REFUSED",
          "CSCR.ARRAY.BOUNDS_INVALID",
          "CSCR.MAP.KEY_DUPLICATE_REFUSED",
          "CSCR.COMPOUND.FIELD_NOT_VISIBLE",
          "CSCR.DOMAIN_ELEMENT_PATH.STALE",
          "CSCR.VARIANT.TAG_UNKNOWN",
          "CSCR.RANGE.BOUND_INVALID",
          "CSCR.MULTIRANGE.NOT_CANONICAL",
          "CDSSV.DOCUMENT.DESCRIPTOR_INVALID",
          "CDSSV.DOCUMENT.INVALID_PAYLOAD",
          "CDSSV.DOCUMENT.PATH_MISSING",
          "CDSSV.DOCUMENT.DUPLICATE_KEY_REFUSED",
          "CDSSV.SEARCH.DESCRIPTOR_INVALID",
          "CDSSV.SEARCH.ANALYZER_MISSING",
          "CDSSV.SEARCH.QUERY_INVALID",
          "CDSSV.SPATIAL.DESCRIPTOR_INVALID",
          "CDSSV.SPATIAL.SRID_MISMATCH",
          "CDSSV.SPATIAL.INVALID_GEOMETRY",
          "CDSSV.VECTOR.DESCRIPTOR_INVALID",
          "CDSSV.VECTOR.DIMENSION_MISMATCH",
          "CDSSV.VECTOR.METRIC_MISMATCH",
          "CDSSV.VECTOR.QUANTIZATION_REFUSED",
          "CGTC.GRAPH.DESCRIPTOR_INVALID",
          "CGTC.GRAPH.LABEL_UNKNOWN",
          "CGTC.GRAPH.PATH_INVALID",
          "CGTC.GRAPH.PROPERTY_INVALID",
          "CGTC.TIMESERIES.DESCRIPTOR_INVALID",
          "CGTC.TIMESERIES.TIMESTAMP_PRECISION_MISMATCH",
          "CGTC.TIMESERIES.TAG_POLICY_VIOLATION",
          "CGTC.TIMESERIES.RETENTION_REFUSED",
          "CGTC.COLUMNAR.DESCRIPTOR_INVALID",
          "CGTC.COLUMNAR.DICTIONARY_MISSING",
          "CGTC.COLUMNAR.SEGMENT_CODEC_MISSING",
          "CGTC.COLUMNAR.WRAPPER_INCOMPATIBLE",
          "CASLO.AGGREGATE.DESCRIPTOR_INVALID",
          "CASLO.AGGREGATE.VERSION_MISMATCH",
          "CASLO.AGGREGATE.NOT_EXPOSED",
          "CASLO.SKETCH.DESCRIPTOR_INVALID",
          "CASLO.SKETCH.PARAMETER_MISMATCH",
          "CASLO.SKETCH.MERGE_REFUSED",
          "CASLO.LOCATOR.DESCRIPTOR_INVALID",
          "CASLO.LOCATOR.EXPIRED",
          "CASLO.LOCATOR.REVOKED",
          "CASLO.LOCATOR.DEREFERENCE_DENIED",
          "CASLO.PSEUDOTYPE.NOT_VISIBLE",
          "CASLO.OPAQUE.DESCRIPTOR_INVALID",
          "CASLO.OPAQUE.UDR_PACKAGE_MISSING",
          "CASLO.OPAQUE.CODEC_MISMATCH",
          "CASLO.OPAQUE.RAW_PAYLOAD_REFUSED",
          "CASLO.UDR.RUNTIME_FORBIDDEN"};
}

std::vector<std::string> RequiredMetrics() {
  return {
      "sys.metrics.datatypes.text.descriptor.admissions_total",
      "sys.metrics.datatypes.binary.length_refusals_total",
      "sys.metrics.datatypes.bit.operation.refusals_total",
      "sys.metrics.datatypes.temporal.invalid_literals_total",
      "sys.metrics.datatypes.interval.subtype_mismatches_total",
      "sys.metrics.datatypes.identity.generation_attempts_total",
      "sys.metrics.datatypes.network.invalid_addresses_total",
      "sys.metrics.datatypes.lob.handle_expirations_total",
      "sys.metrics.datatypes.external_locator.dereference_refusals_total",
      "sys.metrics.datatypes.structured.descriptor_admissions_total",
      "sys.metrics.datatypes.enum.unknown_labels_total",
      "sys.metrics.datatypes.container.invalid_bounds_total",
      "sys.metrics.datatypes.variant.unknown_tags_total",
      "sys.metrics.datatypes.range.invalid_bounds_total",
      "sys.metrics.datatypes.document.invalid_payloads_total",
      "sys.metrics.datatypes.search.analyzer_missing_total",
      "sys.metrics.datatypes.spatial.srid_mismatches_total",
      "sys.metrics.datatypes.vector.dimension_mismatches_total",
      "sys.metrics.datatypes.graph.label_misses_total",
      "sys.metrics.datatypes.timeseries.precision_mismatches_total",
      "sys.metrics.datatypes.columnar.dictionary_misses_total",
      "sys.metrics.datatypes.aggregate_state.version_mismatches_total",
      "sys.metrics.datatypes.sketch.parameter_mismatches_total",
      "sys.metrics.datatypes.locator.dereference_denials_total",
      "sys.metrics.datatypes.pseudotype.visibility_denials_total",
      "sys.metrics.datatypes.opaque.udr_package_misses_total",
      "sys.metrics.datatypes.family.transport_refusals_total",
      "sys.metrics.datatypes.family.reference_mapping_misses_total",
      "sys.metrics.datatypes.family.merge_manual_review_total"};
}

std::vector<std::string> RequiredGates() {
  return {"CTB-GATE-001",   "CTB-GATE-002",   "CTB-GATE-003",
          "CTB-GATE-004",   "CTI-GATE-001",   "CTI-GATE-002",
          "CTI-GATE-003",   "CTI-GATE-004",   "CINL-GATE-001",
          "CINL-GATE-002",  "CINL-GATE-003",  "CINL-GATE-004",
          "CSCR-GATE-001",  "CSCR-GATE-002",  "CSCR-GATE-003",
          "CSCR-GATE-004",  "CDSSV-GATE-001", "CDSSV-GATE-002",
          "CDSSV-GATE-003", "CDSSV-GATE-004", "CGTC-GATE-001",
          "CGTC-GATE-002",  "CGTC-GATE-003",  "CGTC-GATE-004",
          "CASLO-GATE-001", "CASLO-GATE-002", "CASLO-GATE-003",
          "CASLO-GATE-004", "CTB-CONF-001",   "CTB-CONF-002",
          "CTB-CONF-003",   "CTB-CONF-004",   "CTB-CONF-005",
          "CTB-CONF-006",   "CTB-CONF-007",   "CTB-CONF-008",
          "CTB-CONF-009",   "CTI-CONF-001",   "CTI-CONF-002",
          "CTI-CONF-003",   "CTI-CONF-004",   "CTI-CONF-005",
          "CTI-CONF-006",   "CTI-CONF-007",   "CTI-CONF-008",
          "CTI-CONF-009",   "CTI-CONF-010",   "CTI-CONF-011",
          "CINL-CONF-001",  "CINL-CONF-002",  "CINL-CONF-003",
          "CINL-CONF-004",  "CINL-CONF-005",  "CINL-CONF-006",
          "CINL-CONF-007",  "CINL-CONF-008",  "CSCR-CONF-001",
          "CSCR-CONF-002",  "CSCR-CONF-003",  "CSCR-CONF-004",
          "CSCR-CONF-005",  "CSCR-CONF-006",  "CSCR-CONF-007",
          "CSCR-CONF-008",  "CSCR-CONF-009",  "CSCR-CONF-010",
          "CSCR-CONF-011",  "CSCR-CONF-012",  "CSCR-CONF-013",
          "CDSSV-CONF-001", "CDSSV-CONF-002", "CDSSV-CONF-003",
          "CDSSV-CONF-004", "CDSSV-CONF-005", "CDSSV-CONF-006",
          "CDSSV-CONF-007", "CDSSV-CONF-008", "CDSSV-CONF-009",
          "CDSSV-CONF-010", "CDSSV-CONF-011", "CGTC-CONF-001",
          "CGTC-CONF-002",  "CGTC-CONF-003",  "CGTC-CONF-004",
          "CGTC-CONF-005",  "CGTC-CONF-006",  "CGTC-CONF-007",
          "CGTC-CONF-008",  "CGTC-CONF-009",  "CGTC-CONF-010",
          "CASLO-CONF-001", "CASLO-CONF-002", "CASLO-CONF-003",
          "CASLO-CONF-004", "CASLO-CONF-005", "CASLO-CONF-006",
          "CASLO-CONF-007", "CASLO-CONF-008", "CASLO-CONF-009",
          "CASLO-CONF-010", "CASLO-CONF-011"};
}

engine::CanonicalDatatypeFamilyGroup GroupForGate(std::string_view gate_id) {
  using Group = engine::CanonicalDatatypeFamilyGroup;
  if (gate_id.rfind("CTB-", 0) == 0) {
    return Group::text_binary_bit;
  }
  if (gate_id.rfind("CTI-", 0) == 0) {
    return Group::temporal_interval;
  }
  if (gate_id.rfind("CINL-", 0) == 0) {
    return Group::identity_network_lob;
  }
  if (gate_id.rfind("CSCR-", 0) == 0) {
    return Group::structured_container_range;
  }
  if (gate_id.rfind("CDSSV-", 0) == 0) {
    return Group::document_search_spatial_vector;
  }
  if (gate_id.rfind("CGTC-", 0) == 0) {
    return Group::graph_timeseries_columnar;
  }
  return Group::aggregate_sketch_locator_opaque;
}

void AddOperationContract(
    engine::CanonicalDatatypeFamilyRegistry& registry,
    engine::CanonicalDatatypeFamilyGroup group,
    std::uint16_t& uuid_seed) {
  engine::CanonicalDatatypeFamilyOperationContractRecord contract;
  contract.operation_contract_uuid = Uuid(uuid_seed++);
  contract.family_group = group;
  contract.operation_contract_key =
      std::string(engine::CanonicalDatatypeFamilyGroupSearchKey(group)) +
      ".operations";
  contract.operation_contract_hash = contract.operation_contract_key + ".hash";
  registry.operation_contracts.push_back(contract);
}

void AddTransportProfile(engine::CanonicalDatatypeFamilyRegistry& registry,
                         engine::CanonicalDatatypeFamilyGroup group,
                         std::uint16_t& uuid_seed) {
  engine::CanonicalDatatypeFamilyTransportRecord profile;
  profile.transport_profile_uuid = Uuid(uuid_seed++);
  profile.family_group = group;
  profile.transport_profile_key =
      std::string(engine::CanonicalDatatypeFamilyGroupSearchKey(group)) +
      ".transport";
  profile.transport_profile_hash = profile.transport_profile_key + ".hash";
  registry.transport_profiles.push_back(profile);
}

void AddDescriptor(engine::CanonicalDatatypeFamilyRegistry& registry,
                   engine::CanonicalDatatypeFamilyGroup group,
                   const std::string& family_name,
                   std::uint16_t& uuid_seed) {
  engine::CanonicalDatatypeFamilyDescriptorRecord descriptor;
  descriptor.descriptor_uuid = Uuid(uuid_seed++);
  descriptor.family_group = group;
  descriptor.family_name = family_name;
  descriptor.descriptor_record_name = family_name + "_descriptor";
  descriptor.controlling_search_key =
      std::string(engine::CanonicalDatatypeFamilyGroupSearchKey(group));
  descriptor.descriptor_hash = "descriptor." + family_name;
  registry.descriptors.push_back(descriptor);
}

void AddGate(engine::CanonicalDatatypeFamilyRegistry& registry,
             const std::string& gate_id,
             std::uint16_t& uuid_seed) {
  engine::CanonicalDatatypeFamilyConformanceGateRecord gate;
  gate.gate_uuid = Uuid(uuid_seed++);
  gate.family_group = GroupForGate(gate_id);
  gate.gate_id = gate_id;
  gate.evidence_hash = "evidence." + gate_id;
  gate.ctest_name = "database_lifecycle_canonical_datatype_families_"
                    "conformance";
  gate.gate_hash = "gate." + gate_id;
  registry.conformance_gates.push_back(gate);
}

engine::CanonicalDatatypeFamilyRegistry ValidRegistry() {
  engine::CanonicalDatatypeFamilyRegistry registry;
  std::uint16_t uuid_seed = 0x7d00;
  registry.registry_uuid = Uuid(uuid_seed++);
  registry.registry_epoch = 1;
  registry.registry_name = "canonical datatype family implementation registry";
  registry.root_search_key = "CDT-CANONICAL-DATATYPE-FAMILY-IMPLEMENTATION";

  for (const auto group : FamilyGroups()) {
    AddOperationContract(registry, group, uuid_seed);
    AddTransportProfile(registry, group, uuid_seed);
  }
  for (const auto& family : RequiredFamilies()) {
    AddDescriptor(registry, family.first, family.second, uuid_seed);
  }
  for (const auto& gate : RequiredGates()) {
    AddGate(registry, gate, uuid_seed);
  }
  registry.diagnostic_codes = RequiredDiagnostics();
  registry.local_metric_names = RequiredMetrics();
  return registry;
}

void ExpectStatus(const engine::CanonicalDatatypeFamilyRegistry& registry,
                  engine::CanonicalDatatypeFamilyStatus expected,
                  std::string_view scenario) {
  const auto result = engine::ValidateCanonicalDatatypeFamilyRegistry(registry);
  if (result.status != expected) {
    std::cerr << scenario << ": expected "
              << engine::CanonicalDatatypeFamilyStatusName(expected)
              << " but got "
              << engine::CanonicalDatatypeFamilyStatusName(result.status)
              << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void TestValidRegistry() {
  const auto registry = ValidRegistry();
  Require(engine::ValidateCanonicalDatatypeFamilyRegistry(registry).ok(),
          "valid canonical datatype family registry must pass");
  Require(engine::CanonicalDatatypeFamilyStatusName(
              engine::CanonicalDatatypeFamilyStatus::ok) == "ok",
          "status names must be stable");
}

void TestDescriptorFailures() {
  auto registry = ValidRegistry();
  registry.descriptors[0].resource_epoch_bound = false;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   descriptor_contract_incomplete,
               "descriptor resource epoch must be required");

  registry = ValidRegistry();
  registry.descriptors.back().family_name = "opaque_extension_missing";
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   descriptor_record_missing,
               "required family descriptor must be present");

  registry = ValidRegistry();
  registry.descriptors[1].descriptor_uuid =
      registry.descriptors[0].descriptor_uuid;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::descriptor_duplicate,
               "duplicate descriptor UUIDs must fail closed");
}

void TestAuthorityAndContractFailures() {
  auto registry = ValidRegistry();
  registry.parser_sblr_boundary_preserved = false;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   authority_invariant_violation,
               "parser authority drift must fail closed");

  registry = ValidRegistry();
  registry.operation_contracts[0].descriptor_bound = false;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   operation_contract_incomplete,
               "operation contracts must be descriptor-bound");

  registry = ValidRegistry();
  registry.transport_profiles[0].write_after_delta_not_recovery_authority =
      false;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   transport_recovery_authority_violation,
               "write-after delta must not become recovery authority");
}

void TestGateDiagnosticsAndMetricsFailures() {
  auto registry = ValidRegistry();
  registry.conformance_gates.pop_back();
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   conformance_gate_missing,
               "all family conformance gates must be present");

  registry = ValidRegistry();
  registry.conformance_gates[0].status =
      engine::CanonicalDatatypeFamilyGateStatus::failed;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::conformance_gate_failed,
               "failed family gate must block closure");

  registry = ValidRegistry();
  registry.diagnostic_codes.pop_back();
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   diagnostic_vector_missing,
               "diagnostic inventory must be complete");

  registry = ValidRegistry();
  registry.local_metric_names.pop_back();
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::local_metric_missing,
               "metric inventory must be complete");

  registry = ValidRegistry();
  registry.cluster_metrics_guarded_by_cluster_governance = false;
  ExpectStatus(registry,
               engine::CanonicalDatatypeFamilyStatus::
                   cluster_metric_guard_required,
               "cluster metrics must be cluster-governed");
}

}  // namespace

int main() {
  TestValidRegistry();
  TestDescriptorFailures();
  TestAuthorityAndContractFailures();
  TestGateDiagnosticsAndMetricsFailures();
  return EXIT_SUCCESS;
}
