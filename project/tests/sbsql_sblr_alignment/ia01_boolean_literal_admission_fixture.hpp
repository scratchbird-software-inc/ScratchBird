// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ia01_literal_live_fixture.hpp"
#include "engine/sblr/canonical_relational_expression.hpp"
#include "core/datatypes/datatype_type_codec_identity_v3.hpp"

inline void CheckAdmittedBooleanLiteralExecution(
    const bridge::StatementContextReceiptView& view,
    const literal_fixture::runtime::SblrLiteralStatementDescriptorProfileV1& profile,
    const literal_fixture::runtime::SblrExpressionLiteralNodeV1& node) {
  namespace rt=literal_fixture::runtime;
  const auto identity=scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV3(
      view.literal_catalog_snapshot_uuid,view.literal_catalog_generation,
      view.literal_registry_generation,api::EngineUuid{profile.descriptor_uuid},
      profile.descriptor_generation);
  Require(identity.ok,"BOOLEAN admitted execution registry lookup failed");
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id=1;descriptor.descriptor_uuid.bytes=profile.profile_uuid;
  descriptor.type_uuid=identity.row.legacy_fields.type_uuid;
  descriptor.nullability=profile.nullable?api::RelationalNullability::kNullable:
                                         api::RelationalNullability::kNonNull;
  descriptor.datatype_identity_authoritative=true;
  descriptor.descriptor_generation=profile.descriptor_generation;
  descriptor.type_generation=identity.row.legacy_fields.type_generation;
  descriptor.codec_id=profile.codec_id;descriptor.codec_version=profile.codec_version;
  descriptor.codec_generation=profile.codec_generation;
  descriptor.statement_receipt_uuid=view.receipt_uuid;
  descriptor.datatype_catalog_snapshot_uuid=view.literal_catalog_snapshot_uuid;
  descriptor.datatype_catalog_generation=view.literal_catalog_generation;
  descriptor.datatype_registry_generation=view.literal_registry_generation;
  api::RelationalExpressionRecord expression;
  expression.expression_id=node.node_id;
  expression.result_descriptor_id=1;
  expression.expression_kind=api::RelationalExpressionKind::kLiteral;
  expression.literal_kind=node.value_state==1?api::RelationalLiteralKind::kNull:
                                             api::RelationalLiteralKind::kBoolean;
  api::RelationalExpressionRecord::LiteralTypedValueV1 typed;
  using HashProfile=decltype(typed)::HashProfile;
  typed.descriptor_uuid=descriptor.descriptor_uuid;
  typed.descriptor_generation=node.descriptor_generation;
  typed.value_state=node.value_state==1?"null":"value";
  typed.canonical_value_bytes=node.literal_body;
  typed.hash_profile=HashProfile::state_and_canonical_bytes;
  const auto hash=rt::ComputeSblrLiteralStateSha256V1(node.value_state,
      node.literal_body.data(),node.literal_body.size());
  Require(hash.has_value(),"BOOLEAN admitted value/state hash unavailable");
  typed.canonical_value_sha256=*hash;
  expression.literal_typed_value_v1=typed;
  api::TypedRelationalDag dag;dag.descriptors={descriptor};dag.expressions={expression};
  rt::CanonicalRelationalExpressionRuntime runtime(dag);
  api::EngineTypedValue value;std::string detail;
  if(!runtime.Evaluate(node.node_id,"boolean",&value,&detail)){
    std::cerr<<"BOOLEAN admitted evaluation: "<<detail<<'\n';
    Require(false,"BOOLEAN admitted typed literal did not execute");
  }
  Require(value.descriptor.descriptor_uuid==descriptor.descriptor_uuid&&
          value.descriptor.type_uuid.bytes==profile.type_uuid&&
          value.descriptor.datatype_descriptor_uuid==identity.row.legacy_fields.descriptor_uuid&&
          value.descriptor.datatype_cohort==api::EngineDatatypeCohort{
              view.literal_catalog_snapshot_uuid,view.literal_catalog_generation,view.literal_registry_generation}&&
          value.encoded_value.empty()&&value.binary_value==node.literal_body&&
          value.isSqlNull()==(node.value_state==1),
          "BOOLEAN typed literal lost actual occurrence/cohort/native value state");
  for(unsigned mutation=0;mutation<12;++mutation){
    auto changed=dag;
    auto& e=changed.expressions[0];auto& t=*e.literal_typed_value_v1;
    if(mutation==0)t.value_state="invalid";
    if(mutation==1)t.value_state=node.value_state==1?"value":"null";
    if(mutation==2)t.canonical_value_sha256[0]^=1;
    if(mutation==3)t.descriptor_uuid=view.receipt_uuid;
    if(mutation==4)++t.descriptor_generation;
    if(mutation==5){t.canonical_value_bytes={2};
      t.canonical_value_sha256=*rt::ComputeSblrLiteralStateSha256V1(0,t.canonical_value_bytes.data(),1);}
    if(mutation==6)e.literal_kind=node.value_state==1?api::RelationalLiteralKind::kBoolean:
                                                               api::RelationalLiteralKind::kNull;
    if(mutation==7){
      t.hash_profile=HashProfile::canonical_bytes;
      const auto raw=scratchbird::core::hash::ComputeSha256Digest(t.canonical_value_bytes);
      Require(raw.ok(),"BOOLEAN downgrade fixture raw hash unavailable");
      t.canonical_value_sha256=raw.digest;
    }
    if(mutation==8)t.hash_profile=static_cast<HashProfile>(255);
    if(mutation==9)e.literal_kind=api::RelationalLiteralKind::kNumeric;
    if(mutation==10){
      if(node.value_state!=1)continue;
      changed.descriptors[0].nullability=api::RelationalNullability::kNonNull;
    }
    if(mutation==11)t.hash_profile=HashProfile::canonical_bytes;
    rt::CanonicalRelationalExpressionRuntime invalid(changed);
    api::EngineTypedValue refused;
    Require(!invalid.Evaluate(node.node_id,"boolean",&refused,&detail)&&
            refused.state==api::EngineValueState::error&&refused.binary_value.empty()&&
            refused.encoded_value.empty()&&!detail.empty(),
            "BOOLEAN malformed typed execution published a value or bypassed NULL checks");
  }
}

