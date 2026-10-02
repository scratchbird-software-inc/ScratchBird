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
struct ParentPublicationState {
  mga::LocalTransactionInventory inventory;
  db::NativeManagementOperation record;
  db::NativePublicationReservation held;
  db::NativePublicationInspection published;
};
ParentPublicationState PublishParent(d::FileDevice& device,
    const std::vector<d::NativeFilespaceDevice>& devices,u64 budget,
    scratchbird::core::uuid::StandaloneUuidV7Issuer& issuer,unsigned profile,unsigned death=0,
    bool resume=false) {
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),devices,Id(2),budget);
  Check(selected.ok(),"actual initial inventory");
  const auto inventory=selected.checkpoint_inventory.inventory;
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(),"actual parent bootstrap binding");
  auto record=records::Example(1); record.uuid=Id(24000);record.request_context_uuid=Id(24001);
  record.bootstrap_uuid=zero.record->page_uuid;record.security_snapshot_uuid={};record.generation_guards={};
  record.idempotency_key="startup-parent-source-probe";
  record.normalized_request_bytes=ParentRequest();record.normalized_request_sha256=Sha(record.normalized_request_bytes);
  db::NativePublicationIntent intent{record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
      record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),budget);
  Check(before.ok(),"actual parent publication base");
  if(death==1)_exit(41); // No reservation/publication was attempted.
  if(resume) {
    Check(before.snapshot->watermark.operation_uuid==Id(24100)&&
        before.snapshot->watermark.intent==intent&&!before.snapshot->watermark.publication_plan&&
        before.snapshot->watermark.watermark>before.snapshot->selection.checkpoint_generation,
        "cold inspection retains exact pending reservation without a selected parent");
    const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),devices,Id(2),budget);
    Check(history.ok()&&history.entries.empty(),"reservation is not complete parent request persistence");
    auto wrong=intent;wrong.normalized_request_sha256.back()^=1;
    reads=writes=syncs=0;io_counting=true;
    const auto replacement=db::ReserveNativePublicationGenerationOnOpenDevices(
        Id(1),devices,Id(2),*before.snapshot,Id(24101),budget,&intent);
    const auto wrong_attempt=db::ResumeNativePublicationGenerationOnOpenDevices(
        Id(1),devices,Id(2),*before.snapshot,Id(24101),intent,budget);
    const auto wrong_request=db::ResumeNativePublicationGenerationOnOpenDevices(
        Id(1),devices,Id(2),*before.snapshot,Id(24100),wrong,budget);
    io_counting=false;
    Check(replacement.error==db::NativePublicationError::operation_pending&&
        !wrong_attempt.ok()&&!wrong_request.ok()&&!writes&&!syncs,
        "new identity or changed request cannot displace pending parent or write storage");
  }
  auto held=resume?db::ResumeNativePublicationGenerationOnOpenDevices(
      Id(1),devices,Id(2),*before.snapshot,Id(24100),intent,budget):
      db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),*before.snapshot,Id(24100),budget,&intent);
  Check(held.ok(),"reserve parent independently of child BEGIN");
  if(resume)Check(held.lease->snapshot().watermark.watermark==before.snapshot->watermark.watermark,
      "exact parent resume consumes no replacement generation");
  if(death==3)_exit(43); // Durable reservation, before the complete request is installed.
  const auto published=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,budget,issuer);
  if(!published.ok())std::cerr<<"parent profile="<<profile<<" error="<<int(published.error)<<'\n';
  Check(published.ok()&&published.effects.selected_graph_verified&&published.effects.successful_syncs,
      "parent request physically selected and verified");
  if(death==2)_exit(42); // Deliberately bypass lease/device destructors and Close.
  return {inventory,std::move(record),std::move(held),published};
}
void ParentPublication(unsigned profile) {
  Fixture f(profile); f.budget*=4;
  auto state=PublishParent(f.device,f.devices,f.budget,*f.issuer,profile);
  auto& inventory=state.inventory;auto& record=state.record;
  auto& held=state.held;auto& published=state.published;
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
void ParentPublisherDeath(unsigned profile,unsigned death) {
  Fixture f(profile);f.budget*=4;
  const auto initial=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(initial.ok(),"death fixture original selected inventory");
  Check(f.device.Close().ok(),"release fixture handles before fresh publisher");
  const auto child=fork();Check(child>=0,"spawn independent parent publisher");
  const auto p=std::to_string(profile),point=std::to_string(death);
  if(child==0){execl("/proc/self/exe","startup-parent","--publisher-death",f.path.c_str(),p.c_str(),point.c_str(),nullptr);_exit(125);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==40+death,
      "publisher reaches exact process-death boundary");
  Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),
      "independent owner can reopen after publisher death");
  const auto after=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(after.ok()&&SameOwnedInventory(after.checkpoint_inventory.inventory,initial.checkpoint_inventory.inventory),
      "publisher death preserves every original transaction field");
  if(death==2)CheckParentHistory(f.devices,f.budget);
  else {
    const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(history.ok()&&history.entries.empty(),"death before parent selection invents no retained parent request");
  }
  if(death==3) {
    Check(f.device.Close().ok(),"release observer before fresh recovery owner");
    const auto recovery=fork();Check(recovery>=0,"spawn independent exact parent recovery");
    if(recovery==0){execl("/proc/self/exe","startup-parent","--resume-parent",f.path.c_str(),p.c_str(),nullptr);_exit(125);}
    Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
        "fresh recovery consumes original caller-retained request without a new identity");
    Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),
        "independent reopen after exact parent recovery");
    CheckParentHistory(f.devices,f.budget);
    const auto recovered=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(recovered.ok()&&SameOwnedInventory(recovered.checkpoint_inventory.inventory,initial.checkpoint_inventory.inventory),
        "recovered parent preserves every original transaction field");
  }
}
}
int main(int argc,char** argv) {
  try {
    if((argc==5&&std::string_view(argv[1])=="--publisher-death")||
        (argc==4&&std::string_view(argv[1])=="--resume-parent")) {
      const bool resume=argc==4;
      const auto profile=std::stoul(argv[3]),death=resume?0:std::stoul(argv[4]);
      Check(profile<5&&(resume||death==1||death==2||death==3),"publisher death arguments");
      d::FileDevice device;Check(device.Open(argv[2],d::FileOpenMode::open_existing).ok(),"fresh publisher owns actual file");
      const auto& page=d::kCanonicalFilespacePageProfiles[profile];
      scratchbird::core::uuid::StandaloneUuidV7Issuer issuer(
          scratchbird::core::uuid::StandaloneUuidV7Binding{Id(1),Id(10)},
          scratchbird::core::uuid::StandaloneUuidV7Policy{{},0,1000});
      // Test supplies the ORIGINAL complete request; a reservation digest alone
      // cannot reconstruct a registered startup family or authorize recovery.
      (void)PublishParent(device,{{Id(2),page.uuid,&device}},4096*u64{page.page_size_bytes},issuer,profile,death,resume);
      return resume?0:126;
    }else if(argc==4&&std::string_view(argv[1])=="--read-parent") {
      const auto profile=std::stoul(argv[3]);Check(profile<5,"cold reader page profile");
      d::FileDevice device;Check(device.Open(argv[2],d::FileOpenMode::open_existing_read_only).ok(),"cold read-only open");
      const auto& page=d::kCanonicalFilespacePageProfiles[profile];
      CheckParentHistory({{Id(2),page.uuid,&device}},4096*u64{page.page_size_bytes});
    }else {Check(argc==1,"parent gate arguments");for(unsigned p=0;p<5;++p){
      ParentPublication(p);for(unsigned death=1;death<=3;++death)ParentPublisherDeath(p,death);}}
    std::cout<<"startup parent publication checks="<<checks<<" PASS; storage seam only\n";return 0;
  }catch(const std::exception& e){std::cerr<<"startup parent publication FAIL: "<<e.what()<<'\n';return 1;}
}
