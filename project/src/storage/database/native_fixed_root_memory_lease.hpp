// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// Read-only source built from page zero's fixed roots, with no selection step
// (owner 2026-11-05: the checkpoint/selector/watermark layer is drift; open
// reads page zero's roots directly; R1 of the drift removal). It replaces the
// selected-checkpoint memory lease for every consumer as they move over (R2).
//
// What is read, in this order, under every device's operation guard:
//  - the primary file's page zero (bootstrap + root directory, hash-sealed);
//  - the transaction inventory from root kind 4, which must be the in-place
//    inventory (SB_COMPACT_TRANSACTION_STATE_PAGE); any other family is
//    refused by the in-place reader itself (invalid_family; the SBTINV01
//    family was removed in W4d-3);
//  - the primary allocation chain from root kind 3 (bootstrap context: the
//    reader takes the head from page zero itself);
//  - the filespace directory chain from root kind 5.
// What is checked beyond each reader's own checks: every fixed root (kinds
// 1-8 required, 10 when present) and every inventory page is an allocated page
// of the root's page type, owned by the root's object, created by a committed
// local transaction; the allocation maps and the directory were created by
// committed local transactions. Kinds 9 and 18-21 (checkpoint, selectors,
// watermarks) are ignored: they are the drift layer, retired by R3/R4.
// Page-zero roots carry no digest: each content page keeps its own integrity
// check, and a stale page that is valid in itself is not detected through its
// root (the Firebird careful-write model, owner rulings 2026-11-05).
//
// Read-only: nothing is written and no lock is taken beyond the device
// operation guards a read needs. Thread-affine: the lease may be used only on
// the thread that acquired it.
#include "native_page_chain_readonly.hpp"
#include "native_allocation_chain_backing.hpp"
#include "native_directory_chain_backing.hpp"
#include "native_inventory_chain_backing.hpp"
#include "transaction_header_page.hpp"
#include "transaction_state_page.hpp"
#include "native_storage_memory.hpp"
#include "native_read_source_identity.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

namespace scratchbird::storage::database {

struct NativeFixedRootMemoryLimits {
  u64 maximum_verification_image_bytes = 0;  // images retained by the lease
  std::size_t maximum_backing_bytes = 0;     // the lease's governed backing
};

enum class NativeFixedRootMemoryError {
  none, invalid_request, invalid_device, memory_binding_failure, memory_allocation_failure,
  page_zero_failure, encrypted_requires_authority, missing_root, inventory_failure, allocation_failure,
  directory_failure, root_mismatch, creator_mismatch, creator_not_committed, resource_exhausted,
  lock_failure, io_failure
};

// The source identity a consumer compares instead of a checkpoint selection:
// the primary page zero's own identity.
struct NativeFixedRootIdentity {
  Uuid database_uuid, filespace_uuid, page_size_profile_uuid;
  Uuid page_zero_uuid, creation_operation_uuid;
  u64 page_generation = 0, root_set_generation = 0, total_pages = 0, creation_utc_millis = 0;
  u32 page_size_bytes = 0, bootstrap_flags = 0;
  u16 lifecycle_state = 0;
  bool cluster_authority_required() const noexcept {
    return (bootstrap_flags & disk::FilespaceBootstrapFlag::cluster_authority_required) != 0;
  }
  bool operator==(const NativeFixedRootIdentity&) const = default;
};

// The neutral identity of a fixed-root source (see native_read_source_identity.hpp).
inline NativeReadSourceIdentity NativeReadSourceIdentityFromFixedRoot(const NativeFixedRootIdentity& id) noexcept {
  NativeReadSourceIdentity out;
  out.kind = NativeReadSourceKind::fixed_root;
  out.database_uuid = id.database_uuid;
  out.filespace_uuid = id.filespace_uuid;
  out.page_size_bytes = id.page_size_bytes;
  out.root_set_uuid = id.page_zero_uuid;
  out.source_generation = id.page_generation;
  out.root_set_generation = id.root_set_generation;
  return out;
}

struct NativeFixedRootMemoryResult;
class NativeFixedRootMemoryHandle;
namespace detail {
struct NativeLeaseSourceAccess;
NativeFixedRootMemoryResult AcquireNativeFixedRootLeaseOn(
    const Uuid&, std::span<const disk::NativeFilespaceDevice>, const Uuid&, NativeFixedRootMemoryLimits,
    std::span<core::platform::byte>, NativeStorageArena, NativeFixedRootMemoryResult) noexcept;
}  // namespace detail

class NativeFixedRootMemoryLease {
 public:
  NativeFixedRootMemoryLease(const NativeFixedRootMemoryLease&) = delete;
  NativeFixedRootMemoryLease& operator=(const NativeFixedRootMemoryLease&) = delete;
  const NativeFixedRootIdentity& identity() const noexcept { CheckThread(); return identity_; }
  // The primary page zero's roots, as stored.
  std::span<const disk::FilespaceRootReference> roots() const noexcept { CheckThread(); return roots_; }
  const disk::FilespaceRootReference* Root(u16 kind) const noexcept {
    CheckThread();
    const auto at = std::find_if(roots_.begin(), roots_.end(), [&](const auto& r) { return r.kind == kind; });
    return at == roots_.end() ? nullptr : &*at;
  }
  const NativeReadTransactionInventory& inventory() const noexcept { CheckThread(); return inventory_; }
  // The counters and horizons (OIT, OAT, OST, ...) the first state page stores.
  const page::TipCounters& counters() const noexcept { CheckThread(); return counters_; }
  // State pages, then header pages.
  std::span<const NativeInventoryPageBinding> inventory_pages() const noexcept { CheckThread(); return inventory_pages_; }
  // Each inventory page's sequence within its own chain (state chain, then
  // header chain, as inventory_pages()).
  std::span<const u64> inventory_page_sequences() const noexcept { CheckThread(); return inventory_sequences_; }
  const NativeReadAllocationChain& allocation() const noexcept { CheckThread(); return allocation_; }
  const NativeReadDirectoryChain& directory() const noexcept { CheckThread(); return directory_; }
  u64 retained_image_bytes() const noexcept { CheckThread(); return retained_image_bytes_; }
  std::size_t backing_bytes_used() const noexcept { CheckThread(); return backing_bytes_used_; }

