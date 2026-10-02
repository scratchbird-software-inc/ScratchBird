// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse actual native creation and independent inventory/history oracles.
// The opaque payload is NOT a registered startup-family manifest or admission.
#define main UnusedManagementBundleMain
#include "../sbsql_sblr_alignment/native_management_control_bundle_test.cpp"
#undef main

namespace {
Bytes ParentRequest() {
  Bytes bytes(16 * 7);
  for (unsigned n=0;n<7;++n) Put(bytes,16*n,Id(24000+n));
  return bytes;
}
void CheckParentHistory(const std::vector<d::NativeFilespaceDevice>& devices,u64 budget) {
  const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),devices,Id(2),budget);
  Check(history.ok()&&history.entries.size()==1,"one actual durable parent request");
  const auto& entry=history.entries.front();
  Check(entry.record.uuid==Id(24000)&&entry.record.request_context_uuid==Id(24001)&&
      entry.record.normalized_request_bytes==ParentRequest()&&
      entry.record.normalized_request_sha256==Sha(ParentRequest()),
      "complete binary parent request survives native publication");
  Check(entry.plan.intent.recovery_profile==2&&!entry.plan.intent.startup_binding,
      "parent persistence invents no child transaction binding");
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),devices,Id(2),budget);
  Check(selected.ok()&&selected.checkpoint_inventory.inventory.next_local_transaction_id==2&&
      selected.checkpoint_inventory.inventory.entries.size()==1&&
      selected.checkpoint_inventory.inventory.entries.front().identity.transaction_uuid.value==Id(5),
      "reopened parent publication allocates no startup transaction");
}
void ParentPublication(unsigned profile) {
  Fixture f(profile); f.budget*=4;
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok(),"actual initial inventory");
  const auto inventory=selected.checkpoint_inventory.inventory;
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);
  Check(zero.ok(),"actual parent bootstrap binding");
  auto record=records::Example(1); record.uuid=Id(24000);record.request_context_uuid=Id(24001);
  record.bootstrap_uuid=zero.record->page_uuid;record.security_snapshot_uuid={};record.generation_guards={};
  record.idempotency_key="startup-parent-source-probe";
  record.normalized_request_bytes=ParentRequest();record.normalized_request_sha256=Sha(record.normalized_request_bytes);
  db::NativePublicationIntent intent{record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
      record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(before.ok(),"actual parent publication base");
  auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(24100),f.budget,&intent);
  Check(held.ok(),"reserve parent independently of child BEGIN");
  const auto published=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,*f.issuer);
  if(!published.ok())std::cerr<<"parent profile="<<profile<<" error="<<int(published.error)<<'\n';
  Check(published.ok()&&published.effects.selected_graph_verified&&published.effects.successful_syncs,
      "parent request physically selected and verified");
  const auto after=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(after.ok()&&SameOwnedInventory(after.checkpoint_inventory.inventory,inventory),
      "all original inventory fields unchanged despite physical publication");
  CheckParentHistory(f.devices,f.budget);
  const auto image=f.Read(0,f.total_pages);
  const auto replay=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,*f.issuer);
  Check(replay.error==db::NativePublicationError::operation_pending&&!replay.snapshot&&
      replay.effects==published.effects&&f.Read(0,f.total_pages)==image,
      "anchored parent replay retains original effects without generating a new graph");
  auto conflict=record;conflict.normalized_request_bytes.back()^=1;
  conflict.normalized_request_sha256=Sha(conflict.normalized_request_bytes);
  const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(history.ok()&&db::ValidateNativeManagementHistoryAppend(history,record,f.budget)==
      db::NativeManagementHistoryError::none,"exact parent history replay remains valid");
  Check(db::ValidateNativeManagementHistoryAppend(history,conflict,f.budget)==
      db::NativeManagementHistoryError::transition_failure,"history refuses changed parent payload under original identity");
  Check(!db::PublishNativeInventoryOnLease(*held.lease,conflict,inventory,f.budget,*f.issuer).ok()&&
      f.Read(0,f.total_pages)==image,"conflicting parent request cannot replace retained bytes");
  held.lease.reset();Check(f.device.Close().ok(),"close parent owner before independent reader");
  const auto child=fork();Check(child>=0,"spawn parent cold reader");
  const auto p=std::to_string(profile);
  if(child==0){execl("/proc/self/exe","startup-parent","--read-parent",f.path.c_str(),p.c_str(),nullptr);_exit(125);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
      "fresh process retains parent bytes without process-local state");
}
}
int main(int argc,char** argv) {
  try {
    if(argc==4&&std::string_view(argv[1])=="--read-parent") {
      const auto profile=std::stoul(argv[3]);Check(profile<5,"cold reader page profile");
      d::FileDevice device;Check(device.Open(argv[2],d::FileOpenMode::open_existing_read_only).ok(),"cold read-only open");
      const auto& page=d::kCanonicalFilespacePageProfiles[profile];
      CheckParentHistory({{Id(2),page.uuid,&device}},4096*u64{page.page_size_bytes});
    }else {Check(argc==1,"parent gate arguments");for(unsigned p=0;p<5;++p)ParentPublication(p);}
    std::cout<<"startup parent publication checks="<<checks<<" PASS; storage seam only\n";return 0;
  }catch(const std::exception& e){std::cerr<<"startup parent publication FAIL: "<<e.what()<<'\n';return 1;}
}
