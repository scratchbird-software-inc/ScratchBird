// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual exchange implementation; fixture receipts prove component admission
// and publication only, never real provider/MGA authority or SQL execution.
#include "../../src/engine/executor/model_family_exchange.hpp"
#include "../../src/engine/optimizer/model_family_coordinator.hpp"

#include <iostream>
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
    std::cout << "PASS actual binary model exchange publication; component only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