 private:
  using Batch = disk::FileDevice::ReadLatencyBatch;
  using Guard = std::unique_lock<std::recursive_mutex>;
  NativeFixedRootMemoryLease() noexcept : thread_(std::this_thread::get_id()) {}
  ~NativeFixedRootMemoryLease() { ReleaseDeviceGuards(); }
  void ReleaseDeviceGuards() noexcept {
    while (guard_count_) std::destroy_at(guards_ + --guard_count_);
    while (batch_count_) std::destroy_at(batches_ + --batch_count_);
  }
  void CheckThread() const noexcept { if (thread_ != std::this_thread::get_id()) std::terminate(); }
  std::thread::id thread_;
  Guard* guards_ = nullptr;
  std::size_t guard_count_ = 0;
  Batch* batches_ = nullptr;
  std::size_t batch_count_ = 0;
  std::span<const disk::NativeFilespaceDevice> devices_;
  NativeFixedRootIdentity identity_;
  std::span<const disk::FilespaceRootReference> roots_;
  NativeReadTransactionInventory inventory_;
  page::TipCounters counters_;
  std::span<const NativeInventoryPageBinding> inventory_pages_;
  std::span<const u64> inventory_sequences_;
  NativeReadAllocationChain allocation_;
  NativeReadDirectoryChain directory_;
  u64 retained_image_bytes_ = 0;
  std::size_t backing_bytes_used_ = 0;
  friend class NativeFixedRootMemoryHandle;
  friend struct detail::NativeLeaseSourceAccess;
  friend NativeFixedRootMemoryResult detail::AcquireNativeFixedRootLeaseOn(
      const Uuid&, std::span<const disk::NativeFilespaceDevice>, const Uuid&, NativeFixedRootMemoryLimits,
      std::span<core::platform::byte>, NativeStorageArena, NativeFixedRootMemoryResult) noexcept;
};

// Owns the lease and its backing; the backing cannot outlive or be separated
// from the lease.
class NativeFixedRootMemoryHandle {
 public:
  NativeFixedRootMemoryHandle() noexcept = default;
  NativeFixedRootMemoryHandle(const NativeFixedRootMemoryHandle&) = delete;
  NativeFixedRootMemoryHandle& operator=(const NativeFixedRootMemoryHandle&) = delete;
  NativeFixedRootMemoryHandle(NativeFixedRootMemoryHandle&& other) noexcept {
    other.CheckThread();
    arena_ = std::move(other.arena_);
    lease_ = std::exchange(other.lease_, nullptr);
  }
  NativeFixedRootMemoryHandle& operator=(NativeFixedRootMemoryHandle&& other) noexcept {
    if (this != &other) {
      other.CheckThread();
      Reset();
      arena_ = std::move(other.arena_);
      lease_ = std::exchange(other.lease_, nullptr);
    }
    return *this;
  }
  ~NativeFixedRootMemoryHandle() { Reset(); }
  explicit operator bool() const noexcept { CheckThread(); return lease_ != nullptr; }
  const NativeFixedRootMemoryLease* operator->() const noexcept { CheckThread(); return lease_; }
  const NativeFixedRootMemoryLease& operator*() const noexcept { CheckThread(); return *lease_; }
  // Ends the device guards early; the retained images stay readable, but no
  // reader may be run on the lease afterwards.
  void ReleaseDeviceGuards() noexcept {
    CheckThread();
    if (lease_) lease_->ReleaseDeviceGuards();
  }
  void Reset() noexcept {
    CheckThread();
    if (lease_) {
      lease_->~NativeFixedRootMemoryLease();
      lease_ = nullptr;
    }
    arena_ = NativeStorageArena{};
  }

