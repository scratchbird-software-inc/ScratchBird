// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_inventory_publication_delta.hpp"
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
using Images = std::vector<std::vector<byte>>;
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
    a.evidence_record_written == b.evidence_record_written && a.rollback_only == b.rollback_only;
}
disk::NativePageReference Ref(const disk::NativeCommonPageHeader& h) {
  return {h.filespace_uuid, h.page_number, h.page_generation, h.page_size_profile_uuid};
}
struct Chain {
  mga::LocalTransactionInventory inventory;
  u64 generation = 0;
};
struct Context {
  Uuid database;
  // Shared across both chains: predecessors cannot be overwritten or acquire
  // a different filespace profile through a candidate image.
  std::set<std::pair<Uuid,u64>> slots;
  std::set<Uuid> pages;
  std::map<Uuid,Uuid> profiles;
  Chain Read(const Images& images, const NativeCheckpointRootReference& root) {
    Require(root.role == 1 && root.page_type == 0x301 && V7(root.object_uuid) && !images.empty(), E::invalid_request);
    const auto digest = core::hash::ComputeSha256Digest(images.front());
    Require(digest.ok(), E::hash_failure);
    Require(digest.digest == root.sha256, E::binding_mismatch);
    Chain result;
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
      auto decoded = page::DecodeNativeTransactionInventoryPage(bytes);
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
      if (!result.inventory.entries.empty() && !image.inventory.entries.empty())
        Require(result.inventory.entries.back().identity.local_id.value < image.inventory.entries.front().identity.local_id.value, E::chain_mismatch);
      result.inventory.entries.insert(result.inventory.entries.end(), image.inventory.entries.begin(), image.inventory.entries.end());
      previous = Ref(h);
      expected = image.next;
    }
    Require(!expected, E::chain_mismatch);
    Require(!*mga::ValidateLocalTransactionInventoryStructure(result.inventory), E::invalid_image);
    return result;
  }
};
} // namespace
NativeInventoryDeltaResult ValidateNativeInventoryPublicationDelta(
    const Uuid& database, const NativeCheckpointRootReference& before_root,
    const NativeCheckpointRootReference& after_root, u64 generation,
    const Images& before_images, const Images& after_images, u64 budget) noexcept {
  try {
    Require(V7(database) && generation && before_root.object_uuid == after_root.object_uuid &&
      !before_images.empty() && !after_images.empty(), E::invalid_request);
    u64 work = 0;
    for (const auto* chain : {&before_images, &after_images})
      for (const auto& bytes : *chain) {
        Require(bytes.size() <= (budget - work) / 4, E::resource_exhausted);
        work += 4 * bytes.size();
      }
    Context context{database, {}, {}, {}};
    auto before = context.Read(before_images, before_root);
    auto after = context.Read(after_images, after_root);
    Require(after.generation == generation && generation > before.generation, E::binding_mismatch);
    Require(!*mga::ValidateLocalTransactionInventoryEvolution(before.inventory, after.inventory), E::invalid_evolution);
    NativeInventoryPublicationDelta delta;
    const auto& old = before.inventory.entries;
    const auto& next = after.inventory.entries;
    std::size_t i = 0, j = 0;
    u64 allocated = before.inventory.next_local_transaction_id;
    const auto append = [&](const Entry* a, const Entry* b) {
      if ((a && a->identity.scope == mga::TransactionScope::cluster_global) ||
          (b && b->identity.scope == mga::TransactionScope::cluster_global)) delta.cluster_difference = true;
      delta.differences.push_back({a ? std::optional<Entry>{*a} : std::nullopt,
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
    return {E::none, std::move(delta)};
  } catch (E error) { return {error, {}}; }
    catch (const std::bad_alloc&) { return {E::resource_exhausted, {}}; }
    catch (const std::length_error&) { return {E::resource_exhausted, {}}; }
    catch (...) { return {E::invalid_image, {}}; }
}
} // namespace scratchbird::storage::database
