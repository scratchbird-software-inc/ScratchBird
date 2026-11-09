// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_allocation_map.hpp"
#include "allocation_map_inplace_page.hpp"
#include "native_allocation_chain_backing.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "native_metadata_memory.hpp"
#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
using E = NativeAllocationError;
using S = NativeAllocationState;
bool V7(const Uuid& id) {
  return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
}
NativeAllocationChainResult ChainFail(E error) { NativeAllocationChainResult result; result.error = error; return result; }
}  // namespace

// W4d-3b (owner Q28 = 1, 2026-11-08): the sealed allocation map family, its
// codec and its selected and historical reads are removed; nothing writes
// them. The chain is the in-place family (allocation_map_inplace_page.hpp),
// read from page zero's allocation root (the bootstrap read).
static NativeAllocationChainView ReadAllocationChainBacked(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,u64 maximum_retained_image_bytes,
    std::pmr::memory_resource& resource,disk::FileDevice::ReadLatencyBatch* batch,
    NativeAllocationChainDeviceRead& receipt) noexcept {
  const auto fail=[](E error){NativeAllocationChainView r;r.error=error;return r;};
  try {
    disk::detail::NativeMetadataScratch scratch{&resource};
    const auto read=[&](u64 offset,void* bytes,std::size_t count){
      auto io=batch?batch->ReadAt(offset,bytes,count):device.ReadAt(offset,bytes,count);
      receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);
      if(io.bytes_transferred>std::numeric_limits<u64>::max()-receipt.physical_bytes_read)throw E::invalid_range;
      receipt.physical_bytes_read+=io.bytes_transferred;return io;
    };
    const auto size=[&]{auto io=device.Size();receipt.io_status=io.status;
      receipt.io_diagnostic=std::move(io.diagnostic);return io;};

    if (!maximum_retained_image_bytes) return fail(E::resource_exhausted);
    const auto guard = device.AcquireOperationGuard();
    std::span<const byte> zero_image;
    const auto zero = disk::detail::ReadNativeMetadataPageZero(binding,scratch,read,size,zero_image);
    if (!zero.ok()) {
      if (zero.error == disk::FilespacePageZeroError::resource_exhausted) return fail(E::resource_exhausted);
      if (zero.error == disk::FilespacePageZeroError::hash_provider_failure) return fail(E::hash_failure);
      if (zero.error == disk::FilespacePageZeroError::io_failure) return fail(E::io_failure);
      return fail(E::invalid_filespace);
    }
    const auto& z = *zero.record;
    if (z.bootstrap.flags & disk::FilespaceBootstrapFlag::cluster_authority_required)
      return fail(E::cluster_requires_authority);
    const auto initial = std::find_if(z.roots.begin(), z.roots.end(), [](const auto& r) { return r.kind == 3; });
    const auto* root=initial==z.roots.end()?nullptr:&*initial;
    if (!root||root->kind!=3||root->page_type!=3||!V7(root->object_uuid)||!root->page_number||!root->page_generation||
        root->filespace_uuid != binding.filespace_uuid || root->page_size_profile_uuid != binding.page_size_profile_uuid) return fail(E::invalid_reference);
    disk::NativePageReference ref{root->filespace_uuid, root->page_number, root->page_generation, root->page_size_profile_uuid};
    std::pmr::set<u64> slots(&resource);std::pmr::set<Uuid> page_ids(&resource);
    page_ids.insert(z.page_uuid);
    std::pmr::vector<NativeAllocationChainPageView> pages(&resource);
    NativeAllocationChainView result;
    u64 covered = 0;
    // SB_ALLOCATION_MAP_IN_PLACE (owner Q22 = 1): no seal, no digest link and
    // no page-zero counter cross-check. A head page of any other family is
    // refused as an unknown family; a later one as a broken chain.
    for (;;) {
      if (!slots.insert(ref.page_number).second) return fail(E::chain_mismatch);
      if (z.bootstrap.page_size_bytes > maximum_retained_image_bytes - result.retained_image_bytes)
        return fail(E::resource_exhausted);
      if (ref.page_number >= z.total_pages) return fail(E::invalid_range);
      auto bytes=scratch.Array<byte>(z.bootstrap.page_size_bytes);
      const auto io = read(ref.page_number * z.bootstrap.page_size_bytes, bytes.data(), bytes.size());
      if (!io.ok() || io.bytes_transferred != bytes.size()) return fail(E::io_failure);
      const disk::NativeCommonPageHeaderBinding expected{binding, ref.page_number, ref.page_generation, 3, {}};
      const auto common = disk::DecodeNativeCommonPageHeader(bytes.data(), 128, &expected);
      if (!common.ok()) return fail(E::binding_mismatch);
      if (common.header->flags & 2) return fail(E::cluster_requires_authority);
      if (!page_ids.insert(common.header->page_uuid).second) return fail(E::chain_mismatch);
      if (!IsAllocationMapInplaceImage(bytes)) return fail(pages.empty() ? E::invalid_family : E::chain_mismatch);
      {
        const u32 slots = AllocationMapInplaceSlotsPerPage(z.bootstrap.page_size_bytes);
        if (!slots) return fail(E::invalid_family);
        const auto decoded = DecodeAllocationMapInplaceInto(bytes, scratch.Array<NativeAllocationState>(slots),
                                                            scratch.Array<NativeAllocationRecord>(slots), &expected);
        if (!decoded.ok()) return fail(decoded.error);
        const auto& map = decoded.view->map;
        if (map.object_uuid != root->object_uuid || map.total_pages != z.total_pages || map.first_page != covered ||
            decoded.view->sequence != pages.size())
          return fail(E::chain_mismatch);
        if (!pages.empty()) {
          const auto& head = pages.front().map;
          if (map.creator_transaction_uuid != head.creator_transaction_uuid ||
              map.creator_operation_uuid != head.creator_operation_uuid ||
              map.creator_local_transaction_id != head.creator_local_transaction_id) return fail(E::chain_mismatch);
        }
        covered += map.states.size();
        for (auto state : map.states) ++result.state_counts[static_cast<byte>(state)];
        result.retained_image_bytes += bytes.size();
        const auto next = map.next;
        pages.push_back({map, bytes});
        if (!next) break;
        ref = *next;
        continue;
      }
    }
    if (covered != z.total_pages) return fail(E::counter_mismatch);
    const auto allocated = [&](u64 number, const Uuid& id, u64 generation, u32 type, const Uuid& owner) {
      for (const auto& image : pages) {
        const auto& map = image.map;
        if (number < map.first_page || number - map.first_page >= map.states.size()) continue;
        if (map.states[number - map.first_page] != S::allocated) return false;
        const auto r = std::lower_bound(map.records.begin(), map.records.end(), number,
                                       [](const auto& value, u64 n) { return value.page_number < n; });
        return r != map.records.end() && r->page_number == number && r->page_uuid == id &&
               r->page_generation == generation && r->page_type == type && r->owner_uuid == owner;
      }
      return false;
    };
    const u32 zero_type = z.bootstrap.filespace_role <= 4 ? 1 : 2;
    if (!allocated(0, z.page_uuid, z.page_generation, zero_type, z.bootstrap.filespace_uuid))
      return fail(E::physical_owner_mismatch);
    for (const auto& image : pages) {
      const auto& map = image.map; const auto& h = map.header;
      if (!allocated(h.page_number, h.page_uuid, h.page_generation, 3, map.object_uuid))
        return fail(E::physical_owner_mismatch);
    }
    result.pages=scratch.Copy<NativeAllocationChainPageView>(pages);
    result.error = E::none;
    return result;
  } catch (E error) { return fail(error); }
    catch (const std::bad_alloc&) { return fail(E::resource_exhausted); }
    catch (const std::length_error&) { return fail(E::resource_exhausted); }
    catch (...) { return fail(E::io_failure); }
}
static NativeAllocationChainResult ReadAllocationChain(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,u64 budget) noexcept {
  try {
    disk::detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
    NativeAllocationChainDeviceRead receipt;
    const auto view=ReadAllocationChainBacked(device,binding,budget,resource,nullptr,receipt);
    if(!view.ok())return ChainFail(view.error);
    // Materialize after the complete source fence releases. Both APIs use the
    // same canonical decode and chain rules; no second provider pass is needed.
    NativeAllocationChainResult out;out.retained_image_bytes=view.retained_image_bytes;
    out.state_counts=view.state_counts;out.pages.reserve(view.pages.size());
    for(const auto& page:view.pages){
      NativeAllocationMapResult value;
      NativeAllocationMap map;
      static_cast<disk::NativeCommonPageHeader&>(map.header)=page.map.header;
      map.object_uuid=page.map.object_uuid;map.map_generation=page.map.map_generation;
      map.capacity_generation=page.map.capacity_generation;map.total_pages=page.map.total_pages;
      map.first_page=page.map.first_page;map.creator_transaction_uuid=page.map.creator_transaction_uuid;
      map.creator_local_transaction_id=page.map.creator_local_transaction_id;
      map.creator_operation_uuid=page.map.creator_operation_uuid;
      map.next=page.map.next;map.next_sha256=page.map.next_sha256;
      map.states.assign(page.map.states.begin(),page.map.states.end());
      map.records.assign(page.map.records.begin(),page.map.records.end());
      value.map=std::move(map);value.bytes.assign(page.image.begin(),page.image.end());
      value.error=E::none;out.pages.push_back(std::move(value));
    }
    out.error=E::none;return out;
  }catch(const std::bad_alloc&){return ChainFail(E::resource_exhausted);}
   catch(const std::length_error&){return ChainFail(E::resource_exhausted);}
   catch(...){return ChainFail(E::io_failure);}
}
NativeAllocationChainDeviceRead detail::ReadNativeAllocationChainBacked(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,u64 budget,
    disk::FileDevice::ReadLatencyBatch& batch,std::pmr::memory_resource& resource) noexcept {
  NativeAllocationChainDeviceRead out;
  if(&batch.device()!=&device)return out;
  out.chain=ReadAllocationChainBacked(device,binding,budget,resource,&batch,out);
  return out;
}
NativeAllocationChainDeviceRead ReadNativeAllocationChainInto(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,
    u64 budget,disk::FileDevice::ReadLatencyBatch& batch,std::span<byte> backing) noexcept {
  NativeAllocationChainDeviceRead out;
  if(&batch.device()!=&device)return out;
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  if(!disjoint(std::span{&device,1})||!disjoint(std::span{&binding,1})||!disjoint(std::span{&batch,1})){
    out.chain.error=E::invalid_workspace;return out;
  }
  disk::detail::NativeMetadataMemory resource(backing);
  out=detail::ReadNativeAllocationChainBacked(device,binding,budget,batch,resource);
  if(out.ok())out.chain.backing_bytes_used=resource.used();
  return out;
}
NativeAllocationChainResult ReadNativeAllocationChainFromOpenDevice(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,u64 budget) noexcept {
  return ReadAllocationChain(device,binding,budget);
}
}  // namespace scratchbird::storage::page
