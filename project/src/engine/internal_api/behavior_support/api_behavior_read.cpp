// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "behavior_support/api_behavior_store.hpp"

#include "local_transaction_store.hpp"
#include "uuid.hpp"
#include "api_behavior_record_codec.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <filesystem>
#include <map>

namespace scratchbird::engine::internal_api {
namespace {
std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const auto end = value.find(delimiter, start);
    parts.push_back(value.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

bool HexDecode(const std::string& value, std::string& out) {
  if ((value.size() % 2) != 0) return false;
  std::string decoded;
  decoded.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) return false;
    decoded.push_back(static_cast<char>((hi << 4) | lo));
  }
  out = std::move(decoded);
  return true;
}

bool ParseU64(const std::string& value, std::uint64_t& out) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

std::string ApiBehaviorEventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.api_events.v2";
}

std::string LowerAscii(std::string value) {
  for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return value;
}

bool MgaCreatorVisible(
    const scratchbird::transaction::mga::LocalTransactionInventory& inventory,
    std::uint64_t creator_tx,
    const EngineRequestContext& context) {
  if (creator_tx == 0) { return true; }
  std::uint64_t transaction_snapshot_boundary =
      context.snapshot_visible_through_local_transaction_id;
  bool observer_identity_mismatch = false;
  for (const auto& observer : inventory.entries) {
    if (!observer.identity.local_id.valid() ||
        observer.identity.local_id.value != context.local_transaction_id) {
      continue;
    }
    if (!context.transaction_uuid.is_nil() &&
        (!observer.identity.transaction_uuid.valid() ||
         observer.identity.transaction_uuid.value !=
             context.transaction_uuid)) {
      observer_identity_mismatch = true;
      break;
    }
    transaction_snapshot_boundary =
        observer.begin_visible_through_local_transaction_id;
    break;
  }
  if (observer_identity_mismatch) return false;
  for (const auto& entry : inventory.entries) {
    if (!entry.identity.local_id.valid() || entry.identity.local_id.value != creator_tx) { continue; }
    using scratchbird::transaction::mga::TransactionState;
    if (creator_tx == context.local_transaction_id) {
      if (!context.transaction_uuid.is_nil() &&
          (!entry.identity.transaction_uuid.valid() ||
           entry.identity.transaction_uuid.value !=
               context.transaction_uuid)) {
        return false;
      }
      return entry.state == TransactionState::active ||
             entry.state == TransactionState::read_only_active ||
             entry.state == TransactionState::preparing ||
             entry.state == TransactionState::prepared;
    }
    if (!scratchbird::transaction::mga::HasCommittedInventoryOutcome(entry)) {
      return false;
    }

    if (context.statement_metadata_snapshot_engine_owned) {
      const auto excluded = [creator_tx](const auto& values) {
        return std::find(values.begin(), values.end(), creator_tx) !=
               values.end();
      };
      if (excluded(
              context
                  .statement_metadata_snapshot_active_excluded_local_transaction_ids) ||
          excluded(
              context
                  .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids)) {
        return false;
      }
      return context
                     .statement_metadata_snapshot_visible_through_local_transaction_id !=
                 0 &&
             creator_tx <=
                 context
                     .statement_metadata_snapshot_visible_through_local_transaction_id;
    }

    const std::string isolation = LowerAscii(
        context.transaction_isolation_level.empty()
            ? std::string("read_committed")
            : context.transaction_isolation_level);
    if ((isolation == "snapshot" || isolation == "repeatable_read" ||
         isolation == "serializable")) {
      return creator_tx <= transaction_snapshot_boundary;
    }
    return true;
  }
  return false;
}

}  // namespace

EngineApiDiagnostic ValidateApiBehaviorContext(const EngineRequestContext& context,
                                               const std::string& operation_id,
                                               bool require_transaction,
                                               bool require_database_path) {
  if (require_database_path && context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(operation_id, "database_path_required");
  }
  if (require_transaction && context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

ApiBehaviorStoreResult LoadApiBehaviorState(const EngineRequestContext& context) {
  ApiBehaviorStoreResult result;
  const auto path_status = ValidateApiBehaviorContext(context, "api_behavior.load_state", false, true);
  if (path_status.error) { result.diagnostic = path_status; return result; }
  const auto transaction_inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!transaction_inventory.ok()) {
    result.diagnostic = MakeEngineApiDiagnosticFromNative(
        transaction_inventory.diagnostic, "CATALOG.INVALID_INPUT",
        "catalog.behavior.transaction_inventory_unavailable", {});
    return result;
  }
  const auto path = ApiBehaviorEventPath(context);
  std::error_code ec;
  const auto link_status = std::filesystem::symlink_status(path, ec);
  if (link_status.type() == std::filesystem::file_type::not_found &&
      (!ec || ec == std::errc::no_such_file_or_directory)) {
    const bool legacy=std::filesystem::exists(context.database_path+".sb.api_events",ec);
    if(legacy||ec){result.diagnostic=MakeInvalidRequestDiagnostic("api_behavior.load_state","legacy_format_unsupported");return result;}
    result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    result.ok = true;
    return result;
  }
  const auto fail = [&](const char* detail) {
    ApiBehaviorStoreResult failed;
    failed.diagnostic = MakeInvalidRequestDiagnostic("api_behavior.load_state", detail);
    return failed;
  };
  if (ec) return fail("api_journal_status_failed");
  if (std::filesystem::is_symlink(link_status)) return fail("api_journal_symlink_forbidden");
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::is_regular_file(status)) return fail("api_journal_not_regular");
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return fail("api_journal_open_failed");
  std::map<EngineUuid, ApiBehaviorRecord> latest;
  std::uint64_t sequence=0;
  while(in.peek()!=std::char_traits<char>::eof()){
    ApiBehaviorRecord record;
    if(!ReadApiBehaviorRecord(in,&record))return fail("api_journal_binary_record_invalid");
    record.event_sequence=++sequence;
    if (!MgaCreatorVisible(transaction_inventory.inventory,
                           record.creator_tx,
                           context)) {
      continue;
    }
    latest[record.object_uuid] = std::move(record);
  }
  if (in.bad()) return fail("api_journal_read_failed");
  for (const auto& [uuid, record] : latest) {
    if (!record.deleted) { result.state.records.push_back(record); }
  }
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.ok = true;
  return result;
}

std::vector<ApiBehaviorRecord> VisibleApiBehaviorRecords(const EngineRequestContext& context,
                                                         const std::string& object_kind,
                                                         std::uint64_t observer_tx,
                                                         EngineApiDiagnostic& diagnostic) {
  EngineRequestContext effective = context;
  if (effective.local_transaction_id == 0) { effective.local_transaction_id = observer_tx; }
  const auto loaded = LoadApiBehaviorState(effective);
  std::vector<ApiBehaviorRecord> records;
  diagnostic = loaded.diagnostic;
  if (!loaded.ok) { return records; }
  for (const auto& record : loaded.state.records) {
    if (object_kind.empty() || record.object_kind == object_kind) { records.push_back(record); }
  }
  return records;
}

std::optional<ApiBehaviorRecord> FindVisibleApiBehaviorRecord(const EngineRequestContext& context,
                                                              const EngineUuid& object_uuid,
                                                              std::uint64_t observer_tx,
                                                              EngineApiDiagnostic& diagnostic) {
  for (const auto& record : VisibleApiBehaviorRecords(context, {}, observer_tx, diagnostic)) {
    if (record.object_uuid == object_uuid) { return record; }
  }
  return std::nullopt;
}

}  // namespace scratchbird::engine::internal_api