 private:
  void CheckThread() const noexcept { if (lease_) lease_->CheckThread(); }
  NativeStorageArena arena_;
  NativeFixedRootMemoryLease* lease_ = nullptr;
  friend NativeFixedRootMemoryResult detail::AcquireNativeFixedRootLeaseOn(
      const Uuid&, std::span<const disk::NativeFilespaceDevice>, const Uuid&, NativeFixedRootMemoryLimits,
      std::span<core::platform::byte>, NativeStorageArena, NativeFixedRootMemoryResult) noexcept;
};

struct NativeFixedRootMemoryResult {
  NativeFixedRootMemoryError error = NativeFixedRootMemoryError::invalid_request;
  NativeStorageMemoryError memory_error = NativeStorageMemoryError::none;
  disk::FilespacePageZeroError page_zero_error = disk::FilespacePageZeroError::none;
  page::NativeInventoryError inventory_error = page::NativeInventoryError::none;
  page::NativeAllocationError allocation_error = page::NativeAllocationError::none;
  page::NativeDirectoryError directory_error = page::NativeDirectoryError::none;
  std::optional<u16> root_kind;  // the root a membership or creator finding is about
  core::platform::Status allocation_status, io_status;
  core::platform::DiagnosticRecord allocation_diagnostic, io_diagnostic;
  u64 physical_bytes_read = 0;
  NativeFixedRootMemoryHandle lease;
  bool ok() const noexcept { return error == NativeFixedRootMemoryError::none && bool(lease); }
};

namespace detail {
// The allocation record of page `number` in `chain`, or nullptr.
inline const page::NativeAllocationRecord* NativeFixedRootRecord(const NativeReadAllocationChain& chain,
                                                                 u64 number) noexcept {
  for (const auto& image : chain.pages) {
    const auto& map = image.map;
    if (number < map.first_page || number - map.first_page >= map.states.size()) continue;
    if (map.states[number - map.first_page] != page::NativeAllocationState::allocated) return nullptr;
    const auto r = std::lower_bound(map.records.begin(), map.records.end(), number,
                                    [](const auto& rec, u64 n) { return rec.page_number < n; });
    return r != map.records.end() && r->page_number == number ? &*r : nullptr;
  }
  return nullptr;
}
}  // namespace detail

namespace detail {
// The request checks every acquisition makes before it touches memory or I/O.
inline NativeFixedRootMemoryError CheckNativeFixedRootRequest(
    const Uuid& database, std::span<const disk::NativeFilespaceDevice> files, const Uuid& primary,
    NativeFixedRootMemoryLimits limits) noexcept {
  using E = NativeFixedRootMemoryError;
  if (!core::uuid::IsEngineIdentityUuid(database) || !core::uuid::IsEngineIdentityUuid(primary) || files.empty() ||
      !limits.maximum_verification_image_bytes || !limits.maximum_backing_bytes)
    return E::invalid_request;
  bool found = false;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    if (!f.device || !core::uuid::IsEngineIdentityUuid(f.filespace_uuid) ||
        !disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)) {
      return E::invalid_device;
    }
    found |= f.filespace_uuid == primary;
    for (std::size_t j = 0; j < i; ++j)
      if (files[j].device == f.device || files[j].filespace_uuid == f.filespace_uuid) {
        return E::invalid_device;
      }
  }
  if (!found) {
    return E::invalid_device;
  }
  return E::none;
}

