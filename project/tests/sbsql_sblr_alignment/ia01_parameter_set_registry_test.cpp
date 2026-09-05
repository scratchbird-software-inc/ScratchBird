// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_parameter_set_registry.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
[[noreturn]] void Fail(const char* text) { std::cerr << text << '\n'; std::exit(EXIT_FAILURE); }
void Require(bool value, const char* text) { if (!value) Fail(text); }
std::string Id(UuidKind kind, std::uint64_t salt) {
  static std::uint64_t next=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  (void)salt;
  const auto value=uuid::GenerateEngineIdentityV7(kind,++next);
  if(!value.ok()){std::cerr<<"UUID generation failed kind="<<static_cast<unsigned>(kind)<<" salt="<<salt<<'\n';std::exit(EXIT_FAILURE);} return uuid::UuidToString(value.value.value);
}
struct Fixture {
  std::string base; std::string database_uuid;
  std::vector<std::string> stores;
  ~Fixture(){std::error_code ignored;for(const auto& path:stores)std::filesystem::remove(path,ignored);}
  std::string Store(const std::string& descriptor) {
    auto path=base+".sb.sblr_parameter_set."+descriptor+".v1";stores.push_back(path);return path;
  }
  std::string BindStore(const std::string& descriptor) {
    auto path=base+".sb.sblr_parameter_bind."+descriptor+".v1";stores.push_back(path);return path;
  }
};
Fixture MakeFixture(std::uint64_t salt) {
  Fixture f;f.base=(std::filesystem::temp_directory_path()/("sb_parameter_registry_"+std::to_string(salt)+"_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();f.database_uuid=Id(UuidKind::database,salt);return f;
}
api::EngineRequestContext Context(const Fixture& f,bool authority=true) {
  api::EngineRequestContext c;c.database_path=f.base;c.database_uuid.canonical=f.database_uuid;c.session_uuid.canonical=Id(UuidKind::object,100);c.catalog_generation_id=11;c.security_epoch=12;c.resource_epoch=13;c.security_context_present=true;if(authority){c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_statement_context_receipt");c.trace_tags.push_back("right:SBLR_PARAMETER_SET_ADMIN");}return c;
}
api::SblrParameterSetIssueRequest Request(std::uint64_t salt,bool dynamic=false) {
  api::SblrParameterSetIssueRequest r;r.statement_receipt_uuid=Id(UuidKind::object,salt+1);r.execution_uuid=Id(UuidKind::object,salt+2);r.batch_uuid=Id(UuidKind::object,salt+3);r.batch_generation=4;
  if(dynamic){r.dynamic_package_uuid=Id(UuidKind::object,salt+4);r.dynamic_generation=5;}else{r.prepared_statement_uuid=Id(UuidKind::object,salt+4);r.prepared_generation=5;}
  r.slots.push_back({Id(UuidKind::object,salt+5),7,api::SblrParameterDirection::in,false});
  r.slots.push_back({Id(UuidKind::object,salt+6),8,api::SblrParameterDirection::inout,true});r.reason_code=dynamic?"test.dynamic.batch.issue":"test.prepared.batch.issue";return r;
}
std::string Sha256(const std::vector<std::uint8_t>& bytes) {
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);
  Require(digest.ok(),"value hash failed");
  return "sha256:"+scratchbird::core::hash::HexLower(digest.digest);
}
api::SblrParameterBindPublicationRequest BindRequest(
    const api::EngineRequestContext& context,
    const api::SblrParameterSetSnapshot& set) {
  api::SblrParameterBindPublicationRequest request;
  request.statement_receipt_uuid=Id(UuidKind::object,5001);
  request.execution_uuid=Id(UuidKind::object,5002);
  request.prepared_statement_uuid=set.prepared_statement_uuid;
  request.prepared_generation=set.prepared_generation;
  request.parameter_set_descriptor_uuid=set.parameter_set_descriptor_uuid;
  request.parameter_set_generation=set.descriptor_generation;
  request.ordered_slot_table_sha256=set.slots_sha256;
  request.batch_uuid=set.batch_uuid;
  request.batch_generation=set.batch_generation;
  request.dynamic_package_uuid=set.dynamic_package_uuid;
  request.dynamic_generation=set.dynamic_generation;
  request.catalog_snapshot_uuid=Id(UuidKind::object,5003);
  request.catalog_generation=context.catalog_generation_id;
  request.security_epoch=context.security_epoch;
  request.resource_epoch=context.resource_epoch;
  request.mga_snapshot_uuid=Id(UuidKind::object,5004);
  request.executor_availability_generation=1;
  request.canonical_value_vector={0x53,0x42,0x50,0x56,1,2,3,4};
  request.value_vector_sha256=Sha256(request.canonical_value_vector);
  return request;
}
int main(){
  Fixture fixture=MakeFixture(1);auto context=Context(fixture);auto request=Request(1000);
  auto denied=api::IssueSblrParameterSet(Context(fixture,false),request);Require(!denied.ok&&denied.diagnostic.code=="SECURITY.ACCESS_DENIED","unauthorized issue admitted");
  auto issued=api::IssueSblrParameterSet(context,request);Require(issued.ok&&issued.snapshot.snapshot_generation==1&&issued.snapshot.descriptor_generation==1&&issued.snapshot.slots.size()==2&&issued.snapshot.slots[0].slot_uuid!=issued.snapshot.slots[1].slot_uuid,"engine-issued immutable descriptor missing");fixture.Store(issued.snapshot.parameter_set_descriptor_uuid);
  auto restarted=api::LoadSblrParameterSet(context,issued.snapshot.parameter_set_descriptor_uuid);Require(restarted.ok&&restarted.snapshot.decision_evidence_sha256==issued.snapshot.decision_evidence_sha256&&restarted.snapshot.slots_sha256==issued.snapshot.slots_sha256,"restart did not recover exact descriptor");
  const auto bind_request=BindRequest(context,issued.snapshot);
  auto bound=api::PublishSblrParameterBinding(context,issued.snapshot,bind_request);Require(bound.ok&&!bound.replayed&&!bound.snapshot.bind_evidence_uuid.empty(),"durable parameter binding failed");fixture.BindStore(issued.snapshot.parameter_set_descriptor_uuid);
  auto replay=api::PublishSblrParameterBinding(context,issued.snapshot,bind_request);Require(replay.ok&&replay.replayed&&replay.snapshot.bind_evidence_uuid==bound.snapshot.bind_evidence_uuid&&replay.snapshot.publication_evidence_sha256==bound.snapshot.publication_evidence_sha256,"exact parameter binding replay changed evidence");
  auto changed_bind=bind_request;changed_bind.canonical_value_vector.back()^=1;changed_bind.value_vector_sha256=Sha256(changed_bind.canonical_value_vector);auto conflict=api::PublishSblrParameterBinding(context,issued.snapshot,changed_bind);Require(!conflict.ok&&conflict.diagnostic.code=="MGA.TRANSACTION.STALE","changed parameter value overwrote durable binding");
  auto loaded_bind=api::LoadSblrParameterBinding(context,issued.snapshot.parameter_set_descriptor_uuid);Require(loaded_bind.ok&&loaded_bind.snapshot.canonical_value_vector==bind_request.canonical_value_vector&&loaded_bind.snapshot.bind_evidence_uuid==bound.snapshot.bind_evidence_uuid,"restart did not recover exact parameter binding");

  Fixture cancellation_fixture=MakeFixture(4);auto cancellation_context=Context(cancellation_fixture);auto cancellation_issue=api::IssueSblrParameterSet(cancellation_context,Request(5000));Require(cancellation_issue.ok,"cancellation fixture issue failed");cancellation_fixture.Store(cancellation_issue.snapshot.parameter_set_descriptor_uuid);const auto cancellation_bind_request=BindRequest(cancellation_context,cancellation_issue.snapshot);const auto cancellation_bind_path=cancellation_fixture.BindStore(cancellation_issue.snapshot.parameter_set_descriptor_uuid);cancellation_context.query_cancellation_requested=[] { return true; };auto cancelled_bind=api::PublishSblrParameterBinding(cancellation_context,cancellation_issue.snapshot,cancellation_bind_request);Require(!cancelled_bind.ok&&cancelled_bind.diagnostic.code=="PROCESS.CANCELLED"&&cancelled_bind.diagnostic.message_key=="sblr.parameter_bind.cancelled"&&cancelled_bind.diagnostic.detail=="binding was cancelled before durable publication","pre-publication cancellation did not fail closed");Require(!std::filesystem::exists(cancellation_bind_path),"cancelled parameter binding published durable state");cancellation_context.query_cancellation_requested=[] { return false; };auto post_cancel_bind=api::PublishSblrParameterBinding(cancellation_context,cancellation_issue.snapshot,cancellation_bind_request);Require(post_cancel_bind.ok&&!post_cancel_bind.replayed,"parameter binding did not recover after pre-publication cancellation");cancellation_context.query_cancellation_requested=[] { return true; };auto post_publication_replay=api::PublishSblrParameterBinding(cancellation_context,cancellation_issue.snapshot,cancellation_bind_request);Require(post_publication_replay.ok&&post_publication_replay.replayed&&post_publication_replay.snapshot.bind_evidence_uuid==post_cancel_bind.snapshot.bind_evidence_uuid&&post_publication_replay.snapshot.publication_evidence_sha256==post_cancel_bind.snapshot.publication_evidence_sha256,"post-publication cancellation retracted or changed the exact binding");
  api::SblrParameterSetSnapshot observed;auto diagnostic=api::RevalidateSblrParameterSet(context,issued.snapshot,request.statement_receipt_uuid,request.execution_uuid,request.prepared_statement_uuid,request.prepared_generation,request.batch_uuid,request.batch_generation,"",0,&observed);Require(diagnostic.code=="OK","valid prepared/batch binding refused");
  diagnostic=api::RevalidateSblrParameterSet(context,issued.snapshot,request.statement_receipt_uuid,request.execution_uuid,request.prepared_statement_uuid,request.prepared_generation+1,request.batch_uuid,request.batch_generation,"",0,nullptr);Require(diagnostic.code=="SBLR.PARAMETER.STALE","prepared generation drift admitted");
  auto stale=api::InvalidateSblrParameterSet(context,issued.snapshot.parameter_set_descriptor_uuid,Id(UuidKind::object,9000),1,"test.stale");Require(!stale.ok&&stale.diagnostic.code=="SBLR.PARAMETER.STALE","stale compare admitted");
  auto revoked=api::InvalidateSblrParameterSet(context,issued.snapshot.parameter_set_descriptor_uuid,issued.snapshot.snapshot_uuid,issued.snapshot.snapshot_generation,"test.prepared.invalidate");Require(revoked.ok&&revoked.snapshot.snapshot_generation==2&&revoked.snapshot.descriptor_generation==2&&revoked.snapshot.state==api::SblrParameterSetState::revoked,"durable invalidation failed");
  diagnostic=api::RevalidateSblrParameterSet(context,issued.snapshot,request.statement_receipt_uuid,request.execution_uuid,request.prepared_statement_uuid,request.prepared_generation,request.batch_uuid,request.batch_generation,"",0,nullptr);Require(diagnostic.code=="SBLR.PARAMETER.STALE","revoked set admitted");

  auto dynamic_request=Request(2000,true);auto dynamic=api::IssueSblrParameterSet(context,dynamic_request);Require(dynamic.ok,"dynamic/batch issue refused");fixture.Store(dynamic.snapshot.parameter_set_descriptor_uuid);diagnostic=api::RevalidateSblrParameterSet(context,dynamic.snapshot,dynamic_request.statement_receipt_uuid,dynamic_request.execution_uuid,"",0,dynamic_request.batch_uuid,dynamic_request.batch_generation,dynamic_request.dynamic_package_uuid,dynamic_request.dynamic_generation,nullptr);Require(diagnostic.code=="OK","dynamic generation binding refused");
  auto contradictory_request=dynamic_request;contradictory_request.prepared_statement_uuid=Id(UuidKind::object,2200);contradictory_request.prepared_generation=1;auto contradictory_identity=api::IssueSblrParameterSet(context,contradictory_request);Require(!contradictory_identity.ok&&contradictory_identity.diagnostic.code=="SBLR.OPERAND_INVALID","prepared/dynamic identity contradiction admitted");
  Require(api::BeginSblrParameterSetRegistryRecovery(context).code=="OK","startup recovery boundary failed");auto recovered_metadata=api::LoadSblrParameterSet(context,dynamic.snapshot.parameter_set_descriptor_uuid);Require(recovered_metadata.ok&&recovered_metadata.snapshot.slots_sha256==dynamic.snapshot.slots_sha256,"prepared/dynamic descriptor metadata did not recover");diagnostic=api::RevalidateSblrParameterSet(context,dynamic.snapshot,dynamic_request.statement_receipt_uuid,dynamic_request.execution_uuid,"",0,dynamic_request.batch_uuid,dynamic_request.batch_generation,dynamic_request.dynamic_package_uuid,dynamic_request.dynamic_generation,nullptr);Require(diagnostic.code=="SBLR.PARAMETER.STALE","execution receipt authority recovered across restart");

  Fixture torn_fixture=MakeFixture(2);auto torn_context=Context(torn_fixture);auto torn_issue=api::IssueSblrParameterSet(torn_context,Request(3000));Require(torn_issue.ok,"torn fixture issue failed");const auto torn_path=torn_fixture.Store(torn_issue.snapshot.parameter_set_descriptor_uuid);{std::ifstream in(torn_path);std::string evidence;std::getline(in,evidence);std::ofstream out(torn_path,std::ios::app);out<<evidence<<'\n';}auto torn=api::LoadSblrParameterSet(torn_context,torn_issue.snapshot.parameter_set_descriptor_uuid);Require(!torn.ok&&torn.diagnostic.code=="SBLR.PARAMETER.STALE","torn evidence recovered");
  Fixture corrupt_fixture=MakeFixture(3);auto corrupt_context=Context(corrupt_fixture);auto corrupt_issue=api::IssueSblrParameterSet(corrupt_context,Request(4000));Require(corrupt_issue.ok,"corrupt fixture issue failed");const auto corrupt_path=corrupt_fixture.Store(corrupt_issue.snapshot.parameter_set_descriptor_uuid);{std::ifstream in(corrupt_path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(in)),{});auto position=bytes.find("sha256:");Require(position!=std::string::npos,"evidence hash absent");bytes[position+7]=bytes[position+7]=='0'?'1':'0';std::ofstream out(corrupt_path,std::ios::binary|std::ios::trunc);out<<bytes;}auto corrupt=api::LoadSblrParameterSet(corrupt_context,corrupt_issue.snapshot.parameter_set_descriptor_uuid);Require(!corrupt.ok&&corrupt.diagnostic.code=="SBLR.PARAMETER.STALE","corrupt evidence recovered");return EXIT_SUCCESS;
}
