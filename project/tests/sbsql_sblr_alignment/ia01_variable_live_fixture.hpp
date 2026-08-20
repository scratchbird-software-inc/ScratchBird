#pragma once

#include "engine/internal_api/sblr_variable_frame_coordinator.hpp"
#include "engine/sblr/sblr_variable_runtime.hpp"

namespace variable_fixture {
namespace runtime = scratchbird::engine::sblr;

struct Live {
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  Submission submission;
  Bytes sbve;
  std::string parser_uuid;
};

inline std::array<std::uint8_t,32> DomainHash(std::string_view domain,
                                             const Bytes& bytes) {
  Bytes input(domain.begin(),domain.end());input.insert(input.end(),bytes.begin(),bytes.end());
  return scratchbird::core::hash::ComputeSha256Digest(input).digest;
}

inline Live Build(Fixture& fixture, PublicSession& session,
                  api::EngineRequestContext* context) {
  Require(context!=nullptr,"variable fixture context missing");
  const auto operation=Text(NewUuid(platform::UuidKind::object,9361));
  auto coordinator_context=*context;
  coordinator_context.statement_uuid.canonical=Text(NewUuid(platform::UuidKind::object,9360));
  coordinator_context.statement_metadata_snapshot_engine_owned=true;
  coordinator_context.trace_tags.push_back("private_variable_frame_coordination");
  coordinator_context.catalog_epoch_uuid.canonical=
      "019d0000-0000-7000-8000-00000000d701";
  coordinator_context.catalog_generation_id=1;
  api::SblrVariableFrameDemand structural;
  structural.declaration_occurrence_id=1;structural.datatype_context_code=1;
  structural.mutability=api::SblrVariableMutability::mutable_value;
  structural.initial_state=api::SblrVariableValueState::uninitialized;
  structural.declaration_token_sha256="sha256:"+std::string(64,'a');
  auto begun=api::BeginSblrVariableFrame(coordinator_context,operation,
      UINT64_MAX,{structural});
  if(!begun.ok)std::cerr<<"variable-begin:"<<begun.diagnostic.code<<':'<<begun.diagnostic.message_key<<':'<<begun.diagnostic.detail<<'\n';
  Require(begun.ok&&begun.snapshot.mappings.size()==1,"variable frame begin failed");
  bridge::StatementContextAcquireRequest acquire;acquire.engine_context=context;
  acquire.exact_transaction_uuid=context->transaction_uuid.canonical;
  acquire.variable_frame_selector.version=1;
  acquire.variable_frame_selector.public_coordination_uuid=begun.snapshot.public_coordination_uuid;
  acquire.variable_frame_selector.operation_uuid=operation;
  acquire.variable_frame_selector.expected_coordinator_generation=begun.snapshot.coordinator_generation;
  Live live;sb_engine_result_t result=nullptr;
  const auto acquire_status=bridge::AcquireStatementContextReceipt(session.session,&acquire,&live.receipt,
      &live.view,&result);if(acquire_status!=SB_ENGINE_STATUS_OK){std::cerr<<"variable-acquire:"<<sb_engine_status_name(acquire_status);if(result){sb_engine_diagnostic_set_view_t d{};if(sb_engine_result_diagnostics(result,&d)==SB_ENGINE_STATUS_OK&&d.diagnostic_count)std::cerr<<':'<<std::string(d.diagnostics[0].message_key.data,d.diagnostics[0].message_key.size_bytes);}std::cerr<<'\n';}
  Require(acquire_status==SB_ENGINE_STATUS_OK,"variable receipt acquire failed");
  if(result)(void)sb_engine_result_release(result);
  auto mapping=begun.snapshot.mappings.front();
  runtime::SblrVariableAssignmentRequestV1 assignment;
  assignment.preliminary_receipt_uuid=RawUuid(live.view.receipt_uuid);
  assignment.public_coordination_uuid=RawUuid(begun.snapshot.public_coordination_uuid);
  assignment.operation_uuid=RawUuid(operation);assignment.scope_uuid=RawUuid(begun.snapshot.scope_uuid);
  assignment.scope_generation=begun.snapshot.scope_generation;
  assignment.frame_uuid=RawUuid(begun.snapshot.frame_uuid);
  assignment.frame_generation=begun.snapshot.frame_generation;
  assignment.registry_snapshot_uuid=RawUuid(begun.snapshot.registry_snapshot_uuid);
  assignment.registry_generation=begun.snapshot.registry_generation;
  runtime::SblrVariableAssignmentRecordV1 value;value.assignment_occurrence_id=1;
  value.variable_ordinal=0;value.variable_descriptor_uuid=RawUuid(mapping.descriptor.variable_descriptor_uuid);
  value.variable_descriptor_generation=mapping.descriptor.variable_descriptor_generation;
  value.expected_value_generation=mapping.descriptor.value_generation;
  value.datatype_descriptor_uuid=RawUuid(mapping.descriptor.datatype_descriptor_uuid);
  value.datatype_descriptor_generation=mapping.descriptor.datatype_descriptor_generation;
  value.value_state=1;value.canonical_value_bytes={1,0,0,0,0,0,0,0};assignment.assignments.push_back(value);
  auto sbvy=runtime::EncodeSblrVariableAssignmentRequestV1(&assignment);Bytes sbvw;result=nullptr;
  Require(bridge::AssignStatementVariableValuesV1(live.receipt,sbvy,&sbvw,&result)==SB_ENGINE_STATUS_OK,
      "variable assignment failed");if(result)(void)sb_engine_result_release(result);
  runtime::SblrVariableAssignmentResultV1 assigned;std::string detail;
  Require(runtime::DecodeSblrVariableAssignmentResultV1(sbvw.data(),sbvw.size(),&assigned,&detail),
      "variable assignment result invalid");mapping.descriptor.value_generation=assigned.results[0].new_value_generation;
  mapping.descriptor.registry_generation=assigned.results[0].decision_evidence_generation;
  mapping.descriptor.value_state=api::SblrVariableValueState::value;
  runtime::SblrVariableDemandV1 demand;demand.occurrence_id=1;demand.variable_ordinal=0;
  demand.parent_operand_ordinal=1;demand.scope_uuid=assignment.scope_uuid;
  std::vector<runtime::SblrVariableDemandV1> demands{demand};
  runtime::SblrVariableMappingV1 projected;projected.occurrence_id=1;projected.variable_ordinal=0;
  projected.mutability=1;projected.value_state=1;projected.variable_descriptor_uuid=value.variable_descriptor_uuid;
  projected.variable_descriptor_generation=value.variable_descriptor_generation;
  projected.datatype_descriptor_uuid=value.datatype_descriptor_uuid;
  projected.datatype_descriptor_generation=value.datatype_descriptor_generation;
  projected.datatype_type_uuid=RawUuid(mapping.datatype_type_uuid);
  projected.value_generation=mapping.descriptor.value_generation;
  runtime::SblrVariableNodeV1 node;node.node_id=7;node.parent_operand_ordinal=1;
  node.scope_uuid=assignment.scope_uuid;node.scope_generation=assignment.scope_generation;
  node.frame_uuid=assignment.frame_uuid;node.frame_generation=assignment.frame_generation;
  node.variable_descriptor_uuid=value.variable_descriptor_uuid;
  node.variable_descriptor_generation=value.variable_descriptor_generation;
  node.datatype_descriptor_uuid=value.datatype_descriptor_uuid;
  node.datatype_descriptor_generation=value.datatype_descriptor_generation;
  node.value_generation=projected.value_generation;node.value_state_policy=1;
  runtime::SblrVariableNodeTableV1 table;table.nodes.push_back(node);
  const auto sbvn=runtime::EncodeSblrVariableNodeTableV1(table);
  runtime::SblrVariableFinalizeRequestV1 finalize;finalize.preliminary_receipt_uuid=assignment.preliminary_receipt_uuid;
  finalize.scope_uuid=assignment.scope_uuid;finalize.scope_generation=assignment.scope_generation;
  finalize.frame_uuid=assignment.frame_uuid;finalize.frame_generation=assignment.frame_generation;
  finalize.registry_generation=assigned.new_registry_generation;
  finalize.demand_sha256=runtime::ComputeSblrVariableDemandSha256V1(demands);
  finalize.mapping_sha256=runtime::ComputeSblrVariableMappingSha256V1({projected});
  finalize.sbvn_sha256=DomainHash("ScratchBird.SblrVariableNodeTable.V1",sbvn);
  finalize.canonical_sbvn=sbvn;auto sbvf=runtime::EncodeSblrVariableFinalizeRequestV1(finalize);
  Bytes sbva;result=nullptr;Require(bridge::FinalizeStatementVariableBindingV1(
      live.receipt,sbvf,&sbva,&result)==SB_ENGINE_STATUS_OK,"variable finalize failed");
  if(result)(void)sb_engine_result_release(result);runtime::SblrVariableAdmissionV1 admission;
  Require(runtime::DecodeSblrVariableAdmissionV1(sbva.data(),sbva.size(),&admission,&detail),
      "variable admission invalid");runtime::SblrVariableExecutionBindingV1 execution;
  execution.execution_uuid=RawUuid(live.view.statement_uuid);execution.statement_receipt_uuid=assignment.preliminary_receipt_uuid;
  execution.variable_final_receipt_uuid=admission.final_receipt_uuid;execution.admission_token_uuid=admission.admission_token_uuid;
  execution.scope_uuid=admission.scope_uuid;execution.scope_generation=admission.scope_generation;
  execution.frame_uuid=admission.frame_uuid;execution.frame_generation=admission.frame_generation;
  execution.registry_snapshot_uuid=admission.registry_snapshot_uuid;execution.registry_generation=admission.registry_generation;
  execution.executor_availability_generation=admission.executor_availability_generation;execution.binding_sha256=admission.binding_sha256;
  live.sbve=runtime::EncodeSblrVariableExecutionBindingV1(execution);
  const auto parser_uuid=Text(NewUuid(platform::UuidKind::object,9362));live.parser_uuid=parser_uuid;
  auto member=sblr::MakeSblrEnvelope("query.execute","SBLR_QUERY_EXECUTE","ia01.variable.cancel.query");
  member.opcode_code=0x1207;member.result_shape="query_execute_result";member.diagnostic_shape="diagnostic_vector";
  member.parser_package_uuid=parser_uuid;member.registry_snapshot_uuid=live.view.catalog_epoch_uuid;member.parser_resolved_names_to_uuids=true;
  sblr::SblrOperand carrier;carrier.ordinal=1;carrier.type="expression.variable_node_table.v1";carrier.name="variable_nodes";
  carrier.value_kind=sblr::SblrValueKind::variable_node_table;carrier.value_body=sbvn;member.operands.push_back(carrier);
  runtime::SblrVariableNodeReferenceV1 reference;reference.occurrence_ordinal=1;reference.node_id=7;
  reference.table_sha256=finalize.sbvn_sha256;reference.scope_uuid=node.scope_uuid;reference.scope_generation=node.scope_generation;
  reference.frame_uuid=node.frame_uuid;reference.frame_generation=node.frame_generation;
  reference.variable_descriptor_uuid=node.variable_descriptor_uuid;reference.variable_descriptor_generation=node.variable_descriptor_generation;
  reference.value_generation=node.value_generation;sblr::SblrOperand ref;ref.ordinal=2;ref.type="relational_expression_v1";ref.name="7";
  ref.value_kind=sblr::SblrValueKind::variable_node_ref;ref.value_body=runtime::EncodeSblrVariableNodeReferenceV1(reference);member.operands.push_back(ref);
  Require(sblr::ValidateSblrEnvelope(member).ok,"variable member invalid");
  const auto package=RawUuid(live.view.bound_ast_uuid);sblr::SblrOpcodeStream package_stream;
  package_stream.package_descriptor_uuid=live.view.bound_ast_uuid;package_stream.registry_snapshot_uuid=live.view.catalog_epoch_uuid;
  package_stream.operations={Frame(true,parser_uuid,live.view.catalog_epoch_uuid,package),std::move(member),Frame(false,parser_uuid,live.view.catalog_epoch_uuid,package)};
  const auto stream=sblr::EncodeSblrOpcodeStream(package_stream);Require(!stream.empty(),"variable SBOS invalid");
  auto base=BuildSubmission(fixture,live.view,parser_uuid);auto dc=wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(base.container.data()),base.container.size());dc.container.operation_payload=stream;
  const auto outer=wire::EncodeSblrContainer(dc.container);auto de=wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(base.ingress.data()),base.ingress.size());
  de.envelope.fields[5]={1};U64(&de.envelope.fields[5],stream.size());de.envelope.fields[5].insert(de.envelope.fields[5].end(),stream.begin(),stream.end());
  de.envelope.fields[7]={1};U32(&de.envelope.fields[7],wire::SblrCrc32c(stream.data(),stream.size()));de.envelope.fields[8]=V64(stream.size());
  const auto ingress=wire::EncodeSblrExecutionEnvelopeV1(de.envelope);
  live.submission={{outer.begin(),outer.end()},{ingress.begin(),ingress.end()},stream};return live;
}
} // namespace variable_fixture
