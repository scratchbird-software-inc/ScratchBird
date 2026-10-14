// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_query_join_composition.hpp"
#include "query/native_scalar_descriptor_projection.hpp"
#include "admitted_datatype_cohort.hpp"
#include "sblr_projection_value_runtime.hpp"
#include "sblr_dispatch.hpp"
#include "../../src/engine/public_abi_boolean_payload.hpp"
#include "registry/function_seed_registry.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <iostream>
#include <stdexcept>

namespace sb = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;
namespace ex = scratchbird::engine::executor;
namespace dt = scratchbird::core::datatypes;
namespace {
void Check(bool value, const std::string& detail) {
  if (!value) throw std::runtime_error(detail);
}
struct Inputs {
  api::TypedRelationalDag dag;
  sb::plan::CanonicalLogicalRelationalNode left_node, right_node, root;
  sb::MaterializedValues left, right;
};
Inputs Build(const api::EngineDatatypeCohort& cohort, bool rows) {
  Inputs in;
  const auto boolean = scratchbird::tests::FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000");
  for (std::uint32_t id : {1u, 2u}) {
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = id;
    descriptor.descriptor_uuid = descriptor.type_uuid = boolean;
    descriptor.descriptor_generation = descriptor.type_generation = 1;
    descriptor.codec_id = "datatype.boolean.u8.v1";
    descriptor.codec_version = descriptor.codec_generation = 1;
    descriptor.datatype_identity_authoritative = true;
    descriptor.statement_receipt_uuid = scratchbird::tests::FixtureUuid(281, id);
    descriptor.datatype_catalog_snapshot_uuid = cohort.catalog_snapshot_uuid;
    descriptor.datatype_catalog_generation = cohort.catalog_generation;
    descriptor.datatype_registry_generation = cohort.registry_generation;
    descriptor.nullability = api::RelationalNullability::kNullable;
    in.dag.descriptors.push_back(descriptor);
    api::EngineDescriptor runtime;
    Check(api::BuildExactCanonicalScalarRuntimeDescriptorV1(descriptor,
        dt::CanonicalTypeId::boolean, &runtime), "exact BOOLEAN fixture descriptor refused");
    auto& input = id == 1 ? in.left : in.right;
    input.batch.columns.push_back({"b", runtime, true, id});
    input.result_bindings.push_back({0, false, {}});
    if (rows) {
      api::EngineTypedValue present;
      present.descriptor = runtime;
      present.setState(api::EngineValueState::value);
      present.binary_value = {1};
      input.batch.rows.push_back({{present}});
      api::EngineTypedValue null;
      null.descriptor = runtime;
      null.setState(api::EngineValueState::sql_null);
      input.batch.rows.push_back({{null}});
    }
  }
  in.left_node.logical_node_id = 1;
  in.left_node.output_descriptor_ids = {1};
  in.right_node.logical_node_id = 2;
  in.right_node.output_descriptor_ids = {2};
  in.root.logical_node_id = 3;
  in.root.output_descriptor_ids = {1, 2};
  return in;
}
sb::PreparedJoinRoot Prepare(Inputs& in) {
  return sb::PrepareJoinRootForComposition(in.dag, in.root, in.left_node,
      in.right_node, in.left, in.right, ex::CanonicalAcceptedJoinKind::kCross);
}
void Carriers(const api::EngineDescriptor& descriptor) {
  for (bool nullable : {false, true}) {
    auto bound = descriptor;
    bound.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
    for (bool truth : {false, true}) {
      const auto value = ex::EncodeBoundBoolValue(truth, bound);
      const auto decoded = ex::DecodeBoolValue(value);
      Check(decoded.ok() && decoded.value == truth, "native Boolean decode failed");
      std::string_view transport;
      Check(scratchbird::engine::PublicBooleanScalarPayloadV1(value,&transport) &&
            transport.size()==1 && static_cast<unsigned char>(transport[0])==static_cast<unsigned>(truth),
            "public Boolean projection changed native byte");
      ex::DescriptorRuntimeDiagnostic diagnostic;
      const auto cast = ex::CastDescriptorValue(value, bound, &diagnostic);
      Check(diagnostic.ok && cast.binary_value == value.binary_value &&
            cast.encoded_value.empty(), "native nonnullable Boolean identity cast failed");
      api::EngineProjectionFunctionArgument argument;
      argument.type_name = "boolean"; argument.descriptor = bound;
      argument.state = api::EngineValueState::value;
      argument.binary_value = value.binary_value;
      const auto projected = sb::SblrValueFromProjectionArgument(argument);
      Check(sb::ProjectionSblrValueResolved(projected), "native function argument unresolved");
      const auto roundtrip = sb::EngineTypedValueFromSblrValue(projected);
      Check(roundtrip.descriptor == value.descriptor && roundtrip.state == value.state &&
            roundtrip.is_null == value.is_null && roundtrip.binary_value == value.binary_value &&
            roundtrip.encoded_value.empty(),
            "native function argument roundtrip changed descriptor or payload");
      for (unsigned mutation = 0; mutation != 5; ++mutation) {
        auto invalid = value;
        if (mutation == 0) { invalid.binary_value.clear(); invalid.encoded_value = "true"; }
        if (mutation == 1) invalid.encoded_value = "true";
        if (mutation == 2) invalid.binary_value = {2};
        if (mutation == 3) invalid.binary_value.clear();
        if (mutation == 4) invalid.binary_value = {1,0};
        ex::DescriptorBatch batch;
        batch.columns = {{"b", bound, nullable, 1}};
        batch.rows = {{{invalid}}};
        Check(!ex::ValidateDescriptorBatch(batch).ok &&
              !ex::ValidateCanonicalDescriptorBatch(batch, {1}).ok,
              "a receiver accepted malformed Boolean carrier");
        transport="unchanged";
        Check(!scratchbird::engine::PublicBooleanScalarPayloadV1(invalid,&transport) &&
              transport=="unchanged", "public Boolean projection accepted malformed payload or mutated failure");
      }
      for (unsigned mutation = 0; mutation != 4; ++mutation) {
        auto invalid = bound;
        if (mutation == 0) invalid.datatype_cohort = {};
        if (mutation == 1) invalid.encoded_descriptor += ";codec_id=foreign";
        if (mutation == 2) invalid.encoded_descriptor += ";length=1";
        if (mutation == 3) invalid.encoded_descriptor += ";codec_version=1";
        const auto bad_cast = ex::CastDescriptorValue(value, invalid, &diagnostic);
        Check(!diagnostic.ok, "cast accepted an unadmitted target Boolean profile");
        ex::DescriptorBatch batch;
        batch.columns = {{"b", invalid, nullable, 1}};
        Check(!ex::ValidateDescriptorBatch(batch).ok &&
              !ex::ValidateCanonicalDescriptorBatch(batch, {1}).ok,
              "empty batch bypassed Boolean admission");
      }
    }
    auto null = ex::EncodeBoundBoolValue(false, bound);
    null.setState(api::EngineValueState::sql_null);
    null.binary_value.clear();
    std::string_view transport="unchanged";
    Check(scratchbird::engine::PublicBooleanScalarPayloadV1(null,&transport) && transport.empty(),
          "public Boolean NULL projection has payload");
    for (unsigned mutation=0; mutation<3; ++mutation) {
      auto invalid=null;
      if(mutation==0) invalid.is_null=false;
      if(mutation==1) invalid.state=api::EngineValueState::value;
      if(mutation==2) { invalid.setState(api::EngineValueState::error); invalid.binary_value={1}; }
      transport="unchanged";
      Check(!scratchbird::engine::PublicBooleanScalarPayloadV1(invalid,&transport) &&
            transport=="unchanged", "public Boolean projection admitted state/flag mismatch");
    }
    ex::DescriptorBatch batch;
    batch.columns = {{"b", bound, nullable, 1}}; batch.rows = {{{null}}};
    Check(ex::ValidateDescriptorBatch(batch).ok == nullable &&
          ex::ValidateCanonicalDescriptorBatch(batch, {1}).ok == nullable,
          "Boolean NULL ignored declared nullability");
  }
}
void HeapJoin(const Inputs& in) {
  // Algorithm qualification only: this explicit closure seam is not a live
  // node inventory receipt, transport admission or storage ownership claim.
  const auto id = [](unsigned n) { return scratchbird::tests::FixtureUuid(282, n); };
  ex::CanonicalJoinKindRequest request;
  request.join_kind = ex::CanonicalAcceptedJoinKind::kCross;
  request.bound_pair_truth_profile = true;
  request.maximum_output_rows = 64;
  auto& key = request.residual_request.key_request;
  key.left_batch = in.left.batch; key.right_batch = in.right.batch;
  key.selected_physical_node_id = 3;
  request.residual_request.residual_truth_values.assign(
      key.left_batch.rows.size() * key.right_batch.rows.size(), api::EngineSqlTruthValue::true_value);
  auto& dag = key.physical_dag;
  dag.root_physical_node_id = 3;
  dag.nodes = {
      {.physical_node_id=1, .relational_node_id=1, .node_kind=ex::PhysicalNodeKind::kValues,
       .implementation_id="values.typed.v1", .output_descriptor_ids={1}, .causal_counter_id=101},
      {.physical_node_id=2, .relational_node_id=2, .node_kind=ex::PhysicalNodeKind::kValues,
       .implementation_id="values.typed.v1", .output_descriptor_ids={2}, .causal_counter_id=102},
      {.physical_node_id=3, .relational_node_id=3, .node_kind=ex::PhysicalNodeKind::kJoin,
       .implementation_id="join.cross.3vl.nested.v1", .input_physical_node_ids={1,2},
       .output_descriptor_ids={1,2}, .causal_counter_id=103}};
  ex::PhysicalMgaStatementContext context{id(1),id(2),id(3),id(4),100,0,80,70,70,70,
      {80,100},{90},"statement_stable",110,true,true,true};
  dag.abi_version=2; dag.local_transaction_id=100; dag.selected_plan_uuid=id(5);
  dag.admission_evidence={
      {ex::PhysicalAdmissionStage::kBoundRequest,id(10)},
      {ex::PhysicalAdmissionStage::kCatalogEpoch,id(11)},
      {ex::PhysicalAdmissionStage::kSecurity,id(12)},
      {ex::PhysicalAdmissionStage::kMgaStatementBoundary,id(3)},
      {ex::PhysicalAdmissionStage::kPolicyCapability,id(14)},
      {ex::PhysicalAdmissionStage::kResource,id(15)},
      {ex::PhysicalAdmissionStage::kStatisticsProvenance,id(16)},
      {ex::PhysicalAdmissionStage::kCanonicalRoute,id(17)}};
  dag.bound_sblr_tree_uuid=id(10); dag.catalog_epoch_uuid=id(11);
  dag.security_context_uuid=id(12); dag.capability_snapshot_uuid=id(14);
  dag.resource_snapshot_uuid=id(15); dag.statistics_snapshot_uuid=id(16); dag.route_snapshot_uuid=id(17);
  dag.catalog_generation=dag.security_epoch=dag.policy_epoch=dag.resource_epoch=
      dag.statistics_generation=dag.route_epoch=dag.route_generation=1;
  dag.memory_budget_bytes=32*1024*1024;
  dag.optimizer_published=dag.immutable_node_identity_validated=dag.capability_validated_before_access=true;
  dag.mga_statement_context=context;
  for (auto& node : dag.nodes) {
    node.mga_statement_context=context; node.selected_alternative_uuid=id(20);
    node.executor_capability_uuid=id(21); node.executor_capability_abi_version=1;
    node.cost_vector_uuid=id(22); node.memory_bytes_required=dag.memory_budget_bytes;
    node.engine_capability_validated=true;
  }
  key.mga_authority.statement_context=context;
  key.mga_authority.origin=ex::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  key.mga_authority.resolve_current=[context] {
    ex::CanonicalMgaCurrentResolution current; current.statement_context=context; return current;
  };
  const auto result=ex::ExecuteCanonicalJoinKind(request);
  Check(result.diagnostic.ok, "native heap Boolean join refused: " + result.diagnostic.detail);
  Check(result.output_batch.rows.size()==key.left_batch.rows.size()*key.right_batch.rows.size(),
        "native heap Boolean join cardinality changed");
  for (std::size_t i=0; i<key.left_batch.rows.size(); ++i)
    for (std::size_t j=0; j<key.right_batch.rows.size(); ++j) {
      const auto& row=result.output_batch.rows[i*key.right_batch.rows.size()+j];
      for (unsigned side=0; side<2; ++side) {
        const auto& expected=side==0 ? key.left_batch.rows[i].values[0] : key.right_batch.rows[j].values[0];
        Check(row.values[side].descriptor==expected.descriptor && row.values[side].state==expected.state &&
              row.values[side].is_null==expected.is_null && row.values[side].binary_value==expected.binary_value &&
              row.values[side].encoded_value.empty(), "heap join lost Boolean byte, NULL or descriptor");
      }
    }
}
void RegisteredBoolean(const api::EngineDescriptor& descriptor) {
  const auto package = scratchbird::engine::functions::BuildStandardFunctionSeedPackage();
  const auto* entry = package.registry.Lookup("sb.scalar.is_alpha");
  Check(entry != nullptr, "registered alpha predicate missing");
  api::EngineProjectionFunctionRequest request;
  request.function_uuid = entry->function_uuid;
  request.function_id = entry->function_id;
  request.context.security_context_present = true;
  request.result_descriptor = descriptor;
  request.result_descriptor.encoded_descriptor = "nullability=nullable";
  auto character = ex::MakeExecutorDescriptor("character", "nullability=nullable", descriptor.datatype_cohort);
  character.descriptor_kind = "scalar";
  for (const auto text : {"Alphabet", "123"}) {
    api::EngineTypedValue argument;
    argument.descriptor = character; argument.encoded_value = text;
    request.arguments = {api::MakeProjectionFunctionArgument("address", argument)};
    const auto result = sb::EvaluateProjectionFunctionForTest(request);
    const auto detail = result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
    Check(result.ok, "registered Boolean publication failed: " + detail);
    Check(result.value.descriptor == request.result_descriptor && result.value.encoded_value.empty() &&
          result.value.binary_value == std::vector<std::uint8_t>{static_cast<std::uint8_t>(std::string_view(text)=="Alphabet")},
          "registered predicate changed truth or exact binding");
    for (unsigned mutation=0; mutation<3; ++mutation) {
      auto bad=request;
      if (mutation==0) bad.result_descriptor.datatype_cohort={};
      if (mutation==1) bad.result_descriptor.encoded_descriptor += ";length=1";
      if (mutation==2) ++bad.result_descriptor.datatype_descriptor_generation;
      const auto refused=sb::EvaluateProjectionFunctionForTest(bad);
      Check(!refused.ok && !refused.diagnostics.empty() &&
            refused.diagnostics.front().code=="DATATYPE.DESCRIPTOR.INVALID",
            "registered function accepted unadmitted Boolean result schema");
    }
  }
  api::EngineTypedValue null;
  null.descriptor=character; null.setState(api::EngineValueState::sql_null);
  request.arguments={api::MakeProjectionFunctionArgument("address",null)};
  const auto result=sb::EvaluateProjectionFunctionForTest(request);
  Check(result.ok && result.value.descriptor==request.result_descriptor && result.value.isSqlNull() &&
        result.value.binary_value.empty() && result.value.encoded_value.empty(),
        "registered predicate lost typed NULL");
  const auto* conditional = package.registry.Lookup("sb.scalar.oracle_decode");
  Check(conditional != nullptr, "registered conditional missing");
  request.function_uuid=conditional->function_uuid; request.function_id=conditional->function_id;
  auto source=request.result_descriptor;
  source.descriptor_uuid=scratchbird::tests::FixtureUuid(283,1);
  source.encoded_descriptor="nullability=non_null";
  const auto selected=ex::EncodeBoundBoolValue(true,source);
  request.arguments={api::MakeProjectionFunctionArgument("expression",selected),
      api::MakeProjectionFunctionArgument("search",selected),
      api::MakeProjectionFunctionArgument("result",selected)};
  const auto chosen=sb::EvaluateProjectionFunctionForTest(request);
  const auto detail=chosen.diagnostics.empty()?std::string{}:chosen.diagnostics.front().detail;
  Check(chosen.ok && chosen.value.descriptor==request.result_descriptor &&
        chosen.value.binary_value==std::vector<std::uint8_t>{1} && chosen.value.encoded_value.empty(),
        "selected Boolean result failed legitimate occurrence rebinding: "+detail);
  for (unsigned mutation=0; mutation<6; ++mutation) {
    auto bad=request;
    auto& argument=bad.arguments[2];
    if(mutation==0) argument.descriptor.datatype_cohort={};
    if(mutation==1) argument.descriptor.encoded_descriptor += ";length=1";
    if(mutation==2) { argument.state=api::EngineValueState::sql_null; argument.is_null=true; argument.binary_value.clear(); }
    if(mutation==3) argument.type_name="character";
    if(mutation==4) ++argument.descriptor.datatype_descriptor_generation;
    if(mutation==5) { argument.type_name="character"; argument.descriptor.canonical_type_name="character"; }
    const auto refused=sb::EvaluateProjectionFunctionForTest(bad);
    Check(!refused.ok && !refused.diagnostics.empty(), "conditional accepted malformed Boolean input");
  }
  auto null_source=source;
  null_source.encoded_descriptor="nullability=nullable";
  auto selected_null=ex::EncodeBoundBoolValue(false,null_source);
  selected_null.setState(api::EngineValueState::sql_null); selected_null.binary_value.clear();
  request.arguments[2]=api::MakeProjectionFunctionArgument("result",selected_null);
  request.result_descriptor.encoded_descriptor="nullability=non_null";
  const auto refused_null=sb::EvaluateProjectionFunctionForTest(request);
  Check(!refused_null.ok && !refused_null.diagnostics.empty(),
        "conditional selected nullable NULL into nonnullable result");
}
}
int main() try {
  for (const auto& cohort : {
      api::EngineDatatypeCohort{dt::kDatatypeCohortV5, 5, 5},
      api::EngineDatatypeCohort{dt::kDatatypeCohortV10, 10, 10},
      api::EngineDatatypeCohort{dt::kDatatypeCohortV11, 11, 11}}) {
    for (bool rows : {false, true}) {
      auto in = Build(cohort, rows);
      const auto prepared = Prepare(in);
      Check(prepared.ok, "current BOOLEAN join preparation refused: " + prepared.detail);
      Carriers(in.left.batch.columns[0].descriptor);
      HeapJoin(in);
      RegisteredBoolean(in.left.batch.columns[0].descriptor);
      for (const auto* input : {&in.left, &in.right}) {
        Check(input->batch.columns[0].descriptor.datatype_cohort == cohort,
              "join column lost exact source cohort");
        const auto admitted = ex::ValidateDescriptorBatch(input->batch);
        Check(admitted.ok, "receiving executor refused actual projected BOOLEAN batch: " +
              admitted.diagnostic_code + ":" + admitted.detail);
        for (const auto& row : input->batch.rows)
          Check(row.values[0].descriptor == input->batch.columns[0].descriptor,
                "join row descriptor differs from projected column");
      }
      for (unsigned mutation = 0; mutation != 6; ++mutation) {
        auto invalid = Build(cohort, rows);
        auto& descriptor = invalid.left.batch.columns[0].descriptor;
        if (mutation == 0) descriptor.datatype_cohort.catalog_snapshot_uuid.bytes[15] ^= 0x40;
        if (mutation == 1) ++descriptor.datatype_cohort.catalog_generation;
        if (mutation == 2) ++descriptor.datatype_cohort.registry_generation;
        if (mutation == 3) descriptor.datatype_cohort = {};
        if (mutation == 4) descriptor.type_uuid.bytes[15] ^= 0x40;
        if (mutation == 5) ++descriptor.datatype_descriptor_generation;
        for (auto& row : invalid.left.batch.rows) row.values[0].descriptor = descriptor;
        const auto before = descriptor;
        const auto refused = Prepare(invalid);
        Check(!refused.ok && invalid.left.batch.columns[0].descriptor == before,
              "join repaired or accepted a substituted already-bound tuple");
      }
    }
  }
  std::cout << "boolean_join_cohort_binding=passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
