// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by crud_store.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
#include "crud_support/crud_store.hpp"

#include "local_transaction_store.hpp"
#include "metric_producer.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kCrudMagic = "SBCRUD1";
constexpr char kCompositeSeparator = '\x1f';
constexpr const char* kCrudLargeValueLocatorPrefix = "@SB_OVERFLOW_V1:";
constexpr std::size_t kCrudLargeValueChunkBytes = 2048;

// SEARCH_KEY: SB_MGA_CRUD_SUBORDINATE_TXN_AUTHORITY
using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::IsTerminalTransactionState;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  std::string out;
  if ((value.size() % 2) != 0) { return out; }
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string CrudEventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.crud_events";
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

bool TxVisible(const CrudState& state, std::uint64_t creator_tx, std::uint64_t observer_tx) {
  return CrudCreatorVisible(state, creator_tx, 0, observer_tx);
}

std::uint64_t MaxCommittedCrudTransactionId(const CrudState& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed" || status == "archived") { max_committed = std::max(max_committed, tx); }
  }
  return max_committed;
}

std::uint64_t VisibilityHighWaterForContext(const CrudState& state, const EngineRequestContext& context) {
  const std::string isolation = context.transaction_isolation_level.empty()
                                    ? std::string("read_committed")
                                    : LowerAscii(context.transaction_isolation_level);
  if ((isolation == "snapshot" || isolation == "repeatable_read" || isolation == "serializable") &&
      context.snapshot_visible_through_local_transaction_id != 0) {
    return context.snapshot_visible_through_local_transaction_id;
  }
  return MaxCommittedCrudTransactionId(state);
}

bool CrudRowVersionVisibleToContextInternal(const CrudState& state,
                                            const CrudRowVersionRecord& row,
                                            const EngineRequestContext& context) {
  const std::uint64_t observer_tx = context.local_transaction_id;
  if (row.creator_tx == observer_tx) {
    const auto own = state.transactions.find(row.creator_tx);
    if (own != state.transactions.end() &&
        (own->second == "active" || own->second == "preparing" || own->second == "prepared")) {
      return true;
    }
  }
  const auto it = state.transactions.find(row.creator_tx);
  if (it == state.transactions.end()) { return false; }
  if (it->second != "committed" && it->second != "archived") { return false; }
  const std::uint64_t visible_through = VisibilityHighWaterForContext(state, context);
  return visible_through == 0 || row.creator_tx <= visible_through;
}

std::string SessionUuidOrEmpty(const EngineRequestContext& context) {
  return context.session_uuid.canonical;
}

EngineApiDiagnostic DiagnosticFromTransactionAuthority(const scratchbird::core::platform::DiagnosticRecord& diagnostic,
                                                       const std::string& fallback_code,
                                                       const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key : diagnostic.message_key,
                                 detail,
                                 true);
}

std::string CrudTransactionStateName(TransactionState state) {
  switch (state) {
    case TransactionState::active: return "active";
    case TransactionState::read_only_active: return "read_only_active";
    case TransactionState::preparing: return "preparing";
    case TransactionState::prepared: return "prepared";
    case TransactionState::committing: return "committing";
    case TransactionState::committed: return "committed";
    case TransactionState::rolling_back: return "rolling_back";
    case TransactionState::rolled_back: return "rolled_back";
    case TransactionState::limbo: return "limbo";
    case TransactionState::recovering: return "recovering";
    case TransactionState::failed_terminal: return "failed_terminal";
    case TransactionState::archived: return "archived";
    case TransactionState::none:
    case TransactionState::created:
    default: return "none";
  }
}

EngineApiDiagnostic OverlayCrudTransactionAuthority(const EngineRequestContext& context, CrudState* state) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("crud.transaction_authority", "database_path_required");
  }

  const auto loaded = LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) {
    return DiagnosticFromTransactionAuthority(loaded.diagnostic,
                                              "SB-MGA-TXN-INV-LOAD-FAILED",
                                              "mga.transaction_inventory.load_failed");
  }

  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    state->transactions[entry.identity.local_id.value] = CrudTransactionStateName(entry.state);
    state->max_transaction_id = std::max(state->max_transaction_id, entry.identity.local_id.value);
  }

  if (context.local_transaction_id != 0) {
    const auto lookup = LookupLocalTransaction(loaded.inventory, MakeLocalTransactionId(context.local_transaction_id));
    if (!lookup.ok()) {
      return DiagnosticFromTransactionAuthority(lookup.diagnostic,
                                                "SB-MGA-TXN-INV-LOOKUP-FAILED",
                                                "mga.transaction_inventory.lookup_failed");
    }
    if (lookup.entry.state != TransactionState::active) {
      return MakeInvalidRequestDiagnostic("crud.transaction_authority", "active_local_transaction_required");
    }
  }

  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ValidateCrudMutatingTransactionAuthority(const EngineRequestContext& context) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic("crud.transaction_authority", "local_transaction_id_required");
  }

  const auto loaded = LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) {
    return DiagnosticFromTransactionAuthority(loaded.diagnostic,
                                              "SB-MGA-TXN-INV-LOAD-FAILED",
                                              "mga.transaction_inventory.load_failed");
  }

  const auto lookup = LookupLocalTransaction(loaded.inventory, MakeLocalTransactionId(context.local_transaction_id));
  if (!lookup.ok()) {
    return DiagnosticFromTransactionAuthority(lookup.diagnostic,
                                              "SB-MGA-TXN-INV-LOOKUP-FAILED",
                                              "mga.transaction_inventory.lookup_failed");
  }

  if (lookup.entry.state != TransactionState::active) {
