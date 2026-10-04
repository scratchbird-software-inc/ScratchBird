// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_inventory_publication_delta.hpp"
#include "native_inventory_delta_backing.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "transaction_inventory_validation.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
namespace mga = transaction::mga;
using E = NativeInventoryDeltaError;
using Image = std::span<const byte>;
using Images = std::span<const Image>;
using Entry = mga::TransactionInventoryEntry;
void Require(bool value, E error) { if (!value) throw error; }
bool V7(const Uuid& id) { return core::uuid::IsEngineIdentityUuid(id); }
bool Same(const Entry& a, const Entry& b) {
  return a.identity.transaction_uuid.kind == b.identity.transaction_uuid.kind &&
    a.identity.transaction_uuid.value == b.identity.transaction_uuid.value &&
    a.identity.local_id.value == b.identity.local_id.value && a.identity.scope == b.identity.scope &&
    a.state == b.state && a.archived_from_state == b.archived_from_state &&
    a.begin_unix_epoch_millis == b.begin_unix_epoch_millis &&
    a.final_unix_epoch_millis == b.final_unix_epoch_millis &&
    a.begin_visible_through_local_transaction_id == b.begin_visible_through_local_transaction_id &&
    a.begin_visible_through_commit_sequence == b.begin_visible_through_commit_sequence &&
    a.commit_sequence == b.commit_sequence && a.evidence_record_required == b.evidence_record_required &&
    a.evidence_record_written == b.evidence_record_written && a.rollback_only == b.rollback_only &&
    a.stable_snapshot == b.stable_snapshot;
}
disk::NativePageReference Ref(const disk::NativeCommonPageHeader& h) {
  return {h.filespace_uuid, h.page_number, h.page_generation, h.page_size_profile_uuid};
}
struct Chain {
  page::NativeTransactionInventoryView inventory;
  u64 generation = 0;
};
struct Context {
  Uuid database;
  // Shared across both chains: predecessors cannot be overwritten or acquire
  // a different filespace profile through a candidate image.
  std::pmr::set<std::pair<Uuid,u64>> slots;
  std::pmr::set<Uuid> pages;
  std::pmr::map<Uuid,Uuid> profiles;
  detail::NativeMetadataScratch scratch;
  explicit Context(Uuid id,std::pmr::memory_resource& resource):
    database(id),slots(&resource),pages(&resource),profiles(&resource),scratch(&resource){}
  Chain Read(const Images& images, const NativeCheckpointRootReference& root) {
    Require(root.role == 1 && root.page_type == 0x301 && V7(root.object_uuid) && !images.empty(), E::invalid_request);
    const auto digest = core::hash::ComputeSha256DigestNative(images.front().data(),images.front().size());
    Require(digest.ok(), E::hash_failure);
    Require(digest.digest == root.sha256, E::binding_mismatch);
    Chain result;std::pmr::vector<Entry> entries(scratch.resource);
    std::optional<disk::NativePageReference> previous;
    std::optional<disk::NativePageReference> expected = root.page;
    for (const auto& bytes : images) {
      Require(bytes.size() >= 128, E::invalid_image);
      const auto common = disk::DecodeNativeCommonPageHeader(bytes.data(), 128);
      if (common.error == disk::NativeCommonPageHeaderError::resource_exhausted) throw E::resource_exhausted;
      Require(common.ok(), E::invalid_image);
      // Ciphertext is not a malformed plaintext inventory. The owning crypto
      // boundary must supply authenticated plaintext before family admission.
      if (common.header->flags & 1u) throw E::encrypted_requires_authority;
      auto decoded = scratch.Inventory(bytes);
      if (!decoded.ok()) {
        if (decoded.error == page::NativeInventoryError::hash_failure) throw E::hash_failure;
        if (decoded.error == page::NativeInventoryError::resource_exhausted) throw E::resource_exhausted;
        throw E::invalid_image;
      }
      const auto& image = *decoded.page;
      const auto& h = image.header;
      if (h.flags & 1u) throw E::encrypted_requires_authority;
      Require(h.flags == 0 && h.database_uuid == database && image.object_uuid == root.object_uuid &&
        expected && Ref(h) == *expected, E::binding_mismatch);
      const auto [profile, inserted] = profiles.emplace(h.filespace_uuid, h.page_size_profile_uuid);
      Require(inserted || profile->second == h.page_size_profile_uuid, E::binding_mismatch);
      Require(slots.emplace(h.filespace_uuid, h.page_number).second && pages.insert(h.page_uuid).second &&
        image.previous == previous, E::chain_mismatch);
      if (!previous) {
        result.generation = image.inventory_generation;
        result.inventory.next_local_transaction_id = image.inventory.next_local_transaction_id;
        result.inventory.next_commit_sequence = image.inventory.next_commit_sequence;
      } else Require(result.generation == image.inventory_generation &&
        result.inventory.next_local_transaction_id == image.inventory.next_local_transaction_id &&
        result.inventory.next_commit_sequence == image.inventory.next_commit_sequence, E::chain_mismatch);
      if (!entries.empty() && !image.inventory.entries.empty())
        Require(entries.back().identity.local_id.value < image.inventory.entries.front().identity.local_id.value, E::chain_mismatch);
      entries.insert(entries.end(), image.inventory.entries.begin(), image.inventory.entries.end());
      previous = Ref(h);
      expected = image.next;
    }
    Require(!expected, E::chain_mismatch);
    result.inventory.entries=scratch.Copy<Entry>(entries);
    const auto structure=page::ValidateNativeTransactionInventoryView(result.inventory,
      scratch.Array<std::size_t>(entries.size()),scratch.Array<byte>(entries.size()));
    Require(structure.ok(),E::invalid_image);
    return result;
  }
};
} // namespace
NativeInventoryDeltaViewResult detail::ValidateNativeInventoryPublicationDeltaBacked(
    const Uuid& database, const NativeCheckpointRootReference& before_root,
    const NativeCheckpointRootReference& after_root, u64 generation,
    const Images& before_images, const Images& after_images, u64 budget,std::pmr::memory_resource& resource) noexcept {
  try {
    Require(V7(database) && generation && before_root.object_uuid == after_root.object_uuid &&
      !before_images.empty() && !after_images.empty(), E::invalid_request);
    u64 work = 0;
    for (const auto* chain : {&before_images, &after_images})
      for (const auto& bytes : *chain) {
        Require(bytes.size() <= (budget - work) / 4, E::resource_exhausted);
        work += 4 * bytes.size();
      }
    Context context(database,resource);
    auto before = context.Read(before_images, before_root);
    auto after = context.Read(after_images, after_root);
    Require(after.generation == generation && generation > before.generation, E::binding_mismatch);
    auto& scratch=context.scratch;
    const auto count=std::max(before.inventory.entries.size(),after.inventory.entries.size());
    const auto evolution=page::ValidateNativeTransactionInventoryEvolutionView(before.inventory,after.inventory,
      scratch.Array<std::size_t>(count),scratch.Array<std::size_t>(before.inventory.entries.size()),scratch.Array<byte>(count));
    Require(evolution.ok(),E::invalid_evolution);
    NativeInventoryPublicationDeltaView delta;
    std::pmr::vector<NativeInventoryEntryDifference> differences(&resource);
    const auto& old = before.inventory.entries;
    const auto& next = after.inventory.entries;
    std::size_t i = 0, j = 0;
    u64 allocated = before.inventory.next_local_transaction_id;
    const auto append = [&](const Entry* a, const Entry* b) {
      if ((a && a->identity.scope == mga::TransactionScope::cluster_global) ||
          (b && b->identity.scope == mga::TransactionScope::cluster_global)) delta.cluster_difference = true;
      differences.push_back({a ? std::optional<Entry>{*a} : std::nullopt,
                                   b ? std::optional<Entry>{*b} : std::nullopt});
    };
    while (i < old.size() || j < next.size()) {
      if (j == next.size() || (i < old.size() && old[i].identity.local_id.value < next[j].identity.local_id.value)) {
        append(&old[i++], nullptr);
      } else if (i == old.size() || next[j].identity.local_id.value < old[i].identity.local_id.value) {
        const auto& entry = next[j++];
        Require(entry.identity.local_id.value == allocated && entry.state == mga::TransactionState::created &&
          entry.identity.scope == mga::TransactionScope::local_node && !entry.final_unix_epoch_millis &&
          !entry.commit_sequence && entry.archived_from_state == mga::TransactionState::none &&
          !entry.rollback_only && !entry.evidence_record_written &&
          entry.begin_visible_through_local_transaction_id == allocated - 1 &&
          entry.begin_visible_through_commit_sequence == before.inventory.next_commit_sequence - 1,
          E::starting_allocation_required);
        // Structural admission already proved local_id < after.next_local;
        // therefore this increment cannot wrap even at the u64 boundary.
        ++allocated;
        append(nullptr, &entry);
      } else {
        if (!Same(old[i], next[j])) append(&old[i], &next[j]);
        ++i; ++j;
      }
    }
    Require(allocated == after.inventory.next_local_transaction_id, E::starting_allocation_required);
    delta.before = std::move(before.inventory);
    delta.after = std::move(after.inventory);
    delta.verified_image_bytes = work;
    delta.differences=scratch.Copy<NativeInventoryEntryDifference>(differences);
    return {E::none, std::move(delta)};
  } catch (E error) { return {error, {}}; }
    catch (const std::bad_alloc&) { return {E::resource_exhausted, {}}; }
    catch (const std::length_error&) { return {E::resource_exhausted, {}}; }
    catch (...) { return {E::invalid_image, {}}; }
}

