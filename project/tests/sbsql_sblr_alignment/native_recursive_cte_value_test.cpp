// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "descriptor_value_runtime.hpp"
#include "executor_foundation.hpp"
#include "admitted_datatype_cohort.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace {
auto Id(unsigned value) { return scratchbird::tests::FixtureUuid(87, value); }
void Check(bool good, const std::string& detail) {
  if (!good) throw std::runtime_error(detail);
}
api::EngineDescriptor Descriptor(const char* name, unsigned occurrence,
                                const api::EngineDatatypeCohort& cohort) {
  auto result = ex::MakeExecutorDescriptor(name, "nullability=non_null", cohort);
  Check(!result.type_uuid.is_nil(), "missing fixture cohort binding");
  result.descriptor_uuid = Id(occurrence);
  result.descriptor_kind = "scalar";
  return result;
}
api::EngineTypedValue Integer(const api::EngineDescriptor& descriptor, std::uint64_t n) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.state = api::EngineValueState::value;
  value.binary_value.resize(8);
  for (unsigned i=0; i<8; ++i) value.binary_value[i] = static_cast<std::uint8_t>(n >> (8*i));
  return value;
}
std::uint64_t Number(const api::EngineTypedValue& value) {
  Check(value.state == api::EngineValueState::value && !value.is_null &&
        value.encoded_value.empty() && value.binary_value.size() == 8,
        "recursive INT64 is not native LE8");
  std::uint64_t n = 0;
  for (unsigned i=0; i<8; ++i) n |= std::uint64_t(value.binary_value[i]) << (8*i);
  return n;
}
// Explicit component authority: this fixture does not claim an actual inventory read.
ex::CanonicalExecutionMgaAuthority Bind(ex::TypedPhysicalNodeDag& dag) {
  ex::PhysicalMgaStatementContext context{
    Id(1), Id(2), Id(3), Id(4), 100, 0, 80, 70, 70, 70, {80,100}, {90},
    "statement_stable", 110, true, true, true};
  dag.abi_version = 2;
  dag.local_transaction_id = 100;
  dag.statement_snapshot_id = 0;
  dag.selected_plan_uuid = Id(5);
  dag.admission_evidence = {
    {ex::PhysicalAdmissionStage::kBoundRequest, Id(10)},
    {ex::PhysicalAdmissionStage::kCatalogEpoch, Id(11)},
    {ex::PhysicalAdmissionStage::kSecurity, Id(12)},
    {ex::PhysicalAdmissionStage::kMgaStatementBoundary, Id(3)},
    {ex::PhysicalAdmissionStage::kPolicyCapability, Id(14)},
    {ex::PhysicalAdmissionStage::kResource, Id(15)},
    {ex::PhysicalAdmissionStage::kStatisticsProvenance, Id(16)},
    {ex::PhysicalAdmissionStage::kCanonicalRoute, Id(17)}};
  dag.bound_sblr_tree_uuid = Id(10); dag.catalog_epoch_uuid = Id(11);
  dag.security_context_uuid = Id(12); dag.capability_snapshot_uuid = Id(14);
  dag.resource_snapshot_uuid = Id(15); dag.statistics_snapshot_uuid = Id(16);
  dag.route_snapshot_uuid = Id(17);
  dag.catalog_generation = dag.security_epoch = dag.policy_epoch = dag.resource_epoch =
      dag.statistics_generation = dag.route_epoch = dag.route_generation = 1;
  dag.memory_budget_bytes = 32*1024*1024;
  dag.optimizer_published = dag.immutable_node_identity_validated =
      dag.capability_validated_before_access = true;
  dag.mga_statement_context = context;
  for (auto& node : dag.nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid = Id(20);
    node.executor_capability_uuid = Id(21);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = Id(22);
    node.memory_bytes_required = dag.memory_budget_bytes;
    node.engine_capability_validated = true;
  }
  ex::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = ex::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [context] {
    ex::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    return current;
  };
  return authority;
}
ex::CanonicalRecursiveCteWorkingRequest Working(const api::EngineDatatypeCohort& cohort) {
  ex::CanonicalRecursiveCteWorkingRequest request;
  auto& dag = request.physical_dag;
  dag.root_physical_node_id = 3;
  dag.nodes = {
    {.physical_node_id=1, .relational_node_id=1, .node_kind=ex::PhysicalNodeKind::kValues,
     .implementation_id="values.cte-anchor.typed.v1", .output_descriptor_ids={1}, .causal_counter_id=101},
    {.physical_node_id=2, .relational_node_id=2, .node_kind=ex::PhysicalNodeKind::kCte,
     .implementation_id="cte.recursive-term.typed.v1", .output_descriptor_ids={1}, .causal_counter_id=102},
    {.physical_node_id=3, .relational_node_id=3, .node_kind=ex::PhysicalNodeKind::kRecursiveCte,
     .implementation_id="cte.recursive.working.typed.v1", .input_physical_node_ids={1,2},
     .output_descriptor_ids={1}, .causal_counter_id=103}};
  request.selected_physical_node_id = 3;
  auto descriptor = Descriptor("int64", 30, cohort);
  request.anchor_batch = ex::MakeDescriptorBatch({{"n", descriptor, false, 1}}, {{{Integer(descriptor,1)}}});
  request.recursive_step = [descriptor](const ex::DescriptorBatch& batch, std::size_t) {
    ex::DescriptorBatch result;
    result.columns = batch.columns;
    for (const auto& row : batch.rows) {
      const auto n = Number(row.values.at(0));
      if (n<4) result.rows.push_back({{Integer(descriptor,n+1)}});
    }
    result.rows.shrink_to_fit();
    return result;
  };
  request.maximum_iteration_count = 8;
  request.maximum_working_row_count = 8;
  request.maximum_result_row_count = 16;
  request.enforce_payload_memory_grant = true;
  request.mga_authority = Bind(dag);
  return request;
}
ex::CanonicalRecursiveCteSearchCycleRequest Search(const api::EngineDatatypeCohort& cohort) {
  auto base = Working(cohort);
  ex::CanonicalRecursiveCteSearchCycleRequest request;
  request.physical_dag = base.physical_dag;
  request.physical_dag.nodes.back().implementation_id = "cte.recursive.search-breadth-cycle-int64.typed.v1";
  request.physical_dag.nodes.back().output_descriptor_ids = {1,2,3};
  request.selected_physical_node_id = 3;
  request.anchor_batch = base.anchor_batch;
  const auto descriptor = base.anchor_batch.columns[0].descriptor;
  request.recursive_step = [descriptor](const ex::DescriptorBatch& batch, std::size_t) {
    ex::CanonicalRecursiveCteGeneratedBatch result;
    result.batch.columns = batch.columns;
    for (std::size_t parent=0; parent<batch.rows.size(); ++parent) {
      const auto add = [&](unsigned n) {
        result.batch.rows.push_back({{Integer(descriptor,n)}});
        result.parent_working_row_indices.push_back(parent);
      };
      switch (Number(batch.rows[parent].values.at(0))) {
        case 1: add(2); add(3); break;
        case 2: add(1); break;
        case 3: add(4); break;
      }
    }
    result.batch.rows.shrink_to_fit();
    result.parent_working_row_indices.shrink_to_fit();
    return result;
  };
  request.cycle_key_expression_descriptor_id = 1;
  request.search_sequence_column = {"search_sequence", Descriptor("int64",31,cohort), false,2};
  request.cycle_mark_column = {"is_cycle", Descriptor("boolean",32,cohort), false,3};
  request.maximum_iteration_count = 8;
  request.maximum_working_row_count = 8;
  request.maximum_result_row_count = 16;
  request.enforce_payload_memory_grant = true;
  request.mga_authority = base.mga_authority;
  return request;
}
void Corrupt(api::EngineDescriptor& descriptor, unsigned mutation) {
  switch (mutation) {
    case 0: descriptor.datatype_cohort.catalog_snapshot_uuid = {}; break;
    case 1: ++descriptor.datatype_cohort.catalog_generation; break;
    case 2: ++descriptor.datatype_cohort.registry_generation; break;
    case 3: descriptor.datatype_descriptor_uuid = {}; break;
    case 4: ++descriptor.datatype_descriptor_generation; break;
    case 5: descriptor.type_uuid = {}; break;
    case 6: descriptor.charset_uuid = Id(80); break;
    case 7: descriptor.collation_uuid = Id(81); break;
    case 8: descriptor.encoded_descriptor = "nullability=nullable"; break;
    case 9: descriptor.descriptor_kind = "executor.scalar"; break;
    case 10: descriptor.descriptor_uuid = {}; break;
    case 11: {
      const auto wrong=Descriptor(descriptor.canonical_type_name=="int64" ? "boolean" : "int64",
                                  82, descriptor.datatype_cohort);
      descriptor.datatype_descriptor_uuid=wrong.datatype_descriptor_uuid;
      descriptor.datatype_descriptor_generation=wrong.datatype_descriptor_generation;
      break;
    }
    case 12: descriptor.descriptor_uuid = Id(30); break; // aliases anchor occurrence
  }
}
void CountAnchor(const api::EngineDatatypeCohort& cohort, bool empty) {
  auto working=Working(cohort);
  ex::CanonicalDescriptorCountRequest count;
  auto values=working.physical_dag.nodes[0];
  values.physical_node_id=4; values.relational_node_id=4;
  values.output_descriptor_ids={4}; values.causal_counter_id=104;
  auto aggregate=working.physical_dag.nodes[0];
  aggregate.node_kind=ex::PhysicalNodeKind::kAggregate;
  aggregate.logical_semantic_variant_id="aggregate.global-count-star.v1";
  aggregate.implementation_id="aggregate.count-star.v1";
  aggregate.input_physical_node_ids={4};
  count.physical_dag.nodes={values,aggregate};
  count.physical_dag.root_physical_node_id=1;
  count.selected_physical_node_id=1;
  count.count_column=working.anchor_batch.columns[0];
  auto input_descriptor=Descriptor("int64",33,cohort);
  count.input_batch=ex::MakeDescriptorBatch({{"input",input_descriptor,false,4}},{});
  if (!empty) count.input_batch.rows={{{Integer(input_descriptor,19)}},{{Integer(input_descriptor,91)}}};
  count.mga_authority=Bind(count.physical_dag);
  const auto counted=ex::ExecuteCanonicalDescriptorCountStar(count);
  Check(counted.diagnostic.ok && counted.output_batch.rows.size()==1,
        "actual count anchor: "+counted.diagnostic.detail);
  Check(Number(counted.output_batch.rows[0].values[0])==(empty?0:2),"count cardinality");
  working.anchor_batch=counted.output_batch;
  working.physical_dag.nodes[0]=aggregate;
  working.physical_dag.nodes[1].logical_semantic_variant_id="cte.recursive-term-int64-increment.v1";
  working.physical_dag.nodes[1].implementation_id="cte.recursive-term.int64-increment.typed.v1";
  working.physical_dag.nodes.push_back(values);
  working.mga_authority=Bind(working.physical_dag);
  const auto executed=ex::ExecuteCanonicalRecursiveCteWorking(working);
  Check(executed.diagnostic.ok && executed.converged, "materialized count recursion: "+executed.diagnostic.detail);
  const unsigned start=empty?0:2;
  Check(executed.output_batch.rows.size()==5-start,"count recursion cardinality");
  for(unsigned i=0;i<executed.output_batch.rows.size();++i)
    Check(Number(executed.output_batch.rows[i].values[0])==start+i,"count recursion native value");
}
void Test(const api::EngineDatatypeCohort& cohort) {
  CountAnchor(cohort,false);
  CountAnchor(cohort,true);
  auto working = Working(cohort);
  const auto result = ex::ExecuteCanonicalRecursiveCteWorking(working);
  Check(result.diagnostic.ok && result.converged, "working: "+result.diagnostic.detail);
  Check(result.output_batch.rows.size()==4 && result.output_payload_bytes==32,
        "working cardinality or binary payload accounting");
  for (unsigned i=0; i<4; ++i) Check(Number(result.output_batch.rows[i].values[0])==i+1,"working value");
  auto search = Search(cohort);
  auto found = ex::ExecuteCanonicalRecursiveCteSearchCycle(search);
  Check(found.diagnostic.ok && found.converged, "search: "+found.diagnostic.detail);
  Check(found.output_batch.rows.size()==5 && found.cycle_row_count==1 && found.row_metadata.size()==5,
        "search graph cardinality");
  const std::array<unsigned,5> expected{1,2,3,1,4};
  std::size_t actual_payload=0;
  for (unsigned i=0; i<5; ++i) {
    const auto& row=found.output_batch.rows[i];
    Check(Number(row.values[0])==expected[i] && Number(row.values[1])==i+1,"search native sequence");
    Check(row.values[1].descriptor==search.search_sequence_column.descriptor &&
          row.values[2].descriptor==search.cycle_mark_column.descriptor,"generated binding drift");
    const auto cycle=ex::DecodeBoolValue(row.values[2]);
    Check(cycle.diagnostic.ok && cycle.value==(i==3) && found.row_metadata[i].cycle==(i==3),"cycle truth");
    for (const auto& cell:row.values) actual_payload+=cell.encoded_value.size()+cell.binary_value.size();
  }
  Check(found.output_payload_bytes==actual_payload && actual_payload==85,"search payload accounting");
  for (bool empty : {false,true}) {
    auto valid=search;
    if(empty) { valid.anchor_batch.rows.clear(); valid.anchor_batch.rows.shrink_to_fit(); }
    for(unsigned column=0; column<2; ++column) for(unsigned mutation=0; mutation<15; ++mutation) {
      auto bad=valid;
      auto& changed=column ? bad.cycle_mark_column : bad.search_sequence_column;
      if(mutation==13) changed.nullable=true;
      else if(mutation==14) changed.descriptor_id=1;
      else Corrupt(changed.descriptor, mutation);
      unsigned callback_count=0;
      const auto step=bad.recursive_step;
      bad.recursive_step=[&](const ex::DescriptorBatch& batch,std::size_t ordinal) {
        ++callback_count; return step(batch,ordinal);
      };
      const auto refused=ex::ExecuteCanonicalRecursiveCteSearchCycle(bad);
      Check(!refused.diagnostic.ok && refused.output_batch.rows.empty() && callback_count==0,
            "malformed SEARCH binding accepted: column="+std::to_string(column)+" mutation="+
            std::to_string(mutation)+" empty="+std::to_string(empty));
    }
    const auto retry=ex::ExecuteCanonicalRecursiveCteSearchCycle(valid);
    Check(retry.diagnostic.ok && retry.output_batch.rows.size()==(empty?0:5),"search retry");
  }
  for(bool short_grant:{false,true}) {
    auto limited=search;
    const auto grant=found.peak_live_memory_bytes-(short_grant?1:0);
    Check(grant!=0,"missing peak grant measurement");
    limited.physical_dag.memory_budget_bytes=grant;
    for(auto& node:limited.physical_dag.nodes) node.memory_bytes_required=grant;
    const auto result=ex::ExecuteCanonicalRecursiveCteSearchCycle(limited);
    if(short_grant)
      Check(!result.diagnostic.ok && result.output_batch.rows.empty() &&
            result.diagnostic.diagnostic_code=="QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1", "one-short grant accepted: "+result.diagnostic.detail);
    else Check(result.diagnostic.ok && result.output_payload_bytes==85,"exact peak grant refused: "+result.diagnostic.detail);
  }
  auto cancelled=search;
  cancelled.cancellation_evidence_uuid=Id(14);
  cancelled.cancellation_requested=[](std::size_t ordinal){return ordinal>=1;};
  const auto stopped=ex::ExecuteCanonicalRecursiveCteSearchCycle(cancelled);
  Check(!stopped.diagnostic.ok && stopped.cancellation_observed && stopped.working_state_cleaned &&
        stopped.output_batch.rows.empty(),"cancellation retained output/state");
  Check(ex::ExecuteCanonicalRecursiveCteSearchCycle(search).diagnostic.ok,"retry after cancellation");
  auto depth=search;
  depth.search_order=ex::CanonicalRecursiveCteSearchOrder::kDepthFirst;
  const auto unsupported=ex::ExecuteCanonicalRecursiveCteSearchCycle(depth);
  Check(!unsupported.diagnostic.ok && unsupported.output_batch.rows.empty() &&
        unsupported.diagnostic.detail=="recursive CTE SEARCH order is outside the accepted profile",
        "unsupported depth-first profile falsely succeeded");
}
} // namespace
int main() {
  try {
    for(const auto& cohort:{api::EngineDatatypeCohort{dt::kDatatypeCohortV5,5,5},
                           api::EngineDatatypeCohort{dt::kDatatypeCohortV10,10,10},
                           api::EngineDatatypeCohort{dt::kDatatypeCohortV11,11,11}}) Test(cohort);
    return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