// These tests use the real session/MGA owner and acquire a fresh retained
// statement for each case. No UUID, cohort, generation or profile is supplied
// by a synthetic descriptor fixture.
inline void CheckBooleanLiteralAdmission(
    sb_engine_session_t session,const api::EngineRequestContext& context) {
  namespace rt=scratchbird::engine::sblr;
  const auto release_result=[](sb_engine_result_t& result){
    if(result)(void)sb_engine_result_release(result);
    result=nullptr;
  };
  for(unsigned scenario=0;scenario<11;++scenario){
    bridge::StatementContextAcquireRequest acquire;
    acquire.engine_context=&context;
    acquire.exact_transaction_uuid=context.transaction_uuid;
    bridge::StatementContextReceiptHandle receipt;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result=nullptr;
    Require(bridge::AcquireStatementContextReceipt(
        session,&acquire,&receipt,&view,&result)==SB_ENGINE_STATUS_OK,
        "BOOLEAN real statement acquisition failed");
    release_result(result);
    rt::SblrLiteralPrebindRequestV1 request;
    request.preliminary_receipt_uuid=RawUuid(view.receipt_uuid);
    request.catalog_snapshot_uuid=RawUuid(view.literal_catalog_snapshot_uuid);
    request.catalog_generation=view.literal_catalog_generation;
    request.security_epoch=view.security_epoch;
    request.resource_epoch=view.resource_epoch;
    request.mga_snapshot_uuid=RawUuid(view.statement_snapshot_uuid);
    rt::SblrLiteralDemandV1 demand;
    demand.occurrence_id=1;demand.lexical_class=4;demand.context_class=1;
    demand.nullable=scenario==2||scenario==3||scenario==7;
    const bool is_null=scenario==2||scenario==4||scenario==7;
    Bytes body=is_null?Bytes{}:Bytes{static_cast<std::uint8_t>(scenario!=1)};
    const auto lexical=rt::ComputeSblrBooleanLiteralLexicalSha256V1(
        is_null?1:0,body.data(),body.size());
    Require(lexical.has_value(),"BOOLEAN canonical lexical fixture invalid");
    demand.lexical_sha256=*lexical;
    request.demands={demand};
    request.demand_sha256=rt::ComputeSblrLiteralDemandSequenceSha256V1(request.demands);
    Bytes sblq;
    if(scenario==10){
      // Unsupported context must not publish a prefix or consume negotiation.
      auto unsupported=request;
      unsupported.demands[0].context_class=2;
      unsupported.demand_sha256=rt::ComputeSblrLiteralDemandSequenceSha256V1(unsupported.demands);
      Require(bridge::NegotiateStatementLiteralDescriptorsV1(receipt,
          rt::EncodeSblrLiteralPrebindRequestV1(unsupported),&sblq,&result)==
              SB_ENGINE_STATUS_UNSUPPORTED&&sblq.empty(),
          "BOOLEAN unsupported context was admitted or published a prefix");
      release_result(result);
    }
    const auto sbln=rt::EncodeSblrLiteralPrebindRequestV1(request);
    const auto negotiated=bridge::NegotiateStatementLiteralDescriptorsV1(
        receipt,sbln,&sblq,&result);
    if(negotiated!=SB_ENGINE_STATUS_OK){
      std::cerr<<"BOOLEAN negotiation scenario "<<scenario<<" status "
               <<sb_engine_status_name(negotiated);
      sb_engine_diagnostic_set_view_t diagnostics{};
      if(result&&sb_engine_result_diagnostics(result,&diagnostics)==SB_ENGINE_STATUS_OK){
        for(std::size_t i=0;i<diagnostics.diagnostic_count;++i){
          const auto& d=diagnostics.diagnostics[i];
          std::cerr<<':'<<std::string_view(d.symbolic_code.data,d.symbolic_code.size_bytes)
                   <<':'<<std::string_view(d.message_key.data,d.message_key.size_bytes);
        }
      }
      std::cerr<<'\n';
    }
    Require(negotiated==SB_ENGINE_STATUS_OK,
        "BOOLEAN real descriptor negotiation failed");
    release_result(result);
    Require(sblq.size()>=172&&scratchbird::engine::SblrReadU32(sblq.data()+120)==1&&
            scratchbird::engine::SblrReadU64(sblq.data()+160)==1&&
            scratchbird::engine::SblrReadU32(sblq.data()+168)==sblq.size()-172,
            "BOOLEAN negotiated mapping extent invalid");
    const auto profile=rt::DecodeSblrLiteralDescriptorProfileV1(
        sblq.data()+172,sblq.size()-172);
    Require(profile.ok&&profile.profile.nullable==demand.nullable&&
            profile.profile.statement_receipt_uuid==request.preliminary_receipt_uuid&&
            profile.profile.catalog_snapshot_uuid==request.catalog_snapshot_uuid&&
            profile.profile.catalog_generation==request.catalog_generation&&
            profile.profile.codec_id=="datatype.boolean.u8.v1"&&
            profile.profile.profile_binding_sha256==
                rt::ComputeSblrLiteralDescriptorProfileBindingV1(
                    profile.profile,view.security_epoch,view.resource_epoch),
            "BOOLEAN receipt-owned profile or nullable binding invalid");

    rt::SblrExpressionLiteralNodeV1 node;
    node.node_id=7;node.parent_operand_ordinal=1;
    node.descriptor_uuid=profile.profile.profile_uuid;
    node.descriptor_generation=profile.profile.descriptor_generation;
    node.value_state=is_null?1:0;node.literal_body=body;
    if(scenario==5)node.literal_body={2};
    if(scenario==6)node.literal_body={0}; // Valid FALSE, wrong TRUE identity.
    rt::SblrExpressionNodeTableV1 table;
    table.nodes={node};
    const auto sbxn=rt::EncodeSblrStatefulExpressionNodeTableV2(table);
    Require(!sbxn.empty(),"BOOLEAN stateful table encoding failed");
    rt::SblrLiteralBoundAstNodeV1 ast;
    ast.node_id=node.node_id;ast.parent_operand_ordinal=1;
    ast.descriptor_uuid=node.descriptor_uuid;
    ast.descriptor_generation=node.descriptor_generation;
    ast.type_uuid=profile.profile.type_uuid;
    ast.profile_uuid=profile.profile.profile_uuid;
    ast.occurrence_id=1;ast.lexical_sha256=demand.lexical_sha256;
    ast.nullable=demand.nullable;
    if(scenario==7)ast.nullable=false;
    if(scenario==8)ast.profile_uuid=request.preliminary_receipt_uuid;
    rt::SblrLiteralBoundAstV1 bound;
    bound.preliminary_receipt_uuid=request.preliminary_receipt_uuid;
    bound.demand_sha256=request.demand_sha256;bound.nodes={ast};
    const auto sbba=rt::EncodeSblrLiteralBoundAstV1(bound);
    Require(!sbba.empty(),"BOOLEAN bound AST encoding failed");
    Bytes sblf{'S','B','L','F'};
    U16(&sblf,1);U16(&sblf,208);
    U32(&sblf,static_cast<std::uint32_t>(208+sbba.size()+sbxn.size()));U32(&sblf,0);
    sblf.insert(sblf.end(),request.preliminary_receipt_uuid.begin(),request.preliminary_receipt_uuid.end());
    sblf.insert(sblf.end(),request.demand_sha256.begin(),request.demand_sha256.end());
    sblf.insert(sblf.end(),sblq.begin()+128,sblq.begin()+160);
    const auto ast_hash=rt::ComputeSblrLiteralBoundAstSha256V1(sbba);
    sblf.insert(sblf.end(),ast_hash.begin(),ast_hash.end());
    const auto table_hash=scratchbird::core::hash::ComputeSha256Digest(sbxn);
    Require(table_hash.ok(),"BOOLEAN table hash unavailable");
    sblf.insert(sblf.end(),table_hash.digest.begin(),table_hash.digest.end());
    U64(&sblf,request.catalog_generation);
    U64(&sblf,request.security_epoch+(scenario==9?1:0));
    U64(&sblf,request.resource_epoch);
    sblf.insert(sblf.end(),request.mga_snapshot_uuid.begin(),request.mga_snapshot_uuid.end());
    U32(&sblf,static_cast<std::uint32_t>(sbba.size()));
    U32(&sblf,static_cast<std::uint32_t>(sbxn.size()));
    sblf.insert(sblf.end(),sbba.begin(),sbba.end());
    sblf.insert(sblf.end(),sbxn.begin(),sbxn.end());
    rt::SblrLiteralFinalizeRequestV1 parsed;
    Require(is_null?!rt::DecodeSblrLiteralFinalizeRequestV1(sblf.data(),sblf.size(),&parsed):
                    rt::DecodeSblrLiteralFinalizeRequestV1(sblf.data(),sblf.size(),&parsed),
            "BOOLEAN old finalize ingress changed its state/version boundary");
    if(is_null)Require(rt::DecodeSblrStatefulLiteralFinalizeRequestV2(
        sblf.data(),sblf.size(),&parsed),"BOOLEAN explicit stateful finalize frame refused");
    Bytes admission{0xff}; // A failure must clear stale caller output.
    const auto status=bridge::FinalizeStatementLiteralBindingV1(
        receipt,sblf,&admission,&result);
    const bool positive=scenario<4||scenario==10;
    const auto wanted=positive?SB_ENGINE_STATUS_OK:
        scenario>=8?SB_ENGINE_STATUS_CONFLICT:SB_ENGINE_STATUS_INVALID_ARGUMENT;
    if(status!=wanted)std::cerr<<"BOOLEAN scenario "<<scenario<<" status "
                              <<sb_engine_status_name(status)<<'\n';
    Require(status==wanted,"BOOLEAN finalization status drifted");
    Require(positive?admission.size()==264:admission.empty(),
            "BOOLEAN finalization published invalid admission/prefix");
    if(positive)CheckAdmittedBooleanLiteralExecution(view,profile.profile,node);
    if(!positive){
      sb_engine_diagnostic_set_view_t diagnostics{};
      Require(result&&sb_engine_result_diagnostics(result,&diagnostics)==SB_ENGINE_STATUS_OK&&
              diagnostics.diagnostic_count==1,"BOOLEAN refusal diagnostic missing");
      const auto& d=diagnostics.diagnostics[0];
      const std::string_view code(d.symbolic_code.data,d.symbolic_code.size_bytes);
      Require(code==(scenario>=8?"DATATYPE.DESCRIPTOR.INVALID":"SBLR.OPERAND_INVALID"),
              "BOOLEAN refusal diagnostic identity drifted");
    }
    release_result(result);
    if(positive){
      Require(bridge::FinalizeStatementLiteralBindingV1(receipt,sblf,&admission,&result)==
                  SB_ENGINE_STATUS_CONFLICT&&admission.empty(),
              "BOOLEAN finalized receipt replay was admitted");
      release_result(result);
    }
    Require(bridge::ReleaseStatementContextReceipt(receipt)==SB_ENGINE_STATUS_OK,
            "BOOLEAN real receipt cleanup failed");
    Require(bridge::FinalizeStatementLiteralBindingV1(receipt,sblf,&admission,&result)==
                SB_ENGINE_STATUS_INVALID_HANDLE&&admission.empty(),
            "BOOLEAN released receipt was accepted");
    release_result(result);
  }
}
