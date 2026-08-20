// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_variable_descriptor_registry.hpp"
#include "uuid.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>

using namespace scratchbird::engine::internal_api;
namespace {
std::string Id(scratchbird::core::platform::UuidKind kind) {
  static auto next=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto v=scratchbird::core::uuid::GenerateEngineIdentityV7(kind,++next);
  assert(v.ok());return scratchbird::core::uuid::UuidToString(v.value.value);
}
EngineRequestContext Context(const std::filesystem::path& base) {
  EngineRequestContext c;c.database_path=base.string();
  c.database_uuid.canonical=Id(scratchbird::core::platform::UuidKind::database);
  c.session_uuid.canonical=Id(scratchbird::core::platform::UuidKind::object);
  c.transaction_uuid.canonical=Id(scratchbird::core::platform::UuidKind::object);
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags={"private_variable_registry","canonical_datatype_value_validated"};return c;
}
}
int main(){
  const auto base=std::filesystem::temp_directory_path()/"sb_variable_descriptor_registry_test";
  const auto store=base.string()+".sb.sblr_variable_registry.v1";std::error_code ec;std::filesystem::remove(store,ec);
  auto c=Context(base);const auto receipt=Id(scratchbird::core::platform::UuidKind::object);
  const auto scope=Id(scratchbird::core::platform::UuidKind::object);
  const auto frame=Id(scratchbird::core::platform::UuidKind::object);
  std::vector<SblrVariableDemand> demands{
    {Id(scratchbird::core::platform::UuidKind::object),7,false,SblrVariableMutability::mutable_value,SblrVariableValueState::value,"one"},
    {Id(scratchbird::core::platform::UuidKind::object),8,true,SblrVariableMutability::immutable,SblrVariableValueState::null_value,{}}};
  auto issued=PublishSblrVariableFrame(c,receipt,scope,3,frame,4,demands);assert(issued.ok&&issued.rows.size()==2);
  assert(issued.rows[0].variable_ordinal==0&&issued.rows[1].variable_ordinal==1);
  assert(issued.rows[0].registry_generation<issued.rows[1].registry_generation);
  auto found=LookupSblrVariable(c,receipt,scope,3,frame,4,issued.rows[0].variable_descriptor_uuid,1,1);assert(found.ok&&found.row.canonical_value_bytes=="one");
  auto assigned=AssignSblrVariable(c,receipt,scope,3,frame,4,issued.rows[0].variable_descriptor_uuid,1,1,SblrVariableValueState::value,"two");assert(assigned.ok&&assigned.row.value_generation==2&&assigned.row.row_identity_sha256==issued.rows[0].row_identity_sha256);
  auto stale=LookupSblrVariable(c,receipt,scope,3,frame,4,issued.rows[0].variable_descriptor_uuid,1,1);assert(!stale.ok&&stale.diagnostic.code=="SBLR.VARIABLE.STALE");
  auto immutable=AssignSblrVariable(c,receipt,scope,3,frame,4,issued.rows[1].variable_descriptor_uuid,1,1,SblrVariableValueState::null_value,{});assert(!immutable.ok&&immutable.diagnostic.code=="SECURITY.ACCESS_DENIED");
  auto wrong=c;wrong.session_uuid.canonical=Id(scratchbird::core::platform::UuidKind::object);auto hidden=LookupSblrVariable(wrong,receipt,scope,3,frame,4,issued.rows[0].variable_descriptor_uuid,1,2);assert(!hidden.ok&&hidden.diagnostic.code=="SECURITY.ACCESS_DENIED");
  auto admin=c;admin.trace_tags={"right:SBLR_VARIABLE_REGISTRY_ADMIN"};assert(RecoverSblrVariableDescriptorRegistry(admin).code=="OK");
  hidden=LookupSblrVariable(c,receipt,scope,3,frame,4,issued.rows[0].variable_descriptor_uuid,1,2);assert(!hidden.ok&&hidden.diagnostic.code=="SECURITY.ACCESS_DENIED");
  auto next=PublishSblrVariableFrame(c,Id(scratchbird::core::platform::UuidKind::object),Id(scratchbird::core::platform::UuidKind::object),5,Id(scratchbird::core::platform::UuidKind::object),6,{demands[0]});assert(next.ok&&next.rows[0].registry_generation>assigned.row.registry_generation);
  assert(RevokeSblrVariableFrame(c,next.rows[0].statement_receipt_uuid,next.rows[0].scope_uuid,5,next.rows[0].frame_uuid,6,"frame.exit").code=="OK");
  const auto batch_receipt=Id(scratchbird::core::platform::UuidKind::object);
  const auto batch_scope=Id(scratchbird::core::platform::UuidKind::object);
  const auto batch_frame=Id(scratchbird::core::platform::UuidKind::object);
  auto second_mutable=demands[0];second_mutable.datatype_descriptor_uuid=Id(scratchbird::core::platform::UuidKind::object);
  auto batch_frame_rows=PublishSblrVariableFrame(c,batch_receipt,batch_scope,7,batch_frame,8,{demands[0],second_mutable});
  assert(batch_frame_rows.ok&&batch_frame_rows.rows.size()==2);
  std::vector<SblrVariableAssignment> batch;
  for(const auto& row:batch_frame_rows.rows)batch.push_back({row.variable_descriptor_uuid,
      row.variable_descriptor_generation,row.value_generation,SblrVariableValueState::value,"batch"});
  auto batch_result=AssignSblrVariableBatch(c,batch_receipt,batch_scope,7,batch_frame,8,batch);
  assert(batch_result.ok&&batch_result.rows.size()==2&&
         batch_result.rows[0].value_generation==2&&batch_result.rows[1].value_generation==2);
  auto duplicate=batch;duplicate[1].variable_descriptor_uuid=duplicate[0].variable_descriptor_uuid;
  auto duplicate_result=AssignSblrVariableBatch(c,batch_receipt,batch_scope,7,batch_frame,8,duplicate);
  assert(!duplicate_result.ok&&duplicate_result.diagnostic.code=="SBLR.OPERAND_INVALID");
  assert(RevokeSblrVariableFrame(c,batch_receipt,batch_scope,7,batch_frame,8,"frame.exit").code=="OK");
  std::filesystem::remove(store,ec);
}