// The acquisition itself, over `backing`. A governed lease passes the arena
// that owns `backing`; it moves into the handle. A backed lease passes an empty
// arena, and its caller keeps `backing` alive until the lease is reset.
inline NativeFixedRootMemoryResult AcquireNativeFixedRootLeaseOn(
    const Uuid& database, std::span<const disk::NativeFilespaceDevice> files, const Uuid& primary,
    NativeFixedRootMemoryLimits limits, std::span<core::platform::byte> backing, NativeStorageArena arena,
    NativeFixedRootMemoryResult out) noexcept {
  using E = NativeFixedRootMemoryError;
  using L = NativeFixedRootMemoryLease;
  namespace mga = scratchbird::transaction::mga;
  try {
    detail::NativeMetadataMemory resource({backing.data(), backing.size()});
    detail::NativeMetadataScratch scratch{&resource};
    NativeFixedRootMemoryHandle handle;
    handle.arena_ = std::move(arena);
    handle.lease_ = new (resource.allocate(sizeof(L), alignof(L))) L;
    auto& lease = *handle.lease_;
    auto devices = scratch.Copy<disk::NativeFilespaceDevice>(files);
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return a.filespace_uuid < b.filespace_uuid; });
    lease.devices_ = devices;
    auto pointers = scratch.Array<L::Batch*>(devices.size());
    if (devices.size() > std::numeric_limits<std::size_t>::max() / sizeof(L::Batch) ||
        devices.size() > std::numeric_limits<std::size_t>::max() / sizeof(L::Guard))
      throw std::bad_alloc();
    lease.batches_ = static_cast<L::Batch*>(resource.allocate(devices.size() * sizeof(L::Batch), alignof(L::Batch)));
    lease.guards_ = static_cast<L::Guard*>(resource.allocate(devices.size() * sizeof(L::Guard), alignof(L::Guard)));
    for (std::size_t i = 0; i < devices.size(); ++i) {
      pointers[i] = std::construct_at(lease.batches_ + i, *devices[i].device);
      ++lease.batch_count_;
    }
    for (const auto& device : devices) {
      std::construct_at(lease.guards_ + lease.guard_count_, device.device->AcquireOperationGuard());
      ++lease.guard_count_;
    }
    const auto owner = std::lower_bound(devices.begin(), devices.end(), primary,
                                        [](const auto& f, const auto& id) { return f.filespace_uuid < id; });
    const std::size_t primary_index = static_cast<std::size_t>(owner - devices.begin());
    auto& batch = *pointers[primary_index];
    const auto merge = [&](auto& read) {
      out.io_status = read.io_status;
      out.io_diagnostic = std::move(read.io_diagnostic);
      if (read.physical_bytes_read > std::numeric_limits<u64>::max() - out.physical_bytes_read)
        throw std::length_error("physical bytes");
      out.physical_bytes_read += read.physical_bytes_read;
    };
    u64 retained = 0;
    const auto charge = [&](u64 bytes) {
      if (bytes > limits.maximum_verification_image_bytes - retained) throw E::resource_exhausted;
      retained += bytes;
    };

    // 1. Page zero of the primary file.
    const disk::FilespaceBootstrapBinding zero_binding{database, primary, owner->page_size_profile_uuid};
    std::span<const byte> zero_image;
    const auto zero = disk::detail::ReadNativeMetadataPageZero(
        zero_binding, scratch,
        [&](u64 offset, void* data, std::size_t count) {
          auto io = batch.ReadAtBounded(offset, data, count);
          out.physical_bytes_read += io.bytes_transferred;
          if (!io.ok()) out.io_status = disk::BoundedIoStatus(io);
          return io;
        },
        [&] {
          auto io = batch.SizeBounded();
          if (!io.ok()) out.io_status = disk::BoundedIoStatus(io);
          return io;
        },
        zero_image);
    if (!zero.ok()) {
      out.page_zero_error = zero.error;
      out.error = zero.error == disk::FilespacePageZeroError::io_failure            ? E::io_failure
                  : zero.error == disk::FilespacePageZeroError::resource_exhausted ? E::resource_exhausted
                                                                                    : E::page_zero_failure;
      return out;
    }
    const auto& z = *zero.record;
    if (z.bootstrap.flags & disk::FilespaceBootstrapFlag::payload_encrypted) {
      out.error = E::encrypted_requires_authority;
      return out;
    }
    if (z.bootstrap.filespace_role < 1 || z.bootstrap.filespace_role > 4) {
      out.error = E::page_zero_failure;
      return out;
    }
    charge(zero_image.size());
    auto& id = lease.identity_;
    id.database_uuid = z.bootstrap.database_uuid;
    id.filespace_uuid = z.bootstrap.filespace_uuid;
    id.page_size_profile_uuid = z.bootstrap.page_size_profile_uuid;
    id.page_size_bytes = z.bootstrap.page_size_bytes;
    id.page_zero_uuid = z.page_uuid;
    id.creation_operation_uuid = z.creation_operation_uuid;
    id.page_generation = z.page_generation;
    id.root_set_generation = z.root_set_generation;
    id.total_pages = z.total_pages;
    id.creation_utc_millis = z.creation_utc_millis;
    id.bootstrap_flags = z.bootstrap.flags;
    id.lifecycle_state = z.bootstrap.lifecycle_state;
    lease.roots_ = scratch.Copy<disk::FilespaceRootReference>(z.roots);
    for (const u16 kind : {u16{1}, u16{2}, u16{3}, u16{4}, u16{5}, u16{6}, u16{7}, u16{8}})
      if (!lease.Root(kind)) {
        out.error = E::missing_root;
        out.root_kind = kind;
        return out;
      }

    // 2. The in-place transaction inventory (kind 4).
    {
      const auto& root = *lease.Root(4);
      auto read = page::detail::ReadNativeInPlaceInventoryBacked(database, devices, root,
                                                                 limits.maximum_verification_image_bytes - retained,
                                                                 pointers, resource);
      merge(read.read);
      out.inventory_error = read.read.chain.error;
      const auto& chain = read.read.chain;
      if (!chain.ok()) {
        out.error = chain.error == page::NativeInventoryError::io_failure            ? E::io_failure
                    : chain.error == page::NativeInventoryError::resource_exhausted ? E::resource_exhausted
                    : chain.error == page::NativeInventoryError::encrypted_requires_crypto_authority
                        ? E::encrypted_requires_authority
                        : E::inventory_failure;
        return out;
      }
      charge(chain.retained_image_bytes);
      lease.inventory_ = {chain.inventory.next_local_transaction_id, chain.inventory.next_commit_sequence,
                          chain.inventory.entries};
      if (!read.counters) {
        out.error = E::inventory_failure;
        return out;
      }
      lease.counters_ = *read.counters;
      auto pages = scratch.Array<NativeInventoryPageBinding>(chain.pages.size());
      for (std::size_t n = 0; n < pages.size(); ++n) pages[n] = {chain.pages[n].page.header, chain.pages[n].page.object_uuid};
      lease.inventory_pages_ = pages;
      // SB_ALLOCATION_MAP_IN_PLACE (W2c): each page's place in its chain; the
      // first page of each chain is the create's, later pages were grown.
      auto sequences = scratch.Array<u64>(chain.pages.size());
      for (std::size_t n = 0; n < sequences.size(); ++n) {
        const auto image = chain.pages[n].image;
        if (page::IsTransactionHeaderPageImage(image)) {
          const auto d = page::DecodeTransactionHeaderPage(image);
          if (!d.ok()) { out.error = E::inventory_failure; return out; }
          sequences[n] = d.view->sequence;
        } else {
          const auto d = page::DecodeTransactionStatePage(image);
          if (!d.ok()) { out.error = E::inventory_failure; return out; }
          sequences[n] = d.view->tip_sequence;
        }
      }
      lease.inventory_sequences_ = sequences;
    }
    const auto committed = [&](const Uuid& creator, u64 local, u16 kind) {
      const auto& entries = lease.inventory_.entries;
      const auto at = std::lower_bound(entries.begin(), entries.end(), local,
                                       [](const auto& e, u64 n) { return e.identity.local_id.value < n; });
      if (!mga::MakeLocalTransactionId(local).valid() || at == entries.end() || at->identity.local_id.value != local ||
          at->identity.transaction_uuid.value != creator) {
        out.root_kind = kind;
        throw E::creator_mismatch;
      }
      if (at->identity.scope != mga::TransactionScope::local_node || !mga::HasCommittedInventoryOutcome(*at)) {
        out.root_kind = kind;
        throw E::creator_not_committed;
      }
    };

    // 3. The primary allocation chain (kind 3, read from page zero itself).
    {
      auto read = page::detail::ReadNativeAllocationChainBacked(
          *owner->device, zero_binding, limits.maximum_verification_image_bytes - retained, batch, resource);
      merge(read);
      out.allocation_error = read.chain.error;
      if (!read.chain.ok()) {
        out.error = read.chain.error == page::NativeAllocationError::io_failure            ? E::io_failure
                    : read.chain.error == page::NativeAllocationError::resource_exhausted ? E::resource_exhausted
                                                                                          : E::allocation_failure;
        return out;
      }
      charge(read.chain.retained_image_bytes);
      lease.allocation_ = detail::ReadOnly(read.chain, scratch);
      for (const auto& image : lease.allocation_.pages) {
        if (!image.map.creator_operation_uuid.is_nil()) {
          out.root_kind = 3;
          throw E::creator_mismatch;
        }
        committed(image.map.creator_transaction_uuid, image.map.creator_local_transaction_id, 3);
      }
    }

    // 4. The filespace directory chain (kind 5).
    {
      auto read = page::detail::ReadNativeDirectoryChainBacked(
          database, devices, *lease.Root(5), limits.maximum_verification_image_bytes - retained,
          page::NativeDirectoryChainReadContext::current, nullptr, {}, pointers, resource);
      merge(read);
      out.directory_error = read.chain.error;
      if (!read.chain.ok()) {
        out.error = read.chain.error == page::NativeDirectoryError::io_failure            ? E::io_failure
                    : read.chain.error == page::NativeDirectoryError::resource_exhausted ? E::resource_exhausted
                                                                                         : E::directory_failure;
        return out;
      }
      charge(read.chain.retained_image_bytes);
      lease.directory_ = detail::ReadOnly(read.chain, scratch);
      for (const auto& image : lease.directory_.pages) {
        if (!image.directory.creator_operation_uuid.is_nil()) {
          out.root_kind = 5;
          throw E::creator_mismatch;
        }
        committed(image.directory.creator_transaction_uuid, image.directory.creator_local_transaction_id, 5);
      }
    }

    // 5. Every fixed root and every inventory page is an allocated page of the
    //    root's type, owned by the root's object, created by a committed
    //    local transaction.
    const auto member = [&](u64 number, u32 type, const Uuid& owner_uuid, u16 kind) {
      const auto* record = detail::NativeFixedRootRecord(lease.allocation_, number);
      if (!record || record->page_type != type || record->owner_uuid != owner_uuid) {
        out.root_kind = kind;
        throw E::root_mismatch;
      }
      if (!record->creator_operation_uuid.is_nil()) {
        out.root_kind = kind;
        throw E::creator_mismatch;
      }
      committed(record->creator_transaction_uuid, record->creator_local_transaction_id, kind);
    };
    for (const auto& root : lease.roots_) {
      if (root.kind > 10 || root.kind == 9) continue;
      if (root.filespace_uuid != primary || root.page_size_profile_uuid != owner->page_size_profile_uuid) {
        out.root_kind = root.kind;
        throw E::root_mismatch;
      }
      member(root.page_number, root.page_type, root.object_uuid, root.kind);
    }
    // The first page of each inventory chain is the create's (a committed
    // transaction); a later page was grown by the system (W2c, owner Q22 = 1)
    // and its record has no creator at all.
    for (std::size_t n = 0; n < lease.inventory_pages_.size(); ++n) {
      const auto& page = lease.inventory_pages_[n];
      if (page.header.filespace_uuid != primary) {
        out.root_kind = 4;
        throw E::root_mismatch;
      }
      if (lease.inventory_sequences_[n] == 0) {
        member(page.header.page_number, page.header.page_type, page.object_uuid, 4);
        continue;
      }
      const auto* record = detail::NativeFixedRootRecord(lease.allocation_, page.header.page_number);
      if (!record || record->page_type != page.header.page_type || record->owner_uuid != page.object_uuid ||
          record->page_uuid != page.header.page_uuid || record->page_generation != page.header.page_generation) {
        out.root_kind = 4;
        throw E::root_mismatch;
      }
      if (!record->creator_transaction_uuid.is_nil() || record->creator_local_transaction_id ||
          !record->creator_operation_uuid.is_nil()) {
        out.root_kind = 4;
        throw E::creator_mismatch;
      }
    }
    lease.retained_image_bytes_ = retained;
    lease.backing_bytes_used_ = resource.used();
    out.lease = std::move(handle);
    out.error = E::none;
    return out;
  } catch (E e) {
    out.error = e;
  } catch (const std::bad_alloc&) {
    out.error = E::resource_exhausted;
  } catch (const std::length_error&) {
    out.error = E::resource_exhausted;
  } catch (const std::system_error&) {
    out.error = E::lock_failure;
  } catch (...) {
    out.error = E::io_failure;
  }
  return out;
}
}  // namespace detail

