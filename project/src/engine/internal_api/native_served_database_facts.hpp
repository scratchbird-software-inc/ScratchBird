// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_SERVED_DATABASE_FACTS (thin-slice S3e, plan 38 sections 1 and 2f; owner Q238 = 1).
//
// The server-set native marker. An immutable value the database loop (sb-db-loop) publishes after the read-only
// verification and the transaction-store Restart, and the SERVER hands to OpenServedEngine. A handle that carries
// it is a handle on a verified, served NATIVE database; a handle that does not (every sb_engine_open caller) refuses
// a native file (DATABASE.OPEN_NATIVE_RUNTIME_PENDING, owner Q285).
//
// Like the Q113 policy pack root it is process-local state: never serialized, never taken from a client, a parser,
// an SBLR operand or a request option. Every statement context a handle issues carries the HANDLE's pointer
// (AcquireStatementContextReceipt overwrites whatever the caller put there). Copies of the context share it
// (shared_ptr to const).
//
// What it is NOT: authority. It records what the file said at open time. The TIP horizons are the values the
// Restart stored/read back at open; once transactions begin (S4) they are history, not the live horizons.
// Nothing here grants access; authorization still receives user + role + SBLR/UUID/data and nothing else.

#include "../../storage/disk/filespace_page_zero.hpp"

#include <cstdint>
#include <vector>

namespace scratchbird::engine::internal_api {

// The serving state of the DATABASE after the open (mirror of the server's NativeServingState; the engine
// does not include server headers).
enum class NativeServedServingState : std::uint8_t { serving = 0, restricted = 1, failed = 2 };

struct NativeServedDatabaseFacts {
  // Identity, from page zero as the loop verified it.
  core::platform::Uuid database_uuid;
  core::platform::Uuid filespace_uuid;
  core::platform::Uuid page_profile_uuid;    // the primary page-size profile
  core::platform::Uuid engine_instance_uuid; // this open's engine instance
  std::uint32_t page_size_bytes = 0;
  std::uint64_t total_pages = 0;
  // Page zero's fixed roots, as verified (kind + page reference per root). The first block and page zero are
  // the only things the open needs; everything else is found through these.
  std::vector<storage::disk::FilespaceRootReference> fixed_roots;
  // The datatype cohort of the database record the open captured (catalog_snapshot_uuid + two generations).
  // Absent means the database record carried none: the statement path refuses (an explicit migration is needed,
  // never a relabel), exactly as the legacy catalog identity read did.
  bool datatype_cohort_present = false;
  core::platform::Uuid datatype_catalog_snapshot_uuid;
  std::uint64_t datatype_catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  // Transaction-inventory facts at open (after Restart): next transaction order and the six stored horizons.
  std::uint64_t tip_next_transaction = 0;
  std::uint64_t tip_horizon_oit = 0, tip_horizon_oat = 0, tip_horizon_ost = 0, tip_horizon_oct = 0,
                tip_horizon_oar = 0, tip_horizon_olr = 0;
  // The identity of the per-build native executor availability table in force (SBLR availability registry
  // entry points answer from this table, never from a side file). A build constant.
  core::platform::Uuid availability_table_uuid;
  NativeServedServingState serving_state = NativeServedServingState::serving;
};

}  // namespace scratchbird::engine::internal_api
