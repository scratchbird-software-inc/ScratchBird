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
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// The serving state of the DATABASE after the open (mirror of the server's NativeServingState; the engine
// does not include server headers).
enum class NativeServedServingState : std::uint8_t { serving = 0, restricted = 1, failed = 2 };

// SEARCH_KEY: SB_NATIVE_NAMES_SOURCE (thin-slice S4b rework "Part A", owner 2026-11-20). The one native source of
// NAME ROWS for the existing name registry (LoadNameRegistryState): the committed role-2 name entries of the single
// database file, as plain values. The engine never reads a side file for a native database; this is the read it uses
// instead. Values only: raw name bytes AS STORED (the engine stores names as given), binary identities, the u16 record
// kind, flags and counters; no text identifier of any kind except the stored name itself.
struct NativeNameRowValue {
  core::platform::Uuid name_entry_uuid, object_uuid, scope_uuid, language_uuid, dialect_profile_uuid, identifier_profile_uuid;
  core::platform::Uuid parent_object_uuid, parent_schema_uuid;  // meaningful only when the matching has_ flag is set
  bool has_parent_object = false, has_parent_schema = false;
  std::uint16_t object_kind = 0;      // CatalogRecordKind, the u16 kind (no fixed kind UUID exists yet)
  std::uint8_t name_class = 1;        // CatalogNameClass: 1 primary, 2 alias, 3 compatibility, 4 generated, 5 hidden
  std::uint8_t quote_style = 0;       // CatalogNameQuoteStyle
  bool was_quoted = false, requires_exact_match = false, default_for_language = false, default_for_object = false;
  std::string raw_name_text;          // the one stored copy of the name, bytes as given
  std::uint64_t catalog_generation_id = 0, resource_epoch = 0, name_resolution_epoch = 0;
};
// A role-5 identifier profile row, reduced to what the registry projection needs (Part B removes this need).
struct NativeNameProfileValue {
  core::platform::Uuid profile_uuid;
  std::uint64_t unquoted_fold_mode = 0;  // IdentifierFoldMode: 0 none, 1 upper_ascii, 2 lower_ascii, 3 resource_upper, 4 resource_lower
};
enum class NativeNamesSnapshotError : std::uint8_t {
  none = 0, invalid_request, database_not_serving, database_failed, wrong_owner, busy, closed, preparation_failure,
  custody_failure, crypto_failure, memory_failure, configuration_source_failure, names_source_failure
};
inline const char* NativeNamesSnapshotErrorName(NativeNamesSnapshotError e) noexcept {
  switch (e) {
    case NativeNamesSnapshotError::none: return "none";
    case NativeNamesSnapshotError::invalid_request: return "invalid_request";
    case NativeNamesSnapshotError::database_not_serving: return "database_not_serving";
    case NativeNamesSnapshotError::database_failed: return "database_failed";
    case NativeNamesSnapshotError::wrong_owner: return "wrong_owner";
    case NativeNamesSnapshotError::busy: return "busy";
    case NativeNamesSnapshotError::closed: return "closed";
    case NativeNamesSnapshotError::preparation_failure: return "preparation_failure";
    case NativeNamesSnapshotError::custody_failure: return "custody_failure";
    case NativeNamesSnapshotError::crypto_failure: return "crypto_failure";
    case NativeNamesSnapshotError::memory_failure: return "memory_failure";
    case NativeNamesSnapshotError::configuration_source_failure: return "configuration_source_failure";
    case NativeNamesSnapshotError::names_source_failure: return "names_source_failure";
  }
  return "";
}
// One specific code per cause (owner Q193); the codes are INTERNAL, the wire layer decides what a client sees.
inline const char* NativeNamesSnapshotErrorCode(NativeNamesSnapshotError e) noexcept {
  switch (e) {
    case NativeNamesSnapshotError::none: return "";
    case NativeNamesSnapshotError::invalid_request: return "CATALOG.NAMES_SNAPSHOT_NATIVE_REQUEST_INVALID";
    case NativeNamesSnapshotError::database_not_serving: return "CATALOG.NAMES_SNAPSHOT_NATIVE_DATABASE_NOT_SERVING";
    case NativeNamesSnapshotError::database_failed: return "CATALOG.NAMES_SNAPSHOT_NATIVE_DATABASE_FAILED";
    case NativeNamesSnapshotError::wrong_owner: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_WRONG_OWNER";
    case NativeNamesSnapshotError::busy: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_BUSY";
    case NativeNamesSnapshotError::closed: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_CLOSED";
    case NativeNamesSnapshotError::preparation_failure: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_PREPARATION_FAILED";
    case NativeNamesSnapshotError::custody_failure: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_CUSTODY_FAILED";
    case NativeNamesSnapshotError::crypto_failure: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_CRYPTO_SCOPE_FAILED";
    case NativeNamesSnapshotError::memory_failure: return "CATALOG.NAMES_SNAPSHOT_NATIVE_READ_MEMORY_FAILED";
    case NativeNamesSnapshotError::configuration_source_failure: return "CATALOG.NAMES_SNAPSHOT_NATIVE_CONFIGURATION_SOURCE_FAILED";
    case NativeNamesSnapshotError::names_source_failure: return "CATALOG.NAMES_SNAPSHOT_NATIVE_NAMES_SOURCE_FAILED";
  }
  return "";
}
struct NativeNamesSnapshot {
  NativeNamesSnapshotError error = NativeNamesSnapshotError::invalid_request;
  // Structured detail of an infrastructure refusal (enum numbers, never text).
  std::uint32_t source_error = 0, relation_error = 0, custody_error = 0;
  core::platform::Uuid database_uuid;
  std::vector<NativeNameRowValue> rows;          // live, active, committed name entries (role 2)
  std::vector<NativeNameProfileValue> profiles;  // live identifier profiles (role 5)
  // The database's authorization policy bodies' user schema (the principals' name scope); the login (S4c) reads it
  // here instead of a separate resolver. Distinct values, normally exactly one.
  std::vector<core::platform::Uuid> principal_schemas;
  bool ok() const noexcept { return error == NativeNamesSnapshotError::none; }
};
// SEARCH_KEY: SB_NAME_SNAPSHOT_OWN_TXN (owner rule 2026-11-20): everything that reads or writes the database runs
// inside a full MGA transaction, so a session's own uncommitted names are visible to it. SEAM: until S4e's session /
// transaction exists this source reads the COMMITTED snapshot, with no transaction, exactly as S4a/S4b did; the
// transaction parameter of this call arrives with S4e. Do not invent a transaction here.
//
// Implemented by the server's database loop. Read() may be called from any thread: from the loop thread it reads
// directly (S4c's login runs as loop work); from any other thread it is a request on the loop's queue and waits.
class NativeNamesSource {
 public:
  virtual ~NativeNamesSource() = default;
  virtual NativeNamesSnapshot Read() const noexcept = 0;
};

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
  // The code of the loop's diagnostic behind a restricted / failed state (e.g. DATABASE.RESTART_WRITE_FAILED,
  // DATABASE.OPEN_RESTRICTED_UNRESOLVED_TRANSACTIONS); empty when serving. A machine token, not prose.
  std::string serving_diagnostic_code;
  // SB_NATIVE_NAMES_SOURCE: the committed name rows of this database's single file (see NativeNamesSource). Set by
  // the loop with the marker; null only in a marker built without a loop.
  std::shared_ptr<const NativeNamesSource> names_source;
};

}  // namespace scratchbird::engine::internal_api