inline NativeFixedRootMemoryResult AcquireNativeFixedRootMemoryLease(
    const Uuid& database, std::span<const disk::NativeFilespaceDevice> files, const Uuid& primary,
    NativeFixedRootMemoryLimits limits, NativeStorageMemory& memory, const NativeStorageMemoryBinding& binding) noexcept {
  using E = NativeFixedRootMemoryError;
  NativeFixedRootMemoryResult out;
  try {
    out.memory_error = memory.CheckBinding(binding);
    if (out.memory_error == NativeStorageMemoryError::none && binding.database_uuid != database)
      out.memory_error = NativeStorageMemoryError::invalid_binding;
    if (out.memory_error != NativeStorageMemoryError::none) {
      out.error = E::memory_binding_failure;
      return out;
    }
    if (const auto error = detail::CheckNativeFixedRootRequest(database, files, primary, limits); error != E::none) {
      out.error = error;
      return out;
    }
    auto payload = memory.CreateArena(binding, limits.maximum_backing_bytes, alignof(std::max_align_t));
    out.memory_error = payload.error;
    out.allocation_status = payload.backing.status;
    out.allocation_diagnostic = std::move(payload.backing.diagnostic);
    if (!payload.ok()) {
      out.error = E::memory_allocation_failure;
      return out;
    }
    auto block = payload.arena.Allocate(limits.maximum_backing_bytes, alignof(std::max_align_t));
    if (!block.ok()) {
      out.error = E::resource_exhausted;
      return out;
    }
    return detail::AcquireNativeFixedRootLeaseOn(database, files, primary, limits,
                                                 {static_cast<byte*>(block.pointer), block.bytes},
                                                 std::move(payload.arena), std::move(out));
  } catch (const std::bad_alloc&) {
    out.error = E::resource_exhausted;
  } catch (const std::length_error&) {
    out.error = E::resource_exhausted;
  } catch (...) {
    out.error = E::io_failure;
  }
  return out;
}

// SB_NATIVE_FIXED_ROOT_READ: the same acquisition without a memory grant, over
// a backing the caller supplies (the native create's self-check, which runs
// before any memory governor exists). The caller keeps `backing` alive until
// the lease is reset; at most limits.maximum_backing_bytes of it is used.
inline NativeFixedRootMemoryResult AcquireNativeFixedRootBackedLease(
    const Uuid& database, std::span<const disk::NativeFilespaceDevice> files, const Uuid& primary,
    NativeFixedRootMemoryLimits limits, std::span<core::platform::byte> backing) noexcept {
  using E = NativeFixedRootMemoryError;
  NativeFixedRootMemoryResult out;
  if (const auto error = detail::CheckNativeFixedRootRequest(database, files, primary, limits); error != E::none) {
    out.error = error;
    return out;
  }
  if (!backing.data() || backing.size() < limits.maximum_backing_bytes) {
    out.error = E::resource_exhausted;
    return out;
  }
  return detail::AcquireNativeFixedRootLeaseOn(database, files, primary, limits,
                                               backing.first(limits.maximum_backing_bytes), NativeStorageArena{},
                                               std::move(out));
}

}  // namespace scratchbird::storage::database
