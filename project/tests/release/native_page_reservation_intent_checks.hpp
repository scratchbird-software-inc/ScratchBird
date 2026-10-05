// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included in the storage intent gate's anonymous namespace to share real
// provider/heap/I/O fault interceptors, not production encoding helpers.
using RI=db::NativePageReservationIntent;
using RA=db::NativePageReservationAssignment;
using RE=db::NativePageReservationIntentError;
using RR=db::NativePageReservationIntentImage;
void ReservationSeal(Bytes& b){const auto h=Sha(Bytes(b.begin(),b.end()-32));std::copy(h.begin(),h.end(),b.end()-32);}
RI ReservationExample(unsigned target,unsigned checkpoint,std::span<const RA> pages){
  RI i;i.request_uuid=Id(401);i.operation_uuid=Id(402);i.database_uuid=Id(403);i.filespace_uuid=Id(404);
  i.page_size_profile_uuid=disk::kCanonicalFilespacePageProfiles[target].uuid;
  i.locator_uuid=Id(405);i.page_zero_uuid=Id(406);i.allocation_object_uuid=Id(407);
  i.transaction_uuid=Id(408);i.owner_uuid=Id(409);i.initiator_uuid=Id(410);i.request_context_uuid=Id(411);
  i.policy_snapshot_uuid=Id(412);i.security_snapshot_uuid=Id(413);
  i.checkpoint={Id(414),7,8,disk::kCanonicalFilespacePageProfiles[checkpoint].uuid};i.checkpoint_object_uuid=Id(415);
  for(unsigned n=0;n<32;++n){i.checkpoint_sha256[n]=n+1;i.allocation_sha256[n]=100+n;}
  i.allocation_page_number=5;i.allocation_page_generation=6;i.local_transaction_id=9;i.selection_generation=10;
  i.checkpoint_generation=11;i.checkpoint_root_set_generation=12;i.directory_generation=13;i.page_zero_generation=14;
  i.filespace_root_set_generation=15;i.map_generation=16;i.capacity_generation=17;i.total_pages=128;
  i.catalog_generation=18;i.configuration_generation=19;i.security_generation=20;
  i.maximum_work_bytes=pages.size()*u64{disk::kCanonicalFilespacePageProfiles[target].page_size_bytes};i.pages=pages;return i;
}
Bytes ReservationOracle(const RI& i){
  // Spec offsets stated independently; do not use production member tables,
  // size calculation, field writers or decoder as an oracle.
  Bytes b(544+64*i.pages.size());const std::string magic="SBPRSV01";
  std::copy(magic.begin(),magic.end(),b.begin());Number(b,8,2,1);Number(b,10,2,512);Number(b,12,2,64);
  Number(b,16,8,b.size());Number(b,24,8,i.pages.size());
  Identity(b,32,i.request_uuid);Identity(b,48,i.operation_uuid);Identity(b,64,i.database_uuid);Identity(b,80,i.filespace_uuid);
  Identity(b,96,i.page_size_profile_uuid);Identity(b,112,i.locator_uuid);Identity(b,128,i.page_zero_uuid);
  Identity(b,144,i.allocation_object_uuid);Identity(b,160,i.transaction_uuid);Identity(b,176,i.owner_uuid);
  Identity(b,192,i.initiator_uuid);Identity(b,208,i.request_context_uuid);Identity(b,224,i.policy_snapshot_uuid);
  Identity(b,240,i.security_snapshot_uuid);Identity(b,256,i.checkpoint.filespace_uuid);
  Number(b,272,8,i.checkpoint.page_number);Number(b,280,8,i.checkpoint.page_generation);
  Identity(b,288,i.checkpoint.page_size_profile_uuid);Identity(b,304,i.checkpoint_object_uuid);
  std::copy(i.checkpoint_sha256.begin(),i.checkpoint_sha256.end(),b.begin()+320);
  Number(b,352,8,i.allocation_page_number);Number(b,360,8,i.allocation_page_generation);
  std::copy(i.allocation_sha256.begin(),i.allocation_sha256.end(),b.begin()+368);
  Number(b,400,8,i.local_transaction_id);Number(b,408,8,i.selection_generation);Number(b,416,8,i.checkpoint_generation);
  Number(b,424,8,i.checkpoint_root_set_generation);Number(b,432,8,i.directory_generation);Number(b,440,8,i.page_zero_generation);
  Number(b,448,8,i.filespace_root_set_generation);Number(b,456,8,i.map_generation);Number(b,464,8,i.capacity_generation);
  Number(b,472,8,i.total_pages);Number(b,480,8,i.catalog_generation);Number(b,488,8,i.configuration_generation);
  Number(b,496,8,i.security_generation);Number(b,504,8,i.maximum_work_bytes);
  for(usize n=0;n<i.pages.size();++n){const auto& a=i.pages[n];const auto at=512+64*n;
    Number(b,at,8,a.page_number);Identity(b,at+8,a.allocation_uuid);Identity(b,at+24,a.page_uuid);
    Number(b,at+40,8,a.page_generation);Number(b,at+48,4,a.page_type);}
  ReservationSeal(b);return b;
}
void ReservationFailed(const RR& r,RE e){Check(!r.ok()&&r.error==e&&!r.intent&&r.bytes.empty(),"exact reservation refusal without partial view");}
O ReservationOperation(const RI& i,unsigned target){
  auto operation=Operation(Example(target,2));operation.database_uuid=i.database_uuid;
  operation.uuid=i.operation_uuid;operation.target_uuid=i.filespace_uuid;operation.initiator_uuid=i.initiator_uuid;
  operation.request_context_uuid=i.request_context_uuid;operation.policy_snapshot_uuid=i.policy_snapshot_uuid;
  operation.security_snapshot_uuid=i.security_snapshot_uuid;
  operation.generation_guards={i.catalog_generation,i.configuration_generation,i.security_generation,{}};
  operation.normalized_request_bytes=ReservationOracle(i);operation.normalized_request_sha256=Sha(operation.normalized_request_bytes);return operation;
}
void ReservationOperationChecks(const RI& i,unsigned target){
  const auto operation=ReservationOperation(i,target);std::array<RA,3> pages;std::array<Uuid,7> scratch;
  const auto call=[&](const O& o){std::vector<db::NativeManagementStepView> steps;const auto view=OperationView(o,steps);
    allocation_budget=0;const auto r=db::ReadNativePageReservationIntentFromOperationView(view,pages,scratch,736);
    const bool untouched=allocation_budget==0;allocation_budget=-1;Check(untouched,"full reservation operation binding without heap fallback");return r;};
  auto result=call(operation);Check(result.ok()&&ReservationOracle(*result.intent)==operation.normalized_request_bytes,"full exact operation binding");
  for(auto member:{&O::uuid,&O::database_uuid,&O::target_uuid,&O::initiator_uuid,&O::request_context_uuid,&O::policy_snapshot_uuid,&O::security_snapshot_uuid}){
    auto o=operation;o.*member=Id(800);ReservationFailed(call(o),RE::binding_mismatch);
  }
  for(usize n=0;n<3;++n){auto o=operation;++*o.generation_guards[n];ReservationFailed(call(o),RE::binding_mismatch);
    o=operation;o.generation_guards[n].reset();ReservationFailed(call(o),RE::binding_mismatch);}
  auto o=operation;o.scope=db::NativeManagementScope::cluster;o.cluster_uuid=Id(801);o.generation_guards[3]=1;
  ReservationFailed(call(o),RE::binding_mismatch);
  o=operation;o.normalized_request_bytes.clear();ReservationFailed(call(o),RE::invalid_header);
  o=operation;o.normalized_request_sha256[0]^=1;result=call(o);ReservationFailed(result,RE::operation_failure);
  Check(result.operation_error==db::NativeManagementOperationError::invalid_integrity,"complete operation integrity failure retained");
  o=operation;o.idempotency_key.clear();result=call(o);ReservationFailed(result,RE::operation_failure);
  Check(result.operation_error==db::NativeManagementOperationError::invalid_utf8,"unrelated complete operation failure retained");
  o=operation;o.normalized_request_bytes.back()^=1;o.normalized_request_sha256=Sha(o.normalized_request_bytes);
  result=call(o);ReservationFailed(result,RE::invalid_integrity);Check(result.operation_error==db::NativeManagementOperationError::none,"inner and outer seals distinct");
  for(unsigned site=1;site<=2;++site)for(unsigned mode=1;mode<=5;++mode){
    hash_target=site;hash_seen=0;hash_active=false;hash_fault=mode;result=call(operation);
    const bool consumed=!hash_fault;hash_fault=0;hash_active=false;Check(consumed,"both reservation operation hash sites faulted");
    ReservationFailed(result,RE::hash_failure);Check(result.operation_error==(site==1?db::NativeManagementOperationError::hash_failure:
      db::NativeManagementOperationError::none),"exact nested hash error");
  }
  std::vector<db::NativeManagementStepView> steps;const auto view=OperationView(operation,steps);
  result=db::ReadNativePageReservationIntentFromOperationView(view,pages,std::span(scratch).first(6),736);
  ReservationFailed(result,RE::resource_exhausted);Check(result.operation_error==db::NativeManagementOperationError::resource_exhausted,"full record scratch accounting");
  const auto before=operation.normalized_request_bytes;
  ReservationFailed(db::ReadNativePageReservationIntentFromOperationView(view,
    {reinterpret_cast<RA*>(const_cast<byte*>(operation.normalized_request_bytes.data())),3},scratch,736),RE::invalid_workspace);
  ReservationFailed(db::ReadNativePageReservationIntentFromOperationView(view,pages,
    {reinterpret_cast<Uuid*>(const_cast<byte*>(operation.normalized_request_bytes.data())),7},736),RE::invalid_workspace);
  Check(before==operation.normalized_request_bytes,"operation writable buffers cannot alias request");
  Check(call(operation).ok(),"original operation remains reusable after refusal");
}
void ReservationPhysical(const RI& i,unsigned target){
  TemporaryDirectory temporary;const auto path=(temporary.path/"reservation").string();disk::FileDevice device;
  Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"create reservation byte-preservation fixture");
  const auto& p=*disk::FindCanonicalFilespacePageProfile(i.checkpoint.page_size_profile_uuid);
  db::NativeFilespaceInitializationRequest init;
  init.bootstrap={i.database_uuid,i.checkpoint.filespace_uuid,p.uuid,disk::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};
  init.operation_uuid=Id(701);init.writer_uuid=Id(702);init.creator.transaction_uuid={UuidKind::transaction,Id(703)};
  init.creator.local_id=scratchbird::transaction::mga::MakeLocalTransactionId(1);
  init.creator.scope=scratchbird::transaction::mga::TransactionScope::local_node;
  init.creation_utc_millis=1789357072000ULL;init.total_pages=128;init.policy_snapshot_uuid=i.policy_snapshot_uuid;
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({i.database_uuid,i.policy_snapshot_uuid},{{},0,1000});
  Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,init,1<<26,issuer).ok(),"initialize actual native reservation fixture");
  const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok(),"actual page-zero identity");
  auto operation=ReservationOperation(i,target);operation.bootstrap_uuid=zero.record->page_uuid;
  const auto record=db::EncodeNativeManagementOperation(operation,1<<26);Check(record.ok(),"complete operation image for extent sizing");
  const usize extent_pages=(record.bytes.size()+p.page_size_bytes-385)/(p.page_size_bytes-384);
  std::vector<disk::NativeCommonPageHeader> headers;
  for(usize n=0;n<extent_pages;++n)headers.push_back({p.page_size_bytes,0x500,i.database_uuid,i.checkpoint.filespace_uuid,Id(10000+n),64+n,1,0,p.uuid});
  const auto extent=db::EncodeNativeManagementExtent(operation,Id(705),headers,1<<26);
  Check(extent.ok()&&extent.pages.size()==extent_pages,"reservation bytes in complete management extent");
  for(usize n=0;n<extent.pages.size();++n){const auto write=device.WriteAt((64+n)*u64{p.page_size_bytes},extent.pages[n].data(),extent.pages[n].size());
    Check(write.ok()&&write.bytes_transferred==p.page_size_bytes,"write each real extent page");}
  Check(device.Sync().ok()&&device.Close().ok(),"reservation extent sync close");
  const auto read=[&](){disk::FileDevice reader;if(!reader.Open(path,disk::FileOpenMode::open_existing_read_only).ok())return false;
    const auto extent_read=db::ReadNativeManagementExtentFromOpenDevice({i.checkpoint.filespace_uuid,p.uuid,&reader},*extent.root,i.database_uuid,operation.bootstrap_uuid,1<<26);
    if(!extent_read.ok()||*extent_read.record!=operation)return false;
    std::vector<RA> pages(i.pages.size());std::vector<Uuid> scratch(std::max(usize{7},2*i.pages.size()));std::vector<db::NativeManagementStepView> steps;
    const auto view=OperationView(*extent_read.record,steps);
    const auto decoded=db::ReadNativePageReservationIntentFromOperationView(view,pages,scratch,operation.normalized_request_bytes.size());
    return decoded.ok()&&ReservationOracle(*decoded.intent)==operation.normalized_request_bytes;
  };
  Check(read(),"reopen exact durable reservation request bytes");const auto child=fork();Check(child>=0,"reservation independent reader fork");
  if(child==0){try{_exit(read()?0:1);}catch(...){_exit(2);}}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"separate process reads exact reservation identities");
  // Unselected immutable extent, deliberately NOT allocation/publication proof.
}
void ReservationIntentChecks(unsigned target,unsigned checkpoint){
  std::array<RA,3> input{{{32,Id(501),Id(502),1,1},{33,Id(503),Id(504),1,1},{63,Id(505),Id(506),1,1}}};
  auto i=ReservationExample(target,checkpoint,input);const auto raw=ReservationOracle(i);
  Bytes output(raw.size());std::array<RA,3> decoded;std::array<Uuid,6> scratch;
  const auto encode=[&](const RI& value,std::span<byte> out,u64 budget){allocation_budget=0;
    const auto r=db::EncodeNativePageReservationIntentInto(value,out,scratch,budget);const bool untouched=allocation_budget==0;
    allocation_budget=-1;Check(untouched,"reservation encoder uses no C++ heap");return r;};
  const auto decode=[&](std::span<const byte> bytes,u64 budget){allocation_budget=0;
    const auto r=db::DecodeNativePageReservationIntentInto(bytes,decoded,scratch,budget);const bool untouched=allocation_budget==0;
    allocation_budget=-1;Check(untouched,"reservation decoder uses no C++ heap");return r;};
  const auto good=encode(i,output,raw.size());Check(good.ok()&&output==raw&&good.intent->pages.data()==input.data(),"exact independent reservation encode and input lifetime");
  const auto back=decode(raw,raw.size());Check(back.ok()&&decoded==input&&back.intent->pages.data()==decoded.data()&&
    back.bytes.data()==raw.data()&&ReservationOracle(*back.intent)==raw,"complete independent decode and backing lifetime");
  Check(db::NativePageReservationIntentBytes(0)==0&&db::NativePageReservationIntentBytes(1)==608&&db::NativePageReservationIntentBytes(3)==736,"exact shape sizes");
  const auto max=std::numeric_limits<usize>::max();const auto max_count=(max-544)/64;
  Check(db::NativePageReservationIntentBytes(max_count)==544+64*max_count&&db::NativePageReservationIntentBytes(max_count+1)==0&&db::NativePageReservationIntentBytes(max)==0,"overflow-safe shape arithmetic");
  ReservationFailed(encode(i,std::span(output).first(raw.size()-1),raw.size()),RE::resource_exhausted);
  ReservationFailed(encode(i,output,raw.size()-1),RE::resource_exhausted);ReservationFailed(decode(raw,raw.size()-1),RE::resource_exhausted);
  ReservationFailed(db::DecodeNativePageReservationIntentInto(raw,std::span(decoded).first(2),scratch,raw.size()),RE::resource_exhausted);
  ReservationFailed(db::DecodeNativePageReservationIntentInto(raw,decoded,std::span(scratch).first(5),raw.size()),RE::resource_exhausted);
  ReservationFailed(db::EncodeNativePageReservationIntentInto(i,output,std::span(scratch).first(5),raw.size()),RE::resource_exhausted);
  for(usize n=0;n<raw.size();++n)ReservationFailed(decode(std::span(raw).first(n),raw.size()),RE::invalid_header);
  auto extra=raw;extra.push_back(0);ReservationFailed(decode(extra,extra.size()),RE::invalid_header);
  for(usize at=0;at<raw.size();++at){auto x=raw;x[at]^=1;const auto r=decode(x,x.size());
    Check(!r.ok()&&!r.intent&&r.bytes.empty(),"every byte covered by canonical shape or integrity");}
  const auto changed=[&](usize at,unsigned width,u64 value,RE expected){auto x=raw;Number(x,at,width,value);ReservationSeal(x);ReservationFailed(decode(x,x.size()),expected);};
  for(usize at:{usize{8},usize{10},usize{12},usize{14}})changed(at,2,99,RE::invalid_header);
  changed(24,8,0,RE::invalid_header);changed(24,8,std::numeric_limits<u64>::max(),RE::invalid_header);
  // All header and assignment identities, including both profile fields.
  for(usize at:{32,48,64,80,96,112,128,144,160,176,192,208,224,240,256,288,304,520,536,584,600,648,664}){
    auto x=raw;std::fill_n(x.begin()+at,16,0);ReservationSeal(x);
    ReservationFailed(decode(x,x.size()),at==288?RE::invalid_profile:RE::invalid_identity);
    x=raw;x[at+6]=0x40;ReservationSeal(x);ReservationFailed(decode(x,x.size()),at==288?RE::invalid_profile:RE::invalid_identity);
  }
  for(usize at:{96,288}){auto x=raw;Identity(x,at,Id(799));ReservationSeal(x);ReservationFailed(decode(x,x.size()),RE::invalid_profile);}
  for(usize at:{320,368}){auto x=raw;std::fill_n(x.begin()+at,32,0);ReservationSeal(x);ReservationFailed(decode(x,x.size()),RE::invalid_integrity);}
  for(usize at:{272,280,352,360})changed(at,8,0,RE::invalid_reference);
  changed(272,8,std::numeric_limits<u64>::max(),RE::invalid_reference);changed(352,8,i.total_pages,RE::invalid_reference);
  for(usize at=400;at<472;at+=8)changed(at,8,0,RE::invalid_range);
  changed(472,8,std::numeric_limits<u64>::max(),RE::invalid_range);changed(504,8,i.maximum_work_bytes-1,RE::invalid_range);
  for(usize n=0;n<3;++n){const auto at=512+64*n;changed(at,8,0,RE::invalid_range);changed(at,8,i.total_pages,RE::invalid_range);
    changed(at+40,8,0,RE::invalid_range);changed(at+40,8,2,RE::invalid_range);
    changed(at+48,4,0,RE::invalid_profile);changed(at+48,4,0xffffffff,RE::invalid_profile);
    for(usize reserved=52;reserved<64;++reserved)changed(at+reserved,1,1,RE::invalid_header);
  }
  changed(512,8,i.allocation_page_number,RE::invalid_reference);changed(576,8,32,RE::invalid_range);
  const auto collision=[&](usize destination,usize source){auto x=raw;std::copy_n(raw.begin()+source,16,x.begin()+destination);
    ReservationSeal(x);ReservationFailed(decode(x,x.size()),RE::invalid_identity);};
  collision(32,48);
  for(usize dst:{520,536,584,600,648,664}){
    for(usize src=32;src<=256;src+=16)collision(dst,src);
    collision(dst,288);collision(dst,304);
    for(usize src:{520,536,584,600,648,664})if(src!=dst)collision(dst,src);
  }
  auto x=i;x.catalog_generation=x.configuration_generation=x.security_generation=0;
  Check(encode(x,output,raw.size()).ok()&&output==ReservationOracle(x),"bootstrap zero epochs retained");
  x=i;x.checkpoint.filespace_uuid=x.filespace_uuid;
  if(target!=checkpoint)ReservationFailed(encode(x,output,raw.size()),RE::invalid_reference);
  x.checkpoint.page_size_profile_uuid=x.page_size_profile_uuid;Check(encode(x,output,raw.size()).ok(),"same-member checkpoint accepted");
  x.checkpoint.page_number=input[0].page_number;ReservationFailed(encode(x,output,raw.size()),RE::invalid_reference);
  x=i;x.pages={};ReservationFailed(encode(x,output,raw.size()),RE::resource_exhausted);
  // Actual overlapping ranges, rejected before writing immutable bytes.
  output=raw;const auto prior=output;
  ReservationFailed(db::DecodeNativePageReservationIntentInto(output,decoded,{reinterpret_cast<Uuid*>(output.data()),6},raw.size()),RE::invalid_workspace);
  Check(output==prior,"aliased decode preserves canonical input");
  ReservationFailed(db::DecodeNativePageReservationIntentInto(raw,decoded,{reinterpret_cast<Uuid*>(decoded.data()),6},raw.size()),RE::invalid_workspace);
  ReservationFailed(db::EncodeNativePageReservationIntentInto(i,output,{reinterpret_cast<Uuid*>(output.data()),6},raw.size()),RE::invalid_workspace);
  Check(output==prior,"aliased encode refuses before output mutation");
  auto alias_input=input;x=i;x.pages=alias_input;
  ReservationFailed(db::EncodeNativePageReservationIntentInto(x,output,{reinterpret_cast<Uuid*>(alias_input.data()),6},raw.size()),RE::invalid_workspace);
  Check(alias_input==input,"aliased scratch preserves assignment input");
  for(unsigned mode=1;mode<=5;++mode)for(unsigned direction=0;direction<2;++direction){
    hash_target=1;hash_seen=0;hash_active=false;hash_fault=mode;
    const auto r=direction?encode(i,output,raw.size()):decode(raw,raw.size());const bool consumed=!hash_fault;hash_fault=0;hash_active=false;
    Check(consumed,"reservation hash provider failure reached");ReservationFailed(r,RE::hash_failure);
  }
  Check(encode(i,output,raw.size()).ok()&&output==raw&&decode(raw,raw.size()).ok(),"same request succeeds after every refusal without new identities");
  // Legal arithmetic extremes do not wrap the physical extent or work budget.
  x=i;x.total_pages=std::numeric_limits<u64>::max()/disk::kCanonicalFilespacePageProfiles[target].page_size_bytes;
  x.checkpoint.page_number=std::numeric_limits<u64>::max()/disk::kCanonicalFilespacePageProfiles[checkpoint].page_size_bytes-1;
  x.local_transaction_id=std::numeric_limits<u64>::max();x.maximum_work_bytes=std::numeric_limits<u64>::max();
  Check(encode(x,output,raw.size()).ok()&&output==ReservationOracle(x),"largest representable references and work bound");
  ++x.checkpoint.page_number;ReservationFailed(encode(x,output,raw.size()),RE::invalid_reference);
  ReservationOperationChecks(i,target);ReservationPhysical(i,target);
  // Variable-length requests, including a management extent spanning several
  // physical pages on smaller profiles; never imply assignment admission.
  for(usize count:{1,2,257}){
    std::vector<RA> values;for(usize n=0;n<count;++n)values.push_back({32+n,Id(1000+2*n),Id(1001+2*n),1,1});
    auto many=ReservationExample(target,checkpoint,values);many.total_pages=512;
    const auto expected=ReservationOracle(many);Bytes encoded(expected.size());std::vector<RA> readback(count);std::vector<Uuid> ids(2*count);
    allocation_budget=0;const auto written=db::EncodeNativePageReservationIntentInto(many,encoded,ids,expected.size());
    const bool encode_noheap=allocation_budget==0;allocation_budget=-1;
    Check(encode_noheap&&written.ok()&&encoded==expected,"variable-length independent encode without heap");
    allocation_budget=0;const auto restored=db::DecodeNativePageReservationIntentInto(encoded,readback,ids,expected.size());
    const bool decode_noheap=allocation_budget==0;allocation_budget=-1;
    Check(decode_noheap&&restored.ok()&&readback==values&&ReservationOracle(*restored.intent)==expected,"variable-length complete decode without heap");
    ReservationPhysical(many,target);
  }
}
