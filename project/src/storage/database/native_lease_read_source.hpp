// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// What a lease reader needs from its source, independent of which lease
// supplied it (R2 of the drift removal, owner 2026-11-05). Readers take this
// view internally; their public entries still take a lease, so the lease's
// thread affinity is checked on every read. Built only on the reading thread,
// for the duration of one read; every pointer and span borrows the lease.
//
// Roots are keyed by page-zero kind: 1 system state, 2 catalog, 3 allocation
// head, 4 transaction inventory, 5 filespace directory, 6 configuration,
// 7 security, 8 feature catalog, 10 retention. The fixed-root lease supplies
// page zero's roots, which carry no digest. (The selected-checkpoint lease, the
// other source until the checkpoint layer was deleted, W4d, is gone.)
#include "native_fixed_root_memory_lease.hpp"
#include <array>
#include <optional>

namespace scratchbird::storage::database::detail {

struct NativeLeaseRoot {
  u16 kind = 0;
  u32 page_type = 0;
  disk::NativePageReference page;
  Uuid object_uuid;
  std::optional<std::array<byte, 32>> sha256;  // only from a checkpoint
};

struct NativeLeaseReadSource {
  Uuid database_uuid, primary_filespace_uuid;
  bool cluster_authority_required = false;
  std::span<const disk::NativeFilespaceDevice> devices;
  disk::FileDevice::ReadLatencyBatch* batches = nullptr;
  std::size_t batch_count = 0;
  std::array<std::optional<NativeLeaseRoot>, 11> roots;  // by page-zero kind
  const NativeReadTransactionInventory* inventory = nullptr;
  u64 inventory_generation = 0;
  const NativeReadAllocationChain* allocation = nullptr;  // the primary file's
  const NativeReadDirectoryChain* directory = nullptr;
  u64 retained_image_bytes = 0;
  const NativeLeaseRoot* Root(u16 kind) const noexcept {
    return kind < roots.size() && roots[kind] ? &*roots[kind] : nullptr;
  }
};

// Checkpoint role -> page-zero kind (0: none).
constexpr u16 NativeCheckpointRoleKind(u16 role) noexcept {
  switch (role) {
    case 1: return 4;
    case 3: return 5;
    case 4: return 3;
    case 5: return 2;
    case 6: return 6;
    case 7: return 7;
    case 8: return 1;
    case 9: return 8;
    case 10: return 10;
    default: return 0;
  }
}

struct NativeLeaseSourceAccess {
  static NativeLeaseReadSource From(const NativeFixedRootMemoryLease& lease) noexcept {
    lease.CheckThread();
    NativeLeaseReadSource s;
    s.database_uuid = lease.identity_.database_uuid;
    s.primary_filespace_uuid = lease.identity_.filespace_uuid;
    s.cluster_authority_required = lease.identity_.cluster_authority_required();
    s.devices = lease.devices_;
    s.batches = lease.batches_;
    s.batch_count = lease.batch_count_;
    for (const auto& r : lease.roots_)
      if (r.kind < s.roots.size() && r.kind != 9 && r.kind != 0)
        s.roots[r.kind] = NativeLeaseRoot{r.kind, r.page_type,
                                          {r.filespace_uuid, r.page_number, r.page_generation, r.page_size_profile_uuid},
                                          r.object_uuid, std::nullopt};
    s.inventory = &lease.inventory_;
    s.inventory_generation = 1;
    s.allocation = &lease.allocation_;
    s.directory = &lease.directory_;
    s.retained_image_bytes = lease.retained_image_bytes_;
    return s;
  }
};

}  // namespace scratchbird::storage::database::detail
