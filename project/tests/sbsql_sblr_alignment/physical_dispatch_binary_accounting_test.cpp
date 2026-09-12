// SPDX-License-Identifier: MPL-2.0
// Test actual private accounting in the owning TU. Section GC omits unrelated
// runtime entrypoints; this is not full executor or SQL/E2E qualification.
#include "../../src/engine/executor/physical_node_dispatch.cpp"
#include <iostream>
#include <stdexcept>

void CheckFilterDefaultBinaryAuthority();
namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::uint64_t Text(const std::string& text) { return text.capacity() + 1; }
std::uint64_t Descriptor(const api::EngineDescriptor& value) {
  return Text(value.descriptor_kind) + Text(value.canonical_type_name) + Text(value.encoded_descriptor);
}
std::uint64_t Context(const ex::PhysicalMgaStatementContext& value) {
  return Text(value.snapshot_kind) + Text(value.statement_timestamp) +
      8 * (value.active_excluded_local_transaction_ids.capacity() +
           value.in_doubt_excluded_local_transaction_ids.capacity());
}
api::EngineUuid Id(unsigned char value) {
  return {{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,value}};
}
}

int main() try {
  static_assert(sizeof(api::EngineUuid) == 16);
  CheckFilterDefaultBinaryAuthority();
  ex::DescriptorBatch batch;
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = Id(1); descriptor.type_uuid = Id(2);
  descriptor.collation_uuid = Id(3); descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64"; descriptor.encoded_descriptor = "nullability=non_null";
  batch.columns.push_back({"payload", descriptor, false, 1});
  api::EngineTypedValue value; value.descriptor = descriptor;
  value.binary_value = {1,2,3,4,5,6,7,8}; value.encoded_value = "payload evidence";
  batch.rows.resize(1); batch.rows[0].values.push_back(value);
  const auto& column = batch.columns[0]; const auto& row = batch.rows[0];
  const auto& cell = row.values[0];
  const std::uint64_t expected_batch = sizeof(batch) +
      batch.columns.capacity() * sizeof(ex::ExecutorColumnDescriptor) +
      batch.rows.capacity() * sizeof(ex::DescriptorTuple) + Text(column.stable_name) +
      Descriptor(column.descriptor) + row.values.capacity() * sizeof(api::EngineTypedValue) +
      Descriptor(cell.descriptor) + Text(cell.encoded_value) + cell.binary_value.capacity();
  std::uint64_t observed = 0;
  Check(ex::MaterializedBatchLiveBytes(batch, &observed) && observed == expected_batch,
        "batch binary identities must be charged once within inline slots");
  batch.columns[0].descriptor.descriptor_uuid = Id(4);
  batch.rows[0].values[0].descriptor.type_uuid = Id(5);
  Check(ex::MaterializedBatchLiveBytes(batch, &observed) && observed == expected_batch,
        "changing UUID bytes changed batch dynamic storage charges");

  ex::PhysicalNodeRecord node;
  node.required_property_uuids.resize(2); node.delivered_property_uuids.resize(3);
  node.enforced_property_uuids.resize(4); node.input_physical_node_ids.resize(2);
  node.output_descriptor_ids.resize(3);
  node.mga_statement_context.active_excluded_local_transaction_ids.resize(2);
  node.mga_statement_context.in_doubt_excluded_local_transaction_ids.resize(1);
  const std::uint64_t expected_node = sizeof(node) + Text(node.implementation_id) +
      8 * node.input_physical_node_ids.capacity() + 4 * node.output_descriptor_ids.capacity() +
      16 * (node.required_property_uuids.capacity() + node.delivered_property_uuids.capacity() +
            node.enforced_property_uuids.capacity()) + Context(node.mga_statement_context) +
      Text(node.logical_semantic_variant_id) + Text(node.transformation_rule_id) +
      Text(node.retained_cost.scalarization_policy_id);
  Check(ex::PhysicalNodeCopyLiveBytes(node, &observed) && observed == expected_node,
        "node property vectors must charge binary16 capacity, not string objects");
  node.selected_alternative_uuid = Id(6); node.executor_capability_uuid = Id(7);
  node.cost_vector_uuid = Id(8); node.transformation_uuid = Id(9);
  node.retained_cost.cost_vector_uuid = Id(10); node.retained_cost.calibration_profile_uuid = Id(11);
  node.mga_statement_context.statement_uuid = Id(12);
  Check(ex::PhysicalNodeCopyLiveBytes(node, &observed) && observed == expected_node,
        "node UUID contents cannot mint dynamic memory charges");

  ex::CanonicalPhysicalDispatchStepResult step;
  step.executed_input_physical_node_ids.resize(2); step.output_descriptor_ids.resize(3);
  step.table_sample_actuals.emplace();
  const std::uint64_t expected_step = Text(step.diagnostic.diagnostic_code) + Text(step.diagnostic.detail) +
      Text(step.executed_implementation_id) + 8 * step.executed_input_physical_node_ids.capacity() +
      4 * step.output_descriptor_ids.capacity() + Context(step.mga_statement_context) +
      Text(step.table_sample_actuals->method_id);
  Check(ex::DispatchStepMetadataLiveBytes(step, &observed) && observed == expected_step,
        "step dynamic accounting duplicates its already-owned inline UUIDs");
  step.selected_plan_uuid = Id(13); step.cancellation_evidence_uuid = Id(14);
  step.current_relation_descriptor_uuid = Id(15); step.table_sample_actuals->sample_descriptor_uuid = Id(16);
  Check(ex::DispatchStepMetadataLiveBytes(step, &observed) && observed == expected_step,
        "step UUID contents changed dynamic capacity charges");
  observed = std::numeric_limits<std::uint64_t>::max();
  Check(!ex::PhysicalMgaContextDynamicLiveBytes(node.mga_statement_context, &observed),
        "MGA context dynamic byte addition must reject overflow");
  std::cout << "PASS binary filter authority and physical metadata accounting; not runtime acceptance\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