NativeInventoryDeltaViewResult ValidateNativeInventoryPublicationDeltaInto(
    const Uuid& database,const NativeCheckpointRootReference& before_root,
    const NativeCheckpointRootReference& after_root,u64 generation,
    std::span<const std::span<const byte>> before_images,
    std::span<const std::span<const byte>> after_images,u64 budget,std::span<byte> backing) noexcept {
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  bool valid=disjoint(std::span{&database,1})&&disjoint(std::span{&before_root,1})&&
    disjoint(std::span{&after_root,1})&&disjoint(before_images)&&disjoint(after_images);
  for(const auto& image:before_images)valid=valid&&disjoint(image);
  for(const auto& image:after_images)valid=valid&&disjoint(image);
  if(!valid)return {E::invalid_backing,{}};
  detail::NativeMetadataMemory resource(backing);
  auto result=detail::ValidateNativeInventoryPublicationDeltaBacked(database,before_root,after_root,generation,
    before_images,after_images,budget,resource);
  if(result.ok())result.delta->backing_bytes_used=resource.used();return result;
}
NativeInventoryDeltaResult ValidateNativeInventoryPublicationDelta(
    const Uuid& database,const NativeCheckpointRootReference& before_root,
    const NativeCheckpointRootReference& after_root,u64 generation,
    const std::vector<std::vector<byte>>& before_images,
    const std::vector<std::vector<byte>>& after_images,u64 budget) noexcept {
  try{
    detail::NativeMetadataHeapMemory upstream;
    std::pmr::monotonic_buffer_resource resource(&upstream);
    std::pmr::vector<Image> before(&resource),after(&resource);
    before.reserve(before_images.size());after.reserve(after_images.size());
    for(const auto& image:before_images)before.push_back(image);
    for(const auto& image:after_images)after.push_back(image);
    auto view=detail::ValidateNativeInventoryPublicationDeltaBacked(database,before_root,after_root,generation,before,after,budget,resource);
    if(!view.ok())return {view.error,{}};
    const auto& v=*view.delta;NativeInventoryPublicationDelta out;
    out.before.next_local_transaction_id=v.before.next_local_transaction_id;
    out.before.next_commit_sequence=v.before.next_commit_sequence;
    out.before.entries.assign(v.before.entries.begin(),v.before.entries.end());
    out.after.next_local_transaction_id=v.after.next_local_transaction_id;
    out.after.next_commit_sequence=v.after.next_commit_sequence;
    out.after.entries.assign(v.after.entries.begin(),v.after.entries.end());
    out.differences.assign(v.differences.begin(),v.differences.end());
    out.cluster_difference=v.cluster_difference;out.verified_image_bytes=v.verified_image_bytes;
    return {E::none,std::move(out)};
  }catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}
   catch(...){return {E::invalid_image,{}};}
}
} // namespace scratchbird::storage::database
