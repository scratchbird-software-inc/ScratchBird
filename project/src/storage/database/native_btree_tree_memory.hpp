// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_btree_tree_backing.hpp"

namespace scratchbird::storage::database {
inline std::size_t NativeBtreeTreeWorkspaceBytes(std::span<const disk::NativeFilespaceDevice> files,
    NativeBtreeTreeMemoryLimits limits) noexcept {
  const auto largest=btree_memory_detail::LargestProfile(files);
  for(const auto& p:disk::kCanonicalFilespacePageProfiles)
    if(p.page_size_bytes==largest)return NativeBtreeTreeWorkspaceBytes(p.uuid,files.size(),limits);
  return 0;
}
// Standalone admission remains forbidden beneath an outer device guard.
// The shared traversal also serves the pre-admitted retained-source path.
inline NativeBtreeTreeMemoryResult ReadNativeBtreeTreeWithMemoryFromOpenDevices(
    const core::platform::Uuid& database,std::span<const disk::NativeFilespaceDevice> supplied,
    const disk::NativePageReference& root,const page::NativeBtreeDependencies& dependencies,
    NativeBtreeTreeMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using namespace btree_memory_detail;
  using E=NativeBtreeTreeMemoryError;using T=page::NativeBtreeError;
  NativeBtreeTreeMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    if(!core::uuid::IsEngineIdentityUuid(database)||!core::uuid::IsEngineIdentityUuid(root.filespace_uuid)||
       !root.page_number||!root.page_generation||!disk::FindCanonicalFilespacePageProfile(root.page_size_profile_uuid)||supplied.empty())return out;
    if(!page::NativeBtreeDependenciesValid(dependencies)){out.error=E::tree_failure;out.tree_error=T::invalid_dependencies;return out;}
    for(std::size_t i=0;i<supplied.size();++i){const auto& f=supplied[i];
      if(!core::uuid::IsEngineIdentityUuid(f.filespace_uuid)||!f.device||!disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)){
        out.error=E::tree_failure;out.tree_error=T::invalid_filespace;return out;}
      for(std::size_t j=0;j<i;++j)if(supplied[j].device==f.device||supplied[j].filespace_uuid==f.filespace_uuid){
        out.error=E::tree_failure;out.tree_error=T::invalid_filespace;return out;}
    }
    const auto largest=LargestProfile(supplied);
    const auto profile=std::find_if(disk::kCanonicalFilespacePageProfiles.begin(),disk::kCanonicalFilespacePageProfiles.end(),
      [&](const auto& p){return p.page_size_bytes==largest;});
    if(profile==disk::kCanonicalFilespacePageProfiles.end())return out;
    auto prepared=PrepareNativeBtreeTreeRead(profile->uuid,supplied.size(),limits,memory,binding);
    out.error=prepared.error;out.memory_error=prepared.memory_error;out.allocation_status=prepared.allocation_status;
    if(!prepared.ok()){out.allocation_diagnostic=std::move(prepared.allocation_diagnostic);return out;}
    auto& w=prepared.workspace;
    // Device descriptors have their own admitted array, separate from traversal.
    for(std::size_t i=0;i<supplied.size();++i)w.files_[i].source=supplied[i];
    std::sort(w.files_.begin(),w.files_.end(),[](const auto& a,const auto& b){return a.source.filespace_uuid<b.source.filespace_uuid;});
    for(std::size_t i=0;i<w.files_.size();++i)w.sources_[i]=w.files_[i].source;
    {
      Fences fences{w.locks_.data(),w.batches_.data()};
      for(auto& f:w.files_){std::construct_at(w.batches_.data()+fences.observations,*f.source.device);++fences.observations;}
      for(auto& f:w.files_){std::construct_at(w.locks_.data()+fences.locked,f.source.device->AcquireOperationGuard());++fences.locked;}
      auto observed=Reader::Run(database,w.sources_,w.batches_,root,dependencies,w);
      static_cast<NativeBtreeTreeReadResult&>(out)=std::move(observed);
    }
    out.allocation_status=prepared.allocation_status;
    out.allocation_diagnostic=std::move(prepared.allocation_diagnostic);
    if(out.NativeBtreeTreeReadResult::ok())out.arena=std::move(w.arena_);
    return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
