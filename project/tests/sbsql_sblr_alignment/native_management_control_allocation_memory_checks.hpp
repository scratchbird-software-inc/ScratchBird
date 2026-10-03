// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included inside the owning storage test namespace after Check/CE/Bytes and
// fault counters are declared. Does not replace any original test or oracle.
void BoundedControlCheck(const Bytes& base,const Bytes& target,const Bytes& plan,
    const std::vector<Bytes>& extent,const std::vector<Bytes>& before,const std::vector<Bytes>& after,
    u64 limit,const std::vector<Bytes>& bundle,const std::vector<Bytes>& inventory,
    const db::NativeManagementDirectoryBase* directory,CE expected,bool deep=false){
  using Image=std::span<const byte>;
  const auto borrow=[](const auto& group){std::vector<Image> out;for(const auto& raw:group)out.push_back(raw);return out;};
  const auto e=borrow(extent),b=borrow(before),a=borrow(after),c=borrow(bundle),i=borrow(inventory);
  std::vector<Image> d,z;db::NativeManagementDirectoryBaseView context;
  if(directory){d=borrow(directory->directory_images);z=borrow(directory->page_zero_images);context={d,z};}
  const db::NativeManagementControlAllocationInputs request{base,target,plan,e,b,a,c,i,directory?&context:nullptr,limit};
  std::size_t count=base.size()+target.size()+plan.size();
  for(const auto* group:{&extent,&before,&after,&bundle,&inventory})for(const auto& raw:*group)count+=raw.size();
  if(directory)for(const auto* group:{&directory->directory_images,&directory->page_zero_images})for(const auto& raw:*group)count+=raw.size();
  Bytes backing(12*count+65536);
  const auto call=[&](std::span<byte> region){allocation_budget=0;
    const auto result=db::ValidateNativeManagementControlAllocationInto(request,region);
    const bool unchanged=allocation_budget==0;allocation_budget=-1;
    Check(unchanged,"full control delta and typed refusals have no hidden ordinary or aligned payload allocation");return result;};
  const auto full=call(backing);
  Check(full.error==expected,"complete primary/directory/inventory/preallocation/growth bounded delta preserves exact owning error");
  if(!full.ok()){Check(!full.backing_bytes_used,"failed immutable control proof exposes no successful accounting prefix");return;}
  Check(full.backing_bytes_used&&full.backing_bytes_used<=backing.size(),"full control validation accounts actual bounded scratch usage");
  const auto exact=call(std::span(backing).first(full.backing_bytes_used));
  Check(exact.ok()&&exact.backing_bytes_used==full.backing_bytes_used,"exact full control backing succeeds");
  const auto short_result=call(std::span(backing).first(full.backing_bytes_used-1));
  Check(short_result.error==CE::resource_exhausted&&!short_result.backing_bytes_used,"one-byte-short backing refuses full control validation");
  if(!deep)return;
  const auto alias=[&](auto input){if(input.empty())return;
    const auto result=call({reinterpret_cast<byte*>(const_cast<std::remove_const_t<typename decltype(input)::element_type>*>(input.data())),input.size_bytes()});
    Check(result.error==CE::invalid_workspace&&!result.backing_bytes_used,"whole control input overlap refuses before mutation");};
  alias(std::span{&request,1});alias(Image(base));alias(Image(target));alias(Image(plan));
  for(const auto group:{std::span<const Image>(e),std::span<const Image>(b),std::span<const Image>(a),std::span<const Image>(c),std::span<const Image>(i),std::span<const Image>(d),std::span<const Image>(z)}){
    alias(group);for(const auto raw:group)alias(raw);}
  if(directory)alias(std::span{&context,1});
  Check(call(backing).ok(),"all alias refusals preserve every source image and descriptor");
  hash_counting=true;hash_seen=0;const auto measured=call(backing);hash_counting=false;const auto sites=hash_seen;
  Check(measured.ok()&&sites,"every complete bounded control hash site measured");
  for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=sites;++site){
    hash_target=site;hash_seen=0;hash_fault=mode;hash_active=false;
    const auto result=call(backing);const bool consumed=!hash_fault;hash_fault=0;hash_active=false;
    Check(consumed&&result.error==CE::hash_failure&&!result.backing_bytes_used,"every complete control provider phase refuses without heap diagnostics or result prefix");
  }
  d::detail::NativeMetadataMemory resource(backing);allocation_budget=0;
  const auto composed=db::detail::ValidateNativeManagementControlAllocationBacked(request,resource);
  const bool unchanged=allocation_budget==0;allocation_budget=-1;
  Check(unchanged&&composed==CE::none&&resource.used()==full.backing_bytes_used,"whole source may compose exact complete delta under already admitted resource");
}
