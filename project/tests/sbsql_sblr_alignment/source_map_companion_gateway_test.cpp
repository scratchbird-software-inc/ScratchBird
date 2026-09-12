// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_sblr_admission_test_helper.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "server/sblr_local_gateway.hpp"
#include "core/hash/hash_digest.hpp"
#include "uuid.hpp"
#include <cstdlib>
#include <iostream>

namespace s = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace fixture = scratchbird::test::sbsql;
namespace {
unsigned checks=0, failures=0;
void Check(bool condition,const char* message) {
  ++checks;if(!condition){++failures;if(failures<20)std::cerr<<message<<'\n';}
}
s::SblrOperationEnvelope Frame(bool begin,const std::string& package) {
  auto frame=fixture::BuildCanonicalEngineSblrEnvelopeForTest(
      begin?"engine.op.package_begin":"engine.op.package_end",
      begin?"SBLR_PACKAGE_BEGIN":"SBLR_PACKAGE_END","source_map.gateway");
  frame.result_shape="void";
  const auto id=scratchbird::core::uuid::ParseUuid(package);
  s::SblrOperand operand;operand.ordinal=1;
  operand.type=begin?"package.header":"package.footer";operand.name="package_descriptor";
  operand.value_kind=s::SblrValueKind::descriptor_ref;
  operand.value_body.assign(id.value.bytes.begin(),id.value.bytes.end());
  frame.operands={operand};return frame;
}
s::SblrOperationEnvelope SourceMap() {
  auto map=fixture::BuildCanonicalEngineSblrEnvelopeForTest(
      "engine.op.source_map","SBLR_SOURCE_MAP","source_map.gateway");
  map.result_shape="void";map.diagnostic_shape="diagnostic_vector";
  s::SblrOperand operand;operand.ordinal=1;operand.type="source_map.vector";
  operand.name="source_map";operand.value_kind=s::SblrValueKind::descriptor_ref;
  operand.value_body.assign(24,0);operand.value_body[0]=1;
  operand.value_body[6]=0x70;operand.value_body[8]=0x80;operand.value_body[16]=1;
  map.operands={operand};return map;
}
s::SblrOpcodeStream Package(bool local,unsigned map_position) {
  auto root=fixture::BuildCanonicalEngineSblrEnvelopeForTest(
      local?"observability.show_version":"cluster.inspect_provider",
      local?"SBLR_OBSERVABILITY_SHOW_VERSION":"SBLR_CLUSTER_INSPECT_PROVIDER",
      "source_map.gateway");
  if(local){
    s::SblrOperand operand;operand.ordinal=1;operand.type="observability_show_version_descriptor";
    operand.name="show_version";operand.value_kind=s::SblrValueKind::observability_show_version_descriptor;
    operand.value_body.assign(64,0);operand.value_body[0]='S';operand.value_body[1]='V';
    operand.value_body[2]='D';operand.value_body[3]='O';operand.value_body[4]=1;
    root.operands={operand};
  }
  s::SblrOpcodeStream stream;stream.package_descriptor_uuid=root.parser_package_uuid;
  stream.registry_snapshot_uuid=root.registry_snapshot_uuid;
  stream.operations={Frame(true,stream.package_descriptor_uuid),root,Frame(false,stream.package_descriptor_uuid)};
  if(map_position)stream.operations.insert(stream.operations.begin()+map_position,SourceMap());
  return stream;
}
server::LocalSblrGatewayRequest Request(const s::SblrOpcodeStream& stream,bool local,unsigned flags) {
  auto root=Package(local,0).operations[1];
  server::LocalSblrGatewayRequest request;
  request.canonical_sbos=s::EncodeSblrOpcodeStream(stream);
  Check(!request.canonical_sbos.empty(),"fixture failed canonical encoding");
  request.root_operation_id=root.operation_id;request.root_opcode=root.opcode;request.root_opcode_code=root.opcode_code;
  request.route_snapshot_uuid=stream.registry_snapshot_uuid;request.route_epoch=7;request.route_generation=9;
  request.security_snapshot_uuid=stream.package_descriptor_uuid;request.security_epoch=11;request.security_observation_generation=13;
  request.route_snapshot_engine_owned=true;request.security_snapshot_engine_owned=true;
  request.cluster_context_active=flags&1;request.cluster_transaction_active=flags&2;request.route_fence_present=flags&4;
  return request;
}
void Refused(const server::LocalSblrGatewayRequest& request) {
  const auto result=server::AdmitLocalNoClusterSblrGateway(request);
  Check(!result.ok&&result.disposition==server::LocalSblrGatewayDisposition::kRefused,
        "malformed/unauthorized companion package passed routing");
}
}
int main(){
  for(bool local:{false,true})for(unsigned position:{0u,1u,2u})for(unsigned flags=0;flags!=8;++flags){
    const auto package=Package(local,position);const auto request=Request(package,local,flags);
    const auto original=request.canonical_sbos;
    const auto decision=server::AdmitLocalNoClusterSblrGateway(request);
    const bool expected=!local||flags==0;
    Check(decision.ok==expected,"source map changed owning command routing");
    Check(request.canonical_sbos==original,"routing rewrote the canonical submission");
    const auto digest=scratchbird::core::hash::ComputeSha256Digest(original);
    Check(digest.ok()&&decision.canonical_payload_sha256==digest.digest,"routing hash omitted companion bytes");
    if(expected){
      Check(decision.disposition==server::LocalSblrGatewayDisposition::kPassThrough,
            "routing manufactured execution success");
      Check(decision.route_epoch==7&&decision.route_generation==9&&decision.security_epoch==11&&
            decision.security_observation_generation==13&&decision.cluster_context_active==request.cluster_context_active&&
            decision.cluster_transaction_active==request.cluster_transaction_active&&decision.route_fence_present==request.route_fence_present,
            "routing changed authoritative observations");
      if(position){const auto plain=s::EncodeSblrOpcodeStream(Package(local,0));
        Check(decision.canonical_payload_sha256!=scratchbird::core::hash::ComputeSha256Digest(plain).digest,
              "companion package reused command-only evidence");}
    }
  }
  for(unsigned position:{1u,2u}){
    const auto package=Package(false,position);
    for(unsigned mutation=0;mutation!=11;++mutation){
      auto bad=package;auto& map=bad.operations[position];auto& operand=map.operands[0];
      if(mutation==0)map.operands.clear();
      if(mutation==1)operand.value_body.resize(16);
      if(mutation==2)operand.value_body[6]=0x40;
      if(mutation==3)operand.value_body[8]=0;
      if(mutation==4)operand.value_body[16]=0;
      if(mutation==5)operand.value_kind=s::SblrValueKind::uuid_ref;
      if(mutation==6)operand.type="other.vector";
      if(mutation==7)operand.name="other";
      if(mutation==8)map.result_shape="rowset";
      if(mutation==9)++map.parser_package_version_patch;
      if(mutation==10){auto duplicate=operand;duplicate.ordinal=2;map.operands.push_back(duplicate);}
      if(mutation==4||mutation==5||mutation==6||mutation==7||mutation==10){
        // These are already rejected by the canonical operand codec. Do not
        // substitute an empty package and call it a gateway validation test.
        Check(s::EncodeSblrOpcodeStream(bad).empty(),"canonical codec admitted invalid source-map operand");
        continue;
      }
      Refused(Request(bad,false,0));
    }
    auto bad=package;bad.operations[3-position]=SourceMap();Refused(Request(bad,false,0));
    bad=package;bad.operations.insert(bad.operations.begin()+1,package.operations[3-position]);
    Refused(Request(bad,false,0));
    auto request=Request(package,false,0);request.security_snapshot_engine_owned=false;Refused(request);
    request=Request(package,false,0);request.route_snapshot_engine_owned=false;Refused(request);
    request=Request(package,false,0);request.root_operation_id="engine.op.source_map";Refused(request);
    request=Request(package,false,0);request.canonical_sbos[70]^=1;Refused(request);
  }
  std::cout<<"source_map_companion_gateway checks="<<checks<<" failures="<<failures
           <<" engine_execution=false parser_ipc_e2e=false\n";
  return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
