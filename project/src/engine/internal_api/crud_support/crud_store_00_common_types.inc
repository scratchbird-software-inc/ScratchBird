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
#include "catalog/binary_catalog_metadata.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include <iterator>
#include <charconv>
#include "dml/dml_target_access_plan.hpp"

#include "local_transaction_store.hpp"
#include "metric_producer.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kCrudMagic = "SBCRUD2";
constexpr char kCompositeSeparator = '\x1f';
constexpr const char* kCrudLargeValueLocatorPrefix = "SBCRLV02";
constexpr std::size_t kCrudLargeValueChunkBytes = 2048;

// Compatibility event records are subordinate projections. The durable MGA
// inventory overlay below owns transaction finality, including replay.
bool ValidateCrudBinaryEvent(const std::vector<std::string>& fields) {
  if (fields.size()<3 || fields[0]!=kCrudMagic) return false;
  const auto& kind=fields[1];
  const std::map<std::string,std::size_t> widths={
    {"TX_BEGIN",4},{"TX_PREPARE",3},{"TX_COMMIT",3},{"TX_ROLLBACK",3},
    {"SAVEPOINT",5},{"RELEASE_SAVEPOINT",4},{"ROLLBACK_TO_SAVEPOINT",4},
    {"TABLE_CREATE",6},{"TABLE_CREATE_V2",10},{"ROW_VERSION",8},{"ROW_VERSION_V2",10},
    {"INDEX_CREATE",8},{"INDEX_CREATE_V2",15},{"INDEX_ENTRY",9},{"INDEX_ENTRY_V2",12},
    {"OVERFLOW_VALUE",11},{"OVERFLOW_CHUNK",7}};
  const auto width=widths.find(kind);
  if(width==widths.end() || fields.size()!=width->second) return false;
  const auto number=[](const std::string& value) {
    std::uint64_t n=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
    return parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && std::to_string(n)==value;
  };
  if(!number(fields[2])) return false;
  const auto uuid=[&](std::size_t index,bool optional=false) {
    EngineUuid value; return ReadMetadataUuid(fields[index],&value,optional);
  };
  if(kind=="TX_BEGIN") return uuid(3);
  if(kind=="TABLE_CREATE") return uuid(3);
  if(kind=="TABLE_CREATE_V2") return uuid(3) && uuid(8,true);
  if(kind=="ROW_VERSION") return uuid(3)&&uuid(4)&&uuid(5);
  if(kind=="ROW_VERSION_V2") return uuid(3)&&uuid(4)&&uuid(5)&&uuid(7,true)&&number(fields[8]);
  if(kind=="INDEX_CREATE"||kind=="INDEX_CREATE_V2") return uuid(3)&&uuid(4);
  if(kind=="INDEX_ENTRY") return uuid(3)&&uuid(4)&&uuid(7)&&uuid(8);
  if(kind=="INDEX_ENTRY_V2") return uuid(3)&&uuid(4)&&uuid(10)&&uuid(11);
  if(kind=="OVERFLOW_VALUE") return uuid(3)&&uuid(4)&&uuid(5)&&uuid(6)&&number(fields[8]);
  if(kind=="OVERFLOW_CHUNK") return uuid(3)&&number(fields[4])&&number(fields[6]);
  if(kind=="SAVEPOINT") return number(fields[4]);
  return true;
}
EngineUuid CrudBinaryIdentity(std::string_view bytes, bool optional=false) {
  EngineUuid id;
  if (!ReadMetadataUuid(bytes,&id,optional)) throw std::invalid_argument("crud_binary_identity_required");
  return id;
}
bool ReadCrudBinaryEvents(const std::string& path, std::vector<std::vector<std::string>>* output) {
  std::error_code error;
  const bool exists=std::filesystem::exists(path,error);
  if(error || !output) return false;
  if(!exists) { output->clear(); return true; }
  std::ifstream in(path,std::ios::binary);
  if(!in) return false;
  const std::string bytes((std::istreambuf_iterator<char>(in)),{});
  if(in.bad()) return false;
  std::vector<std::string> records;
  if(!DecodeMgaMetadataStream({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},&records)) return false;
  std::vector<std::vector<std::string>> staged;
  for(const auto& record:records) {
    std::vector<std::string> fields;
    if(!DecodeMgaMetadataFields(record,&fields)||!ValidateCrudBinaryEvent(fields)) return false;
    staged.push_back(std::move(fields));
  }
  output->swap(staged); return true;
}

// SEARCH_KEY: SB_MGA_CRUD_SUBORDINATE_TXN_AUTHORITY
using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::IsTerminalTransactionState;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

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

bool TxVisible(const RelationReadSnapshot& state, std::uint64_t creator_tx, std::uint64_t observer_tx) {
  return CrudCreatorVisible(state, creator_tx, 0, observer_tx);
}

std::uint64_t MaxCommittedCrudTransactionId(const RelationReadSnapshot& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed") { max_committed = std::max(max_committed, tx); }
  }
  return max_committed;
}

std::uint64_t VisibilityHighWaterForContext(const RelationReadSnapshot& state, const EngineRequestContext& context) {
  const std::string isolation = context.transaction_isolation_level.empty()
                                    ? std::string("read_committed")
                                    : LowerAscii(context.transaction_isolation_level);
  if ((isolation == "snapshot" || isolation == "repeatable_read" || isolation == "serializable") &&
      context.snapshot_visible_through_local_transaction_id != 0) {
    return context.snapshot_visible_through_local_transaction_id;
  }
  return MaxCommittedCrudTransactionId(state);
}

bool CrudRowVersionVisibleToContextInternal(const RelationReadSnapshot& state,
                                            const CrudRowVersionRecord& row,
                                            const EngineRequestContext& context) {
  if (!row.temporary_session_uuid.is_nil() &&
      row.temporary_session_uuid != context.session_uuid) {
    return false;
  }
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
  if (it->second != "committed") { return false; }
  const std::uint64_t visible_through = VisibilityHighWaterForContext(state, context);
  return visible_through == 0 || row.creator_tx <= visible_through;
}

EngineUuid SessionUuidOrEmpty(const EngineRequestContext& context) {
  return context.session_uuid;
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

  state->transactions.clear();
  state->max_transaction_id = loaded.inventory.next_local_transaction_id - 1;
  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    state->transactions[entry.identity.local_id.value] = CrudTransactionStateName(
        entry.state == TransactionState::archived ? entry.archived_from_state : entry.state);
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
