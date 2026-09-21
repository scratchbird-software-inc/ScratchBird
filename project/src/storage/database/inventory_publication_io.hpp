// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_platform.hpp"
#include <cstdint>

namespace scratchbird::storage::database {
enum class InventoryPageSyncPolicy : std::uint8_t { batched = 1, per_page = 2 };

// Native successful-publication observation, not an ingress capability or
// recovery/finality authority. Counts cover inventory BODY writes and explicit
// device Sync calls only, excluding page headers, journals and row/index I/O.
struct InventoryPublicationIo {
  bool complete = false;
  InventoryPageSyncPolicy sync_policy = InventoryPageSyncPolicy::batched;
  core::platform::Uuid database_uuid;
  core::platform::Uuid filespace_uuid;
  std::uint64_t inventory_generation = 0;
  std::uint64_t publications = 0;
  std::uint64_t page_body_writes = 0;
  std::uint64_t body_bytes_written = 0;
  std::uint64_t page_sync_calls = 0;
};
}  // namespace scratchbird::storage::database
