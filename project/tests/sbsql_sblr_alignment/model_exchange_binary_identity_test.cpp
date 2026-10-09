// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual exchange implementation; fixture receipts prove component admission
// and publication only, never real provider/MGA authority or SQL execution.
#include "../../src/engine/executor/model_family_exchange.hpp"
#include "../../src/engine/optimizer/model_family_coordinator.hpp"

#include <iostream>
#include <array>
#include <limits>
#include <stdexcept>

namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace opt = scratchbird::engine::optimizer;

namespace {
void Require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
bool RejectedWithoutPublication(const ex::ModelExchangeResultV1& result) {
  return !result.accepted && !result.root_publishable &&
         !result.output.exact_exchange_validated && result.output.batch.rows.empty() &&
         result.output.ordered_row_identities.empty();
}
api::EngineUuid FixtureUuid(std::uint8_t ordinal) {
  return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xaa,ordinal}};
}
}

int main() {
  try {
    ex::ModelSourceInputDescriptorV1 input;
    input.family_id = "key_value";
    input.operation_id = "KEY_VALUE_GET";
    input.physical_node_id = 1;
    input.causal_counter_id = 1;
    input.object_uuid = FixtureUuid(1);
    input.selected_alternative_uuid = FixtureUuid(2);
    input.capability_uuid = FixtureUuid(3);
    input.provider_uuid = FixtureUuid(4);
    input.result_handle_uuid = FixtureUuid(5);
    input.catalog_epoch_uuid = FixtureUuid(6);
    input.security_context_uuid = FixtureUuid(7);
    input.policy_snapshot_uuid = FixtureUuid(8);
    input.resource_contract_uuid = FixtureUuid(9);
    input.provider_generation = input.catalog_generation = input.descriptor_generation = 1;
    input.security_generation = input.policy_generation = input.resource_generation = 1;
    input.output_descriptor_ids = {1,2,3};
    input.maximum_rows = 1;
    input.maximum_cells = 3;
    input.maximum_memory_bytes = 1048576;
    auto& mga = input.mga_statement_context;
    mga.statement_uuid = FixtureUuid(10);
    mga.owning_transaction_uuid = FixtureUuid(11);
    mga.statement_snapshot_uuid = FixtureUuid(12);
    mga.statement_metadata_snapshot_uuid = FixtureUuid(13);
    mga.statement_timestamp = "2026-09-12T00:00:00Z";
    mga.snapshot_kind = "statement_stable";
    mga.owning_local_transaction_id = 1;
    mga.publication_inventory_next_local_transaction_id = 2;
    mga.oldest_active_transaction_id = mga.oldest_interesting_transaction_id = 1;
    mga.oldest_snapshot_transaction_id = mga.retention_horizon_transaction_id = 1;
    mga.active_excluded_local_transaction_ids = {1};
    mga.inventory_authoritative = mga.complete = mga.current = true;
    Require(ex::ValidateModelFamilySourceInputV1(input).accepted,
            "binary model input fixture did not pass structural admission");

    opt::ModelFamilyCoordinatorRequestV1 planning;
    planning.family_id = input.family_id;
    planning.operation_id = input.operation_id;
    planning.logical_operator_id = "LOGICAL_KEY_VALUE_SOURCE_V1";
    planning.logical_node_id = 1;
    planning.object_uuid = input.object_uuid;
    planning.output_descriptor_ids = input.output_descriptor_ids;
    planning.mga_statement_context = mga;
    planning.bound_sblr_tree_uuid = FixtureUuid(40);
    planning.catalog_epoch_uuid = input.catalog_epoch_uuid;
    planning.security_context_uuid = input.security_context_uuid;
    planning.capability_snapshot_uuid = FixtureUuid(41);
    planning.resource_snapshot_uuid = input.resource_contract_uuid;
    planning.statistics_snapshot_uuid = FixtureUuid(42);
    planning.route_snapshot_uuid = FixtureUuid(43);
    planning.catalog_generation = planning.current_catalog_generation = 1;
    planning.security_epoch = planning.policy_epoch = planning.resource_epoch = 1;
    planning.statistics_generation = planning.route_epoch = planning.route_generation = 1;
    planning.memory_budget_bytes = input.maximum_memory_bytes;
    opt::ModelFamilyCandidateV1 candidate;
    candidate.alternative_uuid = input.selected_alternative_uuid;
    candidate.provider_uuid = input.provider_uuid;
    candidate.capability_uuid = input.capability_uuid;
    candidate.implementation_id = "physical_key_value_scan_v1";
    candidate.provider_generation = 1;
    candidate.available = candidate.exact = true;
    candidate.candidate_inventory_receipt_uuid = FixtureUuid(44);
    candidate.cost.cost_vector_uuid = FixtureUuid(45);
    candidate.cost.provenance_uuid = FixtureUuid(46);
    candidate.cost.property_snapshot_uuid = FixtureUuid(47);
    candidate.cost.calibration_profile_uuid = FixtureUuid(48);
    candidate.cost.scalarization_policy_id = "model-family.complete-unit-sum-minus-cache-benefit.v1";
    candidate.cost.provenance_generation = 1;
    candidate.cost.confidence_basis_points = 10000;
    candidate.cost.cpu_units = 5;
    candidate.cost.memory_grant_units = 512;
    candidate.cost.memory_allocation_units = 512;
    candidate.cost.memory_bytes_required = 64;
    candidate.cost.scalar_score = 517;
    candidate.cost.complete_dimension_vector = true;
    planning.candidates = {candidate};
    const auto planned = opt::CoordinateModelFamilySourceV1(planning);
    if (!planned.accepted) throw std::runtime_error(planned.detail);
    Require(planned.selected && planned.selected_candidate.alternative_uuid == candidate.alternative_uuid &&
            planned.selected_cost_explain == candidate.cost &&
            planned.physical_dag.nodes.size() == 1 &&
            planned.physical_dag.nodes[0].selected_alternative_uuid == candidate.alternative_uuid,
            "coordinator changed selected binary identity or typed cost explanation");
    const auto replanned = opt::CoordinateModelFamilySourceV1(planning);
    Require(replanned.accepted && replanned.selected_cost_explain == planned.selected_cost_explain &&
            replanned.selected_candidate.alternative_uuid == planned.selected_candidate.alternative_uuid &&
            replanned.physical_dag.selected_plan_uuid != planned.physical_dag.selected_plan_uuid,
            "fresh plan instances reused a content-derived UUID or changed deterministic selection");

    ex::ModelProviderBatchV1 provider;
    provider.provider_uuid = input.provider_uuid;
    provider.provider_generation = 1;
    provider.selected_alternative_uuid = input.selected_alternative_uuid;
    provider.capability_uuid = input.capability_uuid;
    provider.result_handle_uuid = input.result_handle_uuid;
    provider.causal_counter_id = 1;
    provider.output_descriptor_ids = input.output_descriptor_ids;
    provider.mga_statement_context = mga;
    provider.security_receipt_uuid = FixtureUuid(14);
    provider.residual_recheck_complete = provider.base_row_mga_recheck_complete = true;
    provider.security_recheck_complete = true;
    provider.properties.property_uuid = FixtureUuid(15);
    provider.properties.ordering_id = "key_value_unordered_v1";
    provider.properties.uniqueness_id = "key";
    provider.properties.residual_recheck_complete = true;
    provider.properties.base_row_mga_recheck_complete = true;
    provider.properties.security_recheck_complete = true;
    ex::ModelProviderRowIdentityV1 identity;
    identity.row_uuid = FixtureUuid(16);
    identity.key = "key";
    provider.ordered_row_identities = {identity};
    ex::DescriptorTuple row;
    for (unsigned index = 0; index < 3; ++index) {
      api::EngineDescriptor descriptor;
      descriptor.descriptor_uuid = FixtureUuid(20 + index);
      descriptor.type_uuid = FixtureUuid(index == 0 ? 30 : 31);
      descriptor.descriptor_kind = "scalar";
      descriptor.canonical_type_name = index == 0 ? "uuid" : "text";
      descriptor.encoded_descriptor = "nullability=non_null";
      provider.batch.columns.push_back({index == 0 ? "row_uuid" : index == 1 ? "key" : "value",
                                        descriptor, false, index + 1});
      api::EngineTypedValue value;
      value.descriptor = descriptor;
      value.state = api::EngineValueState::value;
      if (index == 0)
        value.binary_value.assign(identity.row_uuid.bytes.begin(), identity.row_uuid.bytes.end());
      else value.encoded_value = index == 1 ? "key" : "stored value";
      row.values.push_back(value);
    }
    provider.batch.rows.push_back(row);
    const auto publish = [&](const ex::ModelProviderBatchV1& batch) {
      return ex::PublishModelFamilyExchangeV1(input, batch, {});
    };
    const auto result = publish(provider);
    if (!result.accepted) throw std::runtime_error(result.detail);
    Require(result.root_publishable && result.output.exact_exchange_validated &&
            result.output.batch.rows.size() == 1 &&
            result.output.batch.rows[0].values[0].binary_value == row.values[0].binary_value &&
            result.output.batch.rows[0].values[2].encoded_value == "stored value",
            "model exchange did not publish the exact binary identity and value");
    for (unsigned byte = 0; byte < 16; ++byte) {
      auto changed = provider;
      changed.batch.rows[0].values[0].binary_value[byte] ^= 1;
      Require(RejectedWithoutPublication(publish(changed)), "model exchange published substituted UUID bytes");
    }
    for (unsigned size = 0; size < 33; ++size) {
      if (size == 16) continue;
      auto changed = provider;
      changed.batch.rows[0].values[0].binary_value.resize(size);
      Require(RejectedWithoutPublication(publish(changed)), "model exchange published wrong UUID payload width");
    }
    auto changed = provider;
    changed.batch.rows[0].values[0].binary_value.clear();
    changed.batch.rows[0].values[0].encoded_value = "019d0000-0000-7000-8000-00000000aa10";
    Require(RejectedWithoutPublication(publish(changed)), "model exchange published textual system identity");
    for (unsigned version = 0; version < 16; ++version) {
      changed = provider;
      changed.provider_uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
      Require(publish(changed).accepted == (version == 7),
              "model exchange accepted substituted provider identity");
    }
    Require(RejectedWithoutPublication(ex::PublishModelFamilyExchangeV1(input, provider, [] { return true; })),
            "cancelled exchange published a result");
    for (const auto state : {api::EngineValueState::sql_null, api::EngineValueState::missing}) {
      changed = provider;
      changed.batch.rows[0].values[0].state = state;
      Require(RejectedWithoutPublication(publish(changed)), "model exchange published a non-value identity");
    }
    changed = provider;
    changed.batch.rows[0].values[0].descriptor.type_uuid.bytes[15] ^= 1;
    Require(RejectedWithoutPublication(publish(changed)), "model exchange published a substituted datatype descriptor");
    auto limited = input;
    limited.maximum_memory_bytes = 1;
    Require(RejectedWithoutPublication(ex::PublishModelFamilyExchangeV1(limited, provider, {})),
            "model exchange published beyond its memory grant");
    auto search_input = input;
    search_input.family_id = "search";
    search_input.operation_id = "SEARCH_RANKED_QUERY";
    search_input.output_descriptor_ids = {1, 2, 3, 4, 5};
    search_input.maximum_cells = 5;
    auto search = provider;
    search.output_descriptor_ids = search_input.output_descriptor_ids;
    search.properties.ordering_id = "search_score_desc_document_uuid_asc_v1";
    search.properties.uniqueness_id = "document_uuid";
    auto& search_identity = search.ordered_row_identities.front();
    search_identity = {};
    search_identity.document_uuid = FixtureUuid(60);
    search_identity.search_analyzer_uuid = FixtureUuid(61);
    search_identity.search_analyzer_generation = 7;
    search_identity.search_score = 0.75;
    search_identity.search_rank = 1;
    search.batch = {};
    ex::DescriptorTuple search_row;
    const std::array names{"document_uuid", "analyzer_uuid", "analyzer_generation", "score", "rank"};
    const std::array types{"uuid", "uuid", "uint64", "real64", "uint64"};
    for (unsigned i = 0; i < 5; ++i) {
      auto descriptor = ex::MakeExecutorDescriptor(types[i], "nullability=non_null");
      descriptor.descriptor_uuid = FixtureUuid(62 + i);
      descriptor.descriptor_kind = "scalar";
      search.batch.columns.push_back({names[i], descriptor, false, i + 1});
      api::EngineTypedValue value;
      if (i < 2) {
        const auto uuid = i == 0 ? search_identity.document_uuid : search_identity.search_analyzer_uuid;
        value.binary_value.assign(uuid.bytes.begin(), uuid.bytes.end());
      } else if (i == 3) value = ex::EncodeReal64Value(0.75);
      else value = ex::EncodeUint64Value(i == 2 ? 7 : 1);
      value.descriptor = descriptor;
      search_row.values.push_back(std::move(value));
    }
    search.batch.rows.push_back(search_row);
    const auto publish_search = [&](const auto& batch) {
      return ex::PublishModelFamilyExchangeV1(search_input, batch, {});
    };
    const auto search_result = publish_search(search);
    if (!search_result.accepted) throw std::runtime_error(search_result.detail);
    Require(search_result.root_publishable && search_result.output.exact_exchange_validated &&
                search_result.output.batch.rows[0].values[3].binary_value == search_row.values[3].binary_value &&
                search_result.output.ordered_row_identities[0].search_score == 0.75,
            "search exchange lost its native score or receipt");
    for (const unsigned column : {2u, 4u}) {
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto invalid = search;
        auto& value = invalid.batch.rows.front().values[column];
        switch (mutation) {
          case 0: value.encoded_value = "1"; break;
          case 1: value.binary_value.clear(); value.encoded_value = "1"; break;
          case 2: value.binary_value.pop_back(); break;
          case 3: value.binary_value[0] ^= 2; break;
          case 4:
            ++invalid.batch.columns[column].descriptor.datatype_descriptor_generation;
            value.descriptor = invalid.batch.columns[column].descriptor;
            break;
        }
        Require(RejectedWithoutPublication(publish_search(invalid)),
                "search exchange published malformed or substituted UINT64");
      }
    }
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
      auto invalid = search;
      auto& score = invalid.batch.rows.front().values[3];
      auto& receipt = invalid.ordered_row_identities.front().search_score;
      switch (mutation) {
        case 0: score.encoded_value = "0.75"; break;
        case 1: score.binary_value.clear(); score.encoded_value = "0.75"; break;
        case 2: score.binary_value.pop_back(); break;
        case 3: ++score.descriptor.datatype_descriptor_generation; break;
        case 4: receipt.reset(); break;
        case 5: receipt = -1.0; break;
        case 6: receipt = 0.5; break;
        case 7: receipt = std::numeric_limits<double>::infinity(); break;
        case 8: receipt = std::numeric_limits<double>::quiet_NaN(); break;
        case 9: score.is_null = true; break;
        case 10: score.binary_value = {0,0,0,0,0,0,0xf0,0x7f}; break;
        case 11:
          ++invalid.batch.columns[3].descriptor.datatype_descriptor_generation;
          score.descriptor = invalid.batch.columns[3].descriptor;
          break;
      }
      Require(RejectedWithoutPublication(publish_search(invalid)),
              "search exchange published a malformed or substituted score");
    }
    auto spatial_input = input;
    spatial_input.family_id = "spatial";
    spatial_input.operation_ids = {"SPATIAL_SOURCE", "SPATIAL_NEAREST"};
    spatial_input.operation_id = "SPATIAL_NEAREST";
    spatial_input.output_descriptor_ids = {1, 2, 3, 4};
    spatial_input.maximum_rows = 3;
    spatial_input.maximum_cells = 12;
    spatial_input.spatial_geometry_descriptor_uuid = FixtureUuid(70);
    spatial_input.spatial_geometry_type_uuid = FixtureUuid(71);
    spatial_input.spatial_crs_uuid = FixtureUuid(72);
    spatial_input.spatial_crs_generation = 1;
    auto spatial = provider;
    spatial.output_descriptor_ids = spatial_input.output_descriptor_ids;
    spatial.properties.ordering_id = "spatial_distance_row_uuid_ascending_v1";
    spatial.properties.uniqueness_id = "row_uuid";
    spatial.batch = {};
    spatial.ordered_row_identities = {};
    const std::array<const char*, 4> spatial_names{"row_uuid", "spatial_value", "crs_uuid", "distance"};
    const std::array<const char*, 4> spatial_types{"uuid", "geometry", "uuid", "real64"};
    for (unsigned column = 0; column < 4; ++column) {
      auto descriptor = ex::MakeExecutorDescriptor(spatial_types[column], "nullability=non_null");
      descriptor.descriptor_kind = "scalar";
      descriptor.descriptor_uuid = FixtureUuid(75 + column);
      spatial.batch.columns.push_back({spatial_names[column], descriptor, false, column + 1});
    }
    for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
      ex::ModelProviderRowIdentityV1 row_identity;
      row_identity.row_uuid = FixtureUuid(80 + ordinal);
      spatial.ordered_row_identities.push_back(row_identity);
      ex::DescriptorTuple tuple;
      for (unsigned column = 0; column < 4; ++column) {
        api::EngineTypedValue value;
        if (column == 0 || column == 2) {
          const auto& id = column == 0 ? row_identity.row_uuid : spatial_input.spatial_crs_uuid;
          value.binary_value.assign(id.bytes.begin(), id.bytes.end());
        } else if (column == 1) {
          // Independent wire oracle for POINT(0,0): SBP1, version1, 2 axes,
          // zero reserved bytes and two big-endian binary64 zero coordinates.
          value.binary_value = {'S','B','P','1',1,2,0,0};
          value.binary_value.resize(24, 0);
        } else value = ex::EncodeReal64Value(ordinal == 2 ? 5.0 : 0.0);
        value.descriptor = spatial.batch.columns[column].descriptor;
        tuple.values.push_back(std::move(value));
      }
      spatial.batch.rows.push_back(std::move(tuple));
    }
    const auto publish_spatial = [&](const auto& batch) {
      return ex::PublishModelFamilyExchangeV1(spatial_input, batch, {});
    };
    const auto spatial_result = publish_spatial(spatial);
    if (!spatial_result.accepted) throw std::runtime_error(spatial_result.detail);
    Require(spatial_result.root_publishable && spatial_result.output.batch.rows.size() == 3 &&
                spatial_result.output.batch.rows[2].values[3].binary_value == ex::EncodeReal64Value(5.0).binary_value,
            "spatial exchange lost native distance or ordered rows");
    for (unsigned mutation = 0; mutation < 10; ++mutation) {
      auto invalid = spatial;
      auto& distance = invalid.batch.rows[0].values[3];
      switch (mutation) {
        case 0: distance.encoded_value = "0"; break;
        case 1: distance.binary_value.clear(); distance.encoded_value = "0"; break;
        case 2: distance.binary_value.pop_back(); break;
        case 3: distance.binary_value = ex::EncodeReal64Value(-1.0).binary_value; break;
        case 4: distance.binary_value = {0,0,0,0,0,0,0,0x80}; break;
        case 5: distance.binary_value = {0,0,0,0,0,0,0xf0,0x7f}; break;
        case 6: distance.binary_value = {1,0,0,0,0,0,0xf8,0x7f}; break;
        case 7: distance.is_null = true; break;
        case 8:
          ++invalid.batch.columns[3].descriptor.datatype_descriptor_generation;
          for (auto& tuple : invalid.batch.rows) tuple.values[3].descriptor = invalid.batch.columns[3].descriptor;
          break;
        case 9:
          std::swap(invalid.batch.rows[0], invalid.batch.rows[1]);
          std::swap(invalid.ordered_row_identities[0], invalid.ordered_row_identities[1]);
          break;
      }
      Require(RejectedWithoutPublication(publish_spatial(invalid)),
              "spatial exchange published malformed native distance or incorrect tie order");
    }
    std::cout << "PASS actual binary model exchange publication; component only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
