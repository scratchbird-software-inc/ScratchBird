// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_read_source_identity.hpp"
#include "transaction/mga/transaction_horizon.hpp"

namespace scratchbird::engine::internal_api {
// Values copied only after the owning verifier reads the complete selected
// allocation chains and inventory. These observations cannot grant custody,
// advance a phase, release a retention boundary or confer readiness.
struct NativeStartupStorageScanObservation {
  storage::database::NativeReadSourceIdentity source;
  core::platform::u64 filespaces=0,allocation_pages=0,capacity_pages=0;
  core::platform::u64 catalog_pages=0;
  core::platform::u64 initial_control_pages=0;
  std::array<core::platform::u64,8> allocation_states{};
  core::platform::u64 inventory_pages=0,transactions=0,active_transactions=0,in_doubt_transactions=0;
  core::platform::u64 horizon_records=0,retention_pins=0;
  core::platform::u64 physical_bytes_read=0;
  transaction::mga::LocalTransactionHorizons horizons;
};
// The two separately rooted configuration/security routes must resolve to the
// same actual catalog relations already traversed under the retained snapshot.
// This observation is one catalog component, not complete reference closure.
struct NativeStartupCatalogRouteObservation {
  storage::database::NativeReadSourceIdentity source;
  core::platform::u64 routes=0,physical_bytes_read=0;
};
} // namespace scratchbird::engine::internal_api
