// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "canonical_aggregate_registry.hpp"
#include "datatype_catalog_manifest.hpp"
#include "executor_foundation.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "database_format.hpp"
#include "hash_digest.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "sblr_dispatch.hpp"
#include "server_engine_bridge/diagnostic_fields.hpp"
#include "server_engine_bridge/prepared_metadata_binding.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

constexpr std::uint64_t kEngineMagic = 0x5342454e47494e45ull;
constexpr std::uint64_t kSessionMagic = 0x534245534553534eull;
constexpr std::uint64_t kTransactionMagic = 0x53425452414e5343ull;
constexpr std::uint64_t kResultMagic = 0x534245524553554cull;
constexpr std::uint64_t kPreparedMetadataBindingMagic =
    0x5342504d45544131ull;
constexpr std::uint64_t kStatementContextReceiptMagic =
    0x5342535443545831ull;

constexpr const char* kBuildId = "scratchbird-engine-abi-v1";

struct DiagnosticFieldStorage {
  std::string key;
  std::string value;
};

struct DiagnosticStorage {
  sb_engine_diagnostic_view_t view{};
  std::string code;
  std::string message;
  std::string detail;
  std::vector<DiagnosticFieldStorage> fields;
};

struct sb_engine_result_s {
  std::uint64_t magic = kResultMagic;
  mutable std::mutex mutex;
  bool released = false;
  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  std::string operation_id;
  std::vector<DiagnosticStorage> diagnostics;
  std::vector<sb_engine_diagnostic_view_t> diagnostic_views;
  std::string payload;
  std::string result_kind;
  std::vector<std::string> row_values;
  std::vector<std::string> row_metadata_values;
  std::vector<std::string> evidence_values;
  std::uint64_t next_row_index = 0;
  std::uint64_t affected_rows = 0;
  std::uint64_t rows_produced = 0;
};

struct sb_engine_handle_s {
  std::uint64_t magic = kEngineMagic;
  mutable std::mutex mutex;
  bool closed = false;
  std::string database_path;
  std::string database_uuid;
  std::uint64_t database_page_size_bytes = 0;
  std::atomic<std::uint64_t> next_session_id{1};
};

struct sb_engine_session_s {
  std::uint64_t magic = kSessionMagic;
  mutable std::mutex mutex;
  sb_engine_handle_t engine = nullptr;
  bool closed = false;
  std::uint64_t session_id = 0;
  sb_engine_uuid_t effective_user_uuid{};
  sb_engine_uuid_t public_session_uuid{};
  sb_engine_trust_mode_t trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  std::uint32_t active_transactions = 0;
  std::uint32_t open_streams = 0;
};

struct sb_engine_transaction_s {
  std::uint64_t magic = kTransactionMagic;
  mutable std::mutex mutex;
  sb_engine_session_t session = nullptr;
  bool closed = false;
};

namespace scratchbird::server_engine_bridge {

struct PreparedMetadataBindingOpaque {
  std::uint64_t magic = kPreparedMetadataBindingMagic;
  mutable std::mutex mutex;
  bool released = false;
  bool invalidated = false;
  std::string invalidation_detail;
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  std::string database_path;
  std::string database_uuid;
  sb_engine_uuid_t effective_user_uuid{};
  sb_engine_uuid_t session_uuid{};
  sb_engine_uuid_t parser_package_uuid{};
  sb_engine_uuid_t dialect_profile_uuid{};
  sb_engine_trust_mode_t trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  std::uint32_t context_flags = 0;
  std::uint64_t rights_set_ref = 0;
  std::uint64_t capability_set_ref = 0;
  std::uint64_t source_artifact_set_ref = 0;
  std::vector<std::uint8_t> encoded_sblr_envelope;
  std::string metadata_snapshot_uuid;
  std::uint64_t metadata_visible_through_local_transaction_id = 0;
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  std::string target_object_uuid;
  std::uint64_t target_executable_generation = 0;
  std::uint64_t target_metadata_epoch = 0;
  std::uint64_t target_creator_local_transaction_id = 0;
};

struct StatementContextReceiptOpaque {
  std::uint64_t magic = kStatementContextReceiptMagic;
  mutable std::mutex mutex;
  bool released = false;
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  StatementContextReceiptView view;
  scratchbird::transaction::mga::SnapshotVectorDescriptor snapshot_vector;
  scratchbird::engine::internal_api::EngineRequestContext engine_context;
};

}  // namespace scratchbird::server_engine_bridge

namespace {

using scratchbird::server_engine_bridge::PreparedMetadataBindingHandle;
using scratchbird::server_engine_bridge::PreparedMetadataBindingDispatchTestHook;
using scratchbird::server_engine_bridge::StatementContextReceiptHandle;

std::mutex g_prepared_metadata_binding_registry_mutex;
std::unordered_set<PreparedMetadataBindingHandle>
    g_prepared_metadata_bindings;
std::atomic<std::uint64_t> g_prepared_metadata_snapshot_ordinal{1};
std::mutex g_prepared_metadata_dispatch_test_hook_mutex;
PreparedMetadataBindingDispatchTestHook
    g_prepared_metadata_dispatch_test_hook = nullptr;
void* g_prepared_metadata_dispatch_test_hook_context = nullptr;

std::mutex g_statement_context_receipt_registry_mutex;
std::map<std::uint64_t, std::unique_ptr<
    scratchbird::server_engine_bridge::StatementContextReceiptOpaque>>
    g_live_statement_context_receipts;
std::atomic<std::uint64_t> g_next_statement_context_receipt_id{1};
std::atomic<std::uint64_t> g_statement_context_identity_ordinal{1};

void invoke_prepared_metadata_dispatch_test_hook(std::string_view phase) {
  PreparedMetadataBindingDispatchTestHook hook = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> guard(
        g_prepared_metadata_dispatch_test_hook_mutex);
    hook = g_prepared_metadata_dispatch_test_hook;
    context = g_prepared_metadata_dispatch_test_hook_context;
  }
  if (hook != nullptr) { hook(phase, context); }
}

bool valid_abi(std::uint32_t abi_version) {
  return abi_version == SB_ENGINE_ABI_VERSION_PACKED;
}

bool valid_string_span(const char* data, std::uint64_t size) {
  return size == 0 || data != nullptr;
}

bool nonzero_uuid(const sb_engine_uuid_t& uuid) {
  return std::any_of(std::begin(uuid.bytes), std::end(uuid.bytes), [](std::uint8_t v) { return v != 0; });
}

std::string current_utc_timestamp_text() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char text[sizeof("YYYY-MM-DDTHH:MM:SSZ")] = {};
  if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
    return {};
  }
  return text;
}

bool canonical_statement_timestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

std::string current_monotonic_ns_text() {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}

bool statement_context_transaction_active(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

bool generate_distinct_statement_context_uuid(
    std::unordered_set<std::string>* identities,
    std::string* generated_uuid) {
  if (identities == nullptr || generated_uuid == nullptr) return false;
  constexpr std::uint64_t kMaximumAttempts = 32;
  const auto now_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (std::uint64_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    const auto ordinal = g_statement_context_identity_ordinal.fetch_add(
        1, std::memory_order_relaxed);
    const auto generated =
        scratchbird::core::uuid::GenerateDurableEngineIdentityV7(
            scratchbird::core::platform::UuidKind::object,
            now_millis + ordinal + attempt);
    if (!generated.ok()) return false;
    std::string candidate =
        scratchbird::core::uuid::UuidToString(generated.value.value);
    if (identities->insert(candidate).second) {
      *generated_uuid = std::move(candidate);
      return true;
    }
  }
  return false;
}

bool canonical_non_nil_uuid_text(std::string_view value) {
  if (value.empty() ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(value));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == value;
}

bool valid_engine(sb_engine_handle_t handle) {
  return handle != nullptr && handle->magic == kEngineMagic && !handle->closed;
}

bool valid_session(sb_engine_session_t handle) {
  return handle != nullptr && handle->magic == kSessionMagic && !handle->closed && valid_engine(handle->engine);
}

bool valid_transaction(sb_engine_transaction_t handle) {
  return handle != nullptr && handle->magic == kTransactionMagic && !handle->closed && valid_session(handle->session);
}

bool valid_result(sb_engine_result_t handle) {
  return handle != nullptr && handle->magic == kResultMagic && !handle->released;
}

using EngineAbiSteadyClock = std::chrono::steady_clock;

std::uint64_t EngineAbiElapsedMicros(EngineAbiSteadyClock::time_point start,
                                     EngineAbiSteadyClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WriteEngineAbiPhaseTrace(
    std::string_view layer,
    std::string_view operation_id,
    std::size_t envelope_size,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_ENGINE_ABI_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "layer=" << layer
      << "\toperation=" << operation_id
      << "\tenvelope_bytes=" << envelope_size;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
}

std::string uuid_to_canonical(const sb_engine_uuid_t& uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < sizeof(uuid.bytes); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[(uuid.bytes[i] >> 4u) & 0x0fu]);
    out.push_back(kHex[uuid.bytes[i] & 0x0fu]);
  }
  return out;
}

bool same_uuid(const sb_engine_uuid_t& left, const sb_engine_uuid_t& right) {
  return std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

bool active_metadata_snapshot_exclusion(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::created ||
         state == TransactionState::active ||
         state == TransactionState::read_only_active ||
         state == TransactionState::preparing ||
         state == TransactionState::rolling_back;
}

bool in_doubt_metadata_snapshot_exclusion(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering ||
         state == TransactionState::failed_terminal;
}

std::string new_prepared_metadata_snapshot_uuid() {
  const auto now_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto ordinal =
      g_prepared_metadata_snapshot_ordinal.fetch_add(1,
                                                     std::memory_order_relaxed);
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(
          scratchbird::core::platform::UuidKind::object,
          now_millis + ordinal);
  return generated.ok()
             ? scratchbird::core::uuid::UuidToString(generated.value.value)
             : std::string{};
}

std::string operation_operand_value(
    const scratchbird::engine::sblr::SblrOperationEnvelope& envelope,
    std::string_view name) {
  for (const auto& operand : envelope.operands) {
    if (operand.name == name) return operand.value;
  }
  return {};
}

bool has_engine_only_prepared_metadata_operand(
    const scratchbird::engine::sblr::SblrOperationEnvelope& envelope) {
  for (const auto& operand : envelope.operands) {
    if (operand.name.starts_with("engine.prepared_metadata.")) return true;
  }
  return false;
}

struct PreparedMetadataBindingSnapshot {
  std::string metadata_snapshot_uuid;
  std::uint64_t metadata_visible_through_local_transaction_id = 0;
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  std::string target_object_uuid;
  std::uint64_t target_executable_generation = 0;
  std::uint64_t target_metadata_epoch = 0;
};

bool copy_prepared_metadata_binding_for_dispatch(
    PreparedMetadataBindingHandle binding,
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t& context,
    const sb_engine_sblr_dispatch_params_v1_t& params,
    PreparedMetadataBindingSnapshot* snapshot,
    std::string* detail) {
  if (snapshot == nullptr || detail == nullptr || binding == nullptr) {
    if (detail != nullptr) *detail = "binding_or_output_missing";
    return false;
  }
  std::lock_guard<std::mutex> registry_guard(
      g_prepared_metadata_binding_registry_mutex);
  if (g_prepared_metadata_bindings.count(binding) == 0) {
    *detail = "binding_not_live";
    return false;
  }
  std::lock_guard<std::mutex> binding_guard(binding->mutex);
  if (binding->released || binding->magic != kPreparedMetadataBindingMagic) {
    *detail = "binding_released";
    return false;
  }
  if (binding->invalidated) {
    *detail = "binding_invalidated:" + binding->invalidation_detail;
    return false;
  }
  if (binding->session != session || binding->engine != session->engine) {
    *detail = "binding_session_or_engine_mismatch";
    return false;
  }
  if (binding->database_path != session->engine->database_path ||
      binding->database_uuid != session->engine->database_uuid) {
    *detail = "binding_database_mismatch";
    return false;
  }
  if (!same_uuid(binding->effective_user_uuid, context.effective_user_uuid) ||
      !same_uuid(binding->session_uuid, context.session_uuid) ||
      !same_uuid(binding->parser_package_uuid, context.parser_package_uuid) ||
      !same_uuid(binding->dialect_profile_uuid, context.dialect_profile_uuid) ||
      binding->trust_mode != context.trust_mode ||
      binding->context_flags != context.flags ||
      binding->rights_set_ref != context.rights_set_ref ||
      binding->capability_set_ref != context.capability_set_ref ||
      binding->source_artifact_set_ref != context.source_artifact_set_ref) {
    *detail = "binding_security_context_mismatch";
    return false;
  }
  if (params.envelope_size_bytes != binding->encoded_sblr_envelope.size() ||
      params.envelope_bytes == nullptr ||
      !std::equal(binding->encoded_sblr_envelope.begin(),
                  binding->encoded_sblr_envelope.end(),
                  params.envelope_bytes)) {
    *detail = "binding_sblr_envelope_mismatch";
    return false;
  }
  snapshot->metadata_snapshot_uuid = binding->metadata_snapshot_uuid;
  snapshot->metadata_visible_through_local_transaction_id =
      binding->metadata_visible_through_local_transaction_id;
  snapshot->active_excluded_local_transaction_ids =
      binding->active_excluded_local_transaction_ids;
  snapshot->in_doubt_excluded_local_transaction_ids =
      binding->in_doubt_excluded_local_transaction_ids;
  snapshot->target_object_uuid = binding->target_object_uuid;
  snapshot->target_executable_generation =
      binding->target_executable_generation;
  snapshot->target_metadata_epoch = binding->target_metadata_epoch;
  return true;
}

void release_prepared_metadata_bindings_for_session(
    sb_engine_session_t session) {
  std::vector<PreparedMetadataBindingHandle> released;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_prepared_metadata_binding_registry_mutex);
    for (auto it = g_prepared_metadata_bindings.begin();
         it != g_prepared_metadata_bindings.end();) {
      auto* binding = *it;
      if (binding->session != session) {
        ++it;
        continue;
      }
      {
        std::lock_guard<std::mutex> binding_guard(binding->mutex);
        binding->released = true;
        binding->magic = 0;
      }
      released.push_back(binding);
      it = g_prepared_metadata_bindings.erase(it);
    }
  }
  for (auto* binding : released) delete binding;
}

void release_statement_context_receipts_for_session(
    sb_engine_session_t session) {
  std::vector<scratchbird::core::platform::TypedUuid> published_snapshots;
  std::vector<std::unique_ptr<
      scratchbird::server_engine_bridge::StatementContextReceiptOpaque>>
      released;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    for (auto it = g_live_statement_context_receipts.begin();
         it != g_live_statement_context_receipts.end();) {
      auto& receipt = it->second;
      if (receipt->session != session) {
        ++it;
        continue;
      }
      {
        std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
        receipt->released = true;
        receipt->magic = 0;
        published_snapshots.push_back(
            receipt->snapshot_vector.snapshot_uuid);
      }
      released.push_back(std::move(receipt));
      it = g_live_statement_context_receipts.erase(it);
    }
  }
  for (const auto& snapshot_uuid : published_snapshots) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot_uuid);
  }
}

bool looks_like_sblr_operation_envelope(const scratchbird::engine::SblrExecutionEnvelope& envelope) {
  if (envelope.payload_kind != scratchbird::engine::SblrPayloadKind::operation_envelope ||
      envelope.canonical_bytes.empty()) {
    return false;
  }
  const auto* data = reinterpret_cast<const char*>(envelope.canonical_bytes.data());
  const std::string_view text(data, envelope.canonical_bytes.size());
  return text.find("operation_id=") != std::string_view::npos &&
         text.find("opcode=") != std::string_view::npos;
}

struct DatabaseHeaderSnapshot {
  std::string database_uuid;
  std::uint64_t page_size_bytes = 0;
};

DatabaseHeaderSnapshot database_header_snapshot(std::string_view database_path) {
  DatabaseHeaderSnapshot snapshot;
  if (database_path.empty()) return snapshot;
  scratchbird::storage::disk::SerializedDatabaseHeader serialized{};
  std::ifstream in(std::string(database_path), std::ios::binary);
  if (!in) return snapshot;
  in.read(reinterpret_cast<char*>(serialized.data()),
          static_cast<std::streamsize>(serialized.size()));
  if (in.gcount() != static_cast<std::streamsize>(serialized.size())) return snapshot;
  const auto parsed = scratchbird::storage::disk::ParseDatabaseHeader(serialized);
  if (!parsed.ok()) return snapshot;
  snapshot.database_uuid =
      scratchbird::core::uuid::UuidToString(parsed.header.database_uuid);
  snapshot.page_size_bytes = parsed.header.page_size;
  return snapshot;
}

scratchbird::engine::internal_api::EngineRequestContext make_internal_context(
    sb_engine_handle_t engine,
    const sb_engine_request_context_v1_t& context) {
  scratchbird::engine::internal_api::EngineRequestContext internal;
  internal.trust_mode = context.trust_mode == SB_ENGINE_TRUST_EMBEDDED_TRUSTED
                            ? scratchbird::engine::internal_api::EngineTrustMode::embedded_in_process
                            : scratchbird::engine::internal_api::EngineTrustMode::server_isolated;
  internal.request_id = "public-abi-sblr-dispatch";
  internal.database_path = engine == nullptr ? std::string{} : engine->database_path;
  internal.database_uuid.canonical = engine == nullptr ? std::string{} : engine->database_uuid;
  internal.database_page_size_bytes =
      engine == nullptr ? 0 : engine->database_page_size_bytes;
  internal.principal_uuid.canonical = uuid_to_canonical(context.effective_user_uuid);
  internal.session_uuid.canonical = uuid_to_canonical(context.session_uuid);
  internal.transaction_uuid.canonical = {};
  internal.statement_uuid.canonical = {};
  internal.local_transaction_id = context.transaction_ref;
  internal.snapshot_visible_through_local_transaction_id =
      context.transaction_ref;
  internal.statement_timestamp = current_utc_timestamp_text();
  internal.current_timestamp = internal.statement_timestamp;
  internal.current_monotonic_ns = current_monotonic_ns_text();
  if (context.transaction_ref != 0) {
    internal.transaction_timestamp = internal.statement_timestamp;
    const auto loaded =
        scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
            internal.database_path);
    if (loaded.ok()) {
      const auto lookup = scratchbird::transaction::mga::LookupLocalTransaction(
          loaded.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(context.transaction_ref));
      if (lookup.ok() && lookup.entry.identity.transaction_uuid.valid()) {
        internal.transaction_uuid.canonical =
            scratchbird::core::uuid::UuidToString(
                lookup.entry.identity.transaction_uuid.value);
        internal.snapshot_visible_through_local_transaction_id =
            lookup.entry.begin_visible_through_local_transaction_id;
      }
    }
  }
  internal.security_context_present = context.rights_set_ref != 0;
  internal.cluster_authority_available = false;
  internal.catalog_generation_id = 1;
  internal.security_epoch = 1;
  internal.resource_epoch = 1;
  internal.name_resolution_epoch = 1;
  internal.trace_tags.push_back("public_abi");
  if (context.rights_set_ref != 0) {
    internal.trace_tags.push_back("group:OPS");
    auto& authorization = internal.authorization_context;
    authorization.present = true;
    authorization.authority_uuid.canonical =
        "public-abi-rights-set:" + std::to_string(context.rights_set_ref);
    authorization.principal_uuid = internal.principal_uuid;
    authorization.security_epoch = internal.security_epoch;
    authorization.policy_epoch = 1;
    authorization.catalog_generation_id = internal.catalog_generation_id;
    authorization.effective_subjects.push_back(
        {internal.principal_uuid, "principal"});
    for (const char* right : {"OBS_MANAGEMENT_INSPECT",
                              "OBS_MANAGEMENT_CONTROL"}) {
      scratchbird::engine::internal_api::EngineMaterializedAuthorizationGrant grant;
      grant.grant_uuid.canonical = authorization.authority_uuid.canonical +
                                   ":" + right;
      grant.subject_uuid = internal.principal_uuid;
      grant.subject_kind = "principal";
      grant.right = right;
      grant.security_epoch = authorization.security_epoch;
      authorization.grants.push_back(std::move(grant));
    }
    authorization.evidence_tags.push_back("public_abi_rights_set_ref");
  }
  return internal;
}

enum class PreparedMetadataCurrentVersionStatus {
  ok,
  binding_invalid,
  stale,
  unavailable,
};

void invalidate_prepared_metadata_binding_if_snapshot_matches(
    PreparedMetadataBindingHandle binding,
    std::string_view expected_metadata_snapshot_uuid,
    std::string detail) {
  std::lock_guard<std::mutex> registry_guard(
      g_prepared_metadata_binding_registry_mutex);
  const auto found = g_prepared_metadata_bindings.find(binding);
  if (found == g_prepared_metadata_bindings.end()) { return; }
  auto* live_binding = *found;
  std::lock_guard<std::mutex> binding_guard(live_binding->mutex);
  if (live_binding->released ||
      live_binding->magic != kPreparedMetadataBindingMagic ||
      live_binding->metadata_snapshot_uuid !=
          expected_metadata_snapshot_uuid) {
    return;
  }
  live_binding->invalidated = true;
  live_binding->invalidation_detail = std::move(detail);
}

PreparedMetadataCurrentVersionStatus
revalidate_prepared_metadata_binding_current_version(
    PreparedMetadataBindingHandle binding,
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t& context,
    const sb_engine_sblr_dispatch_params_v1_t& params,
    PreparedMetadataBindingSnapshot* pinned,
    std::string* detail) {
  if (pinned == nullptr || detail == nullptr) {
    return PreparedMetadataCurrentVersionStatus::binding_invalid;
  }
  if (!copy_prepared_metadata_binding_for_dispatch(
          binding, session, context, params, pinned, detail)) {
    return detail->starts_with("binding_invalidated:")
               ? PreparedMetadataCurrentVersionStatus::stale
               : PreparedMetadataCurrentVersionStatus::binding_invalid;
  }

  const auto inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          session->engine->database_path);
  if (!inventory.ok()) {
    *detail = "current_transaction_inventory_unavailable:" +
              inventory.diagnostic.diagnostic_code;
    return PreparedMetadataCurrentVersionStatus::unavailable;
  }
  const std::uint64_t high_water =
      inventory.inventory.next_local_transaction_id == 0
          ? 0
          : inventory.inventory.next_local_transaction_id - 1;
  std::vector<std::uint64_t> active_excluded;
  std::vector<std::uint64_t> in_doubt_excluded;
  for (const auto& entry : inventory.inventory.entries) {
    if (!entry.identity.local_id.valid() ||
        entry.identity.local_id.value > high_water) {
      continue;
    }
    if (active_metadata_snapshot_exclusion(entry.state)) {
      active_excluded.push_back(entry.identity.local_id.value);
    } else if (in_doubt_metadata_snapshot_exclusion(entry.state)) {
      in_doubt_excluded.push_back(entry.identity.local_id.value);
    }
  }
  std::sort(active_excluded.begin(), active_excluded.end());
  std::sort(in_doubt_excluded.begin(), in_doubt_excluded.end());

  const std::string current_snapshot_uuid =
      new_prepared_metadata_snapshot_uuid();
  if (current_snapshot_uuid.empty()) {
    *detail = "current_metadata_snapshot_uuid_unavailable";
    return PreparedMetadataCurrentVersionStatus::unavailable;
  }
  auto current_context = make_internal_context(session->engine, context);
  current_context.statement_metadata_snapshot_engine_owned = true;
  current_context.statement_metadata_snapshot_uuid.canonical =
      current_snapshot_uuid;
  current_context
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      high_water;
  current_context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids =
      std::move(active_excluded);
  current_context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      std::move(in_doubt_excluded);
  const auto lifecycle =
      scratchbird::engine::internal_api::LoadExecutableObjectLifecycleState(
          current_context);
  if (!lifecycle.ok) {
    *detail = "current_metadata_lifecycle_unavailable:" +
              lifecycle.diagnostic.code + ":" +
              lifecycle.diagnostic.detail;
    return PreparedMetadataCurrentVersionStatus::unavailable;
  }

  const scratchbird::engine::internal_api::EngineExecutableObjectRecord*
      current_object = nullptr;
  for (const auto& object : lifecycle.state.objects) {
    if (object.object_uuid == pinned->target_object_uuid) {
      current_object = &object;
      break;
    }
  }
  const bool exact_live_version =
      current_object != nullptr &&
      current_object->object_kind == "procedure" &&
      current_object->lifecycle_state == "active" &&
      !current_object->deleted && !current_object->invalidated &&
      current_object->executable_generation ==
          pinned->target_executable_generation &&
      current_object->metadata_epoch == pinned->target_metadata_epoch;
  if (!exact_live_version) {
    *detail = current_object == nullptr
                  ? "target_not_live:" + pinned->target_object_uuid
                  : "pinned:" + pinned->target_object_uuid + ":" +
                        std::to_string(pinned->target_executable_generation) +
                        ":" + std::to_string(pinned->target_metadata_epoch) +
                        ":current:" + current_object->object_uuid + ":" +
                        std::to_string(current_object->executable_generation) +
                        ":" + std::to_string(current_object->metadata_epoch) +
                        ":" + current_object->lifecycle_state;
    // Release may retire and delete the opaque handle while current metadata
    // is being loaded. Match the immutable snapshot UUID under the registry
    // and binding locks so a recycled address can never invalidate a newer
    // binding (ABA-safe stale publication).
    invalidate_prepared_metadata_binding_if_snapshot_matches(
        binding, pinned->metadata_snapshot_uuid, *detail);
    return PreparedMetadataCurrentVersionStatus::stale;
  }
  return PreparedMetadataCurrentVersionStatus::ok;
}

std::string api_row_value(const scratchbird::engine::internal_api::EngineApiResult& api_result,
                          std::size_t row_index) {
  std::ostringstream out;
  bool first = true;
  for (const auto& field : api_result.result_shape.rows[row_index].fields) {
    if (!first) {
      out << ";";
    }
    first = false;
    out << field.first << "=" << field.second.encoded_value;
  }
  return out.str();
}

std::vector<std::string> api_row_values(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  std::vector<std::string> rows;
  rows.reserve(api_result.result_shape.rows.size());
  for (std::size_t row_index = 0; row_index < api_result.result_shape.rows.size(); ++row_index) {
    rows.push_back(api_row_value(api_result, row_index));
  }
  return rows;
}

std::string api_row_metadata_value(const scratchbird::engine::internal_api::EngineApiResult& api_result,
                                   std::size_t row_index) {
  std::ostringstream out;
  bool first = true;
  const auto& row = api_result.result_shape.rows[row_index];
  for (std::size_t field_index = 0; field_index < row.fields.size(); ++field_index) {
    if (!first) {
      out << ";";
    }
    first = false;
    const auto& field = row.fields[field_index];
    std::string type_name = field.second.descriptor.canonical_type_name;
    if (type_name.empty() && field_index < api_result.result_shape.columns.size()) {
      type_name = api_result.result_shape.columns[field_index].canonical_type_name;
    }
    if (type_name.empty()) {
      type_name = "unknown";
    }
    out << field.first << ":" << type_name << ":" << (field.second.is_null ? "null" : "not_null");
  }
  return out.str();
}

std::vector<std::string> api_row_metadata_values(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  std::vector<std::string> rows;
  rows.reserve(api_result.result_shape.rows.size());
  for (std::size_t row_index = 0; row_index < api_result.result_shape.rows.size(); ++row_index) {
    rows.push_back(api_row_metadata_value(api_result, row_index));
  }
  return rows;
}

std::vector<std::string> api_evidence_values(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  std::vector<std::string> evidence_values;
  evidence_values.reserve(api_result.evidence.size());
  for (const auto& evidence : api_result.evidence) {
    std::ostringstream out;
    out << evidence.evidence_kind << ":" << evidence.evidence_id;
    evidence_values.push_back(out.str());
  }
  return evidence_values;
}

bool has_text_line_option(std::string_view encoded,
                          std::string_view key,
                          std::string_view expected_value) {
  std::string operand_line;
  operand_line.reserve(key.size() + expected_value.size() + 15);
  operand_line.append("operand=text\t");
  operand_line.append(key);
  operand_line.push_back('\t');
  operand_line.append(expected_value);
  if (encoded.size() >= operand_line.size() &&
      encoded.substr(0, operand_line.size()) == operand_line &&
      (encoded.size() == operand_line.size() || encoded[operand_line.size()] == '\n')) {
    return true;
  }
  operand_line.insert(operand_line.begin(), '\n');
  operand_line.push_back('\n');
  if (encoded.find(operand_line) != std::string_view::npos) {
    return true;
  }

  std::string line;
  line.reserve(key.size() + expected_value.size() + 3);
  line.append(key);
  line.push_back('=');
  line.append(expected_value);
  if (encoded.size() >= line.size() &&
      encoded.substr(0, line.size()) == line &&
      (encoded.size() == line.size() || encoded[line.size()] == '\n')) {
    return true;
  }
  line.insert(line.begin(), '\n');
  line.push_back('\n');
  return encoded.find(line) != std::string_view::npos;
}

bool text_line_field_equals(std::string_view encoded,
                            std::string_view key,
                            std::string_view expected_value) {
  std::string line;
  line.reserve(key.size() + expected_value.size() + 2);
  line.append(key);
  line.push_back('=');
  line.append(expected_value);
  if (encoded.size() >= line.size() &&
      encoded.substr(0, line.size()) == line &&
      (encoded.size() == line.size() || encoded[line.size()] == '\n')) {
    return true;
  }
  line.insert(line.begin(), '\n');
  line.push_back('\n');
  return encoded.find(line) != std::string_view::npos;
}

std::uint16_t read_native_u16(const std::uint8_t* data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         (static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint32_t read_native_u32(const std::uint8_t* data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16u) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24u);
}

std::uint64_t read_native_u64(const std::uint8_t* data, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8u);
  }
  return value;
}

struct NativeRowPacketDecode {
  bool ok = false;
  scratchbird::engine::internal_api::EngineApiRequest request;
  std::string detail;
};

enum class NativeRowPacketColumnType : std::uint8_t {
  kText = 1,
  kInt64 = 2,
  kBoolean = 3,
  kInt32 = 4,
  kUInt64 = 5,
  kReal64 = 6,
  kBinary = 7,
};

scratchbird::engine::internal_api::EngineDescriptor native_row_descriptor(
    const char* canonical_type_name) {
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type_name;
  descriptor.encoded_descriptor = std::string("type=") + canonical_type_name;
  return descriptor;
}

std::int64_t read_native_i64(const std::uint8_t* data, std::size_t offset) {
  const std::uint64_t bits = read_native_u64(data, offset);
  std::int64_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::int32_t read_native_i32(const std::uint8_t* data, std::size_t offset) {
  const std::uint32_t bits = read_native_u32(data, offset);
  std::int32_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double read_native_real64(const std::uint8_t* data, std::size_t offset) {
  const std::uint64_t bits = read_native_u64(data, offset);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::string native_i64_to_string(std::int64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, ptr);
}

std::string native_u64_to_string(std::uint64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, ptr);
}

std::string native_real64_to_string(double value) {
  char buffer[64] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, ptr);
}

bool native_row_packet_column_type_supported(NativeRowPacketColumnType type) {
  switch (type) {
    case NativeRowPacketColumnType::kText:
    case NativeRowPacketColumnType::kInt64:
    case NativeRowPacketColumnType::kBoolean:
    case NativeRowPacketColumnType::kInt32:
    case NativeRowPacketColumnType::kUInt64:
    case NativeRowPacketColumnType::kReal64:
    case NativeRowPacketColumnType::kBinary:
      return true;
  }
  return false;
}

NativeRowPacketDecode decode_native_row_packet_v1(const std::uint8_t* data,
                                                  std::size_t packet_size) {
  static const scratchbird::engine::internal_api::EngineDescriptor
      kTextDescriptor = native_row_descriptor("text");
  static const scratchbird::engine::internal_api::EngineDescriptor
      kNullDescriptor = native_row_descriptor("null");
  NativeRowPacketDecode decoded;
  const std::uint64_t row_count = read_native_u64(data, 8);
  const std::uint32_t column_count = read_native_u32(data, 16);
  if (row_count == 0 || column_count == 0 || column_count > 4096) {
    decoded.detail = "native_row_packet_shape_invalid";
    return decoded;
  }
  if (row_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / column_count)) {
    decoded.detail = "native_row_packet_cell_count_overflow";
    return decoded;
  }
  std::size_t offset = 20;
  std::vector<std::string> columns;
  columns.reserve(column_count);
  for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
    if (offset + 4 > packet_size) {
      decoded.detail = "native_row_packet_column_truncated";
      return decoded;
    }
    const std::uint32_t name_size = read_native_u32(data, offset);
    offset += 4;
    if (name_size == 0 || offset + name_size > packet_size) {
      decoded.detail = "native_row_packet_column_name_invalid";
      return decoded;
    }
    columns.emplace_back(reinterpret_cast<const char*>(data + offset), name_size);
    offset += name_size;
  }
  decoded.request.rows.reserve(static_cast<std::size_t>(row_count));
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    scratchbird::engine::internal_api::EngineRowValue row;
    row.fields.reserve(column_count);
    for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
      if (offset + 5 > packet_size) {
        decoded.detail = "native_row_packet_cell_truncated";
        return decoded;
      }
      const bool is_null = data[offset++] != 0;
      const std::uint32_t value_size = read_native_u32(data, offset);
      offset += 4;
      if (offset + value_size > packet_size) {
        decoded.detail = "native_row_packet_value_truncated";
        return decoded;
      }
      scratchbird::engine::internal_api::EngineTypedValue value;
      value.descriptor = is_null ? kNullDescriptor : kTextDescriptor;
      if (is_null) {
        value.is_null = true;
        value.setState(scratchbird::engine::internal_api::EngineValueState::sql_null);
      } else {
        value.encoded_value.assign(reinterpret_cast<const char*>(data + offset),
                                   value_size);
      }
      offset += value_size;
      row.fields.push_back({columns[column_index], std::move(value)});
    }
    decoded.request.rows.push_back(std::move(row));
  }
  if (offset != packet_size) {
    decoded.detail = "native_row_packet_trailing_bytes";
    return decoded;
  }
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_materialized=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_format:scratchbird.native_rows.v1");
  decoded.request.option_envelopes.push_back("sblr.rowset_default_markers_absent=true");
  decoded.request.option_envelopes.push_back(
      "sblr.native_row_packet_row_count:" + std::to_string(row_count));
  decoded.ok = true;
  return decoded;
}

NativeRowPacketDecode decode_native_row_packet_v2(const std::uint8_t* data,
                                                  std::size_t packet_size) {
  NativeRowPacketDecode decoded;
  const std::uint64_t row_count = read_native_u64(data, 8);
  const std::uint32_t column_count = read_native_u32(data, 16);
  if (row_count == 0 || column_count == 0 || column_count > 4096) {
    decoded.detail = "native_row_packet_shape_invalid";
    return decoded;
  }
  if (row_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / column_count)) {
    decoded.detail = "native_row_packet_cell_count_overflow";
    return decoded;
  }
  const std::size_t null_bitmap_bytes = (static_cast<std::size_t>(column_count) + 7u) / 8u;
  std::size_t offset = 20;
  if (offset + column_count > packet_size) {
    decoded.detail = "native_row_packet_type_vector_truncated";
    return decoded;
  }
  std::vector<NativeRowPacketColumnType> column_types;
  column_types.reserve(column_count);
  for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
    const auto type = static_cast<NativeRowPacketColumnType>(data[offset++]);
    if (!native_row_packet_column_type_supported(type)) {
      decoded.detail = "native_row_packet_type_unsupported";
      return decoded;
    }
    column_types.push_back(type);
    decoded.request.native_row_packet.column_type_tags.push_back(
        static_cast<std::uint8_t>(type));
  }
  std::vector<std::string> columns;
  columns.reserve(column_count);
  for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
    if (offset + 4 > packet_size) {
      decoded.detail = "native_row_packet_column_truncated";
      return decoded;
    }
    const std::uint32_t name_size = read_native_u32(data, offset);
    offset += 4;
    if (name_size == 0 || offset + name_size > packet_size) {
      decoded.detail = "native_row_packet_column_name_invalid";
      return decoded;
    }
    columns.emplace_back(reinterpret_cast<const char*>(data + offset), name_size);
    offset += name_size;
  }
  decoded.request.native_row_packet.field_order = columns;
  decoded.request.shared_row_field_order = std::move(columns);
  decoded.request.native_row_packet.row_offsets.reserve(
      static_cast<std::size_t>(row_count));
  decoded.request.native_row_packet.row_sizes.reserve(
      static_cast<std::size_t>(row_count));
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    if (offset + null_bitmap_bytes > packet_size) {
      decoded.detail = "native_row_packet_null_bitmap_truncated";
      return decoded;
    }
    const std::size_t null_bitmap_offset = offset;
    const std::size_t row_start_offset = offset;
    offset += null_bitmap_bytes;
    for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
      const bool is_null =
          (data[null_bitmap_offset + column_index / 8u] &
           static_cast<std::uint8_t>(1u << (column_index % 8u))) != 0;
      if (is_null) {
        continue;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kBoolean) {
        if (offset + 1 > packet_size) {
          decoded.detail = "native_row_packet_boolean_truncated";
          return decoded;
        }
        offset += 1;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kInt32) {
        if (offset + 4 > packet_size) {
          decoded.detail = "native_row_packet_int32_truncated";
          return decoded;
        }
        offset += 4;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kInt64) {
        if (offset + 8 > packet_size) {
          decoded.detail = "native_row_packet_int64_truncated";
          return decoded;
        }
        offset += 8;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kUInt64) {
        if (offset + 8 > packet_size) {
          decoded.detail = "native_row_packet_uint64_truncated";
          return decoded;
        }
        offset += 8;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kReal64) {
        if (offset + 8 > packet_size) {
          decoded.detail = "native_row_packet_real64_truncated";
          return decoded;
        }
        offset += 8;
      } else {
        if (offset + 4 > packet_size) {
          decoded.detail = "native_row_packet_value_length_truncated";
          return decoded;
        }
        const std::uint32_t value_size = read_native_u32(data, offset);
        offset += 4;
        if (offset + value_size > packet_size) {
          decoded.detail = "native_row_packet_value_truncated";
          return decoded;
        }
        offset += value_size;
      }
    }
    if (row_start_offset > std::numeric_limits<std::uint32_t>::max() ||
        offset > std::numeric_limits<std::uint32_t>::max()) {
      decoded.detail = "native_row_packet_row_offset_overflow";
      return decoded;
    }
    decoded.request.native_row_packet.row_offsets.push_back(
        static_cast<std::uint32_t>(row_start_offset));
    decoded.request.native_row_packet.row_sizes.push_back(
        static_cast<std::uint32_t>(offset - row_start_offset));
  }
  if (offset != packet_size) {
    decoded.detail = "native_row_packet_trailing_bytes";
    return decoded;
  }
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_materialized=false");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_frame_only=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_format:scratchbird.native_rows.v2");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_shared_field_order=true");
  decoded.request.option_envelopes.push_back("sblr.rowset_default_markers_absent=true");
  decoded.request.option_envelopes.push_back("sblr.compact_native_rowset_materialized=false");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_type_vector_validated=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_value_body_validated=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_fixed_shape_validated=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_binary_scalar_values=true");
  decoded.request.option_envelopes.push_back(
      "sblr.native_row_packet_row_count:" + std::to_string(row_count));
  decoded.request.option_envelopes.push_back(
      "sblr.native_row_packet_null_bitmap_bytes:" + std::to_string(null_bitmap_bytes));
  decoded.request.native_row_packet.present = true;
  decoded.request.native_row_packet.version = 2;
  decoded.request.native_row_packet.row_count = row_count;
  decoded.request.native_row_packet.column_count = column_count;
  decoded.request.native_row_packet.packet_bytes.assign(data,
                                                        data + packet_size);
  decoded.ok = true;
  return decoded;
}

NativeRowPacketDecode decode_native_row_packet(const std::uint8_t* data,
                                               std::uint64_t size) {
  NativeRowPacketDecode decoded;
  if (size == 0) {
    decoded.ok = true;
    return decoded;
  }
  if (data == nullptr ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    decoded.detail = "native_row_packet_invalid_pointer_or_size";
    return decoded;
  }
  const auto packet_size = static_cast<std::size_t>(size);
  if (packet_size < 20 ||
      data[0] != 'S' || data[1] != 'B' || data[2] != 'N' || data[3] != 'R') {
    decoded.detail = "native_row_packet_bad_header";
    return decoded;
  }
  const std::uint16_t version = read_native_u16(data, 4);
  const std::uint16_t flags = read_native_u16(data, 6);
  if (flags != 0) {
    decoded.detail = "native_row_packet_flags_unsupported";
    return decoded;
  }
  if (version == 1) return decode_native_row_packet_v1(data, packet_size);
  if (version == 2) return decode_native_row_packet_v2(data, packet_size);
  decoded.detail = "native_row_packet_version_unsupported";
  return decoded;
}

std::uint64_t api_evidence_u64(const scratchbird::engine::internal_api::EngineApiResult& api_result,
                               std::string_view evidence_kind,
                               std::uint64_t fallback) {
  for (const auto& evidence : api_result.evidence) {
    if (std::string_view(evidence.evidence_kind) != evidence_kind) {
      continue;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(evidence.evidence_id.c_str(), &end, 10);
    if (end != evidence.evidence_id.c_str() && end != nullptr && *end == '\0') {
      return static_cast<std::uint64_t>(parsed);
    }
  }
  return fallback;
}

void append_transaction_context(std::string* payload,
                                const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  if (payload == nullptr) {
    return;
  }
  if (api_result.local_transaction_id != 0) {
    *payload += "local_transaction_id=" + std::to_string(api_result.local_transaction_id) + "\n";
  }
  if (!api_result.transaction_uuid.canonical.empty()) {
    *payload += "transaction_uuid=" + api_result.transaction_uuid.canonical + "\n";
  }
}

std::string api_result_payload(std::string_view operation_id,
                               std::string_view result_kind,
                               const std::vector<std::string>& rows,
                               const std::vector<std::string>& row_metadata,
                               const std::vector<std::string>& evidence_values,
                               std::uint64_t first_row,
                               std::uint64_t row_count) {
  std::ostringstream out;
  out << "operation_id=" << operation_id << "\n";
  out << "result_kind=" << result_kind << "\n";
  out << "row_count=" << row_count << "\n";
  for (std::uint64_t offset = 0; offset < row_count; ++offset) {
    const std::uint64_t row_index = first_row + offset;
    if (row_index >= rows.size()) {
      break;
    }
    out << "row[" << row_index << "]=" << rows[static_cast<std::size_t>(row_index)] << "\n";
    if (row_index < row_metadata.size()) {
      out << "row_meta[" << row_index << "]=" << row_metadata[static_cast<std::size_t>(row_index)] << "\n";
    }
  }
  for (const auto& evidence : evidence_values) {
    out << "evidence=" << evidence << "\n";
  }
  return out.str();
}

std::string api_result_payload(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  const auto rows = api_row_values(api_result);
  const auto row_metadata = api_row_metadata_values(api_result);
  const auto evidence_values = api_evidence_values(api_result);
  std::string payload = api_result_payload(api_result.operation_id,
                                           api_result.result_shape.result_kind,
                                           rows,
                                           row_metadata,
                                           evidence_values,
                                           0,
                                           static_cast<std::uint64_t>(rows.size()));
  append_transaction_context(&payload, api_result);
  return payload;
}

bool dispatch_has_diagnostic(const scratchbird::engine::sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

std::string first_dispatch_diagnostic_code(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (!diagnostic.code.empty() && diagnostic.code != "SB_ENGINE_API_OK") {
      return diagnostic.code;
    }
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (!diagnostic.code.empty()) {
      return diagnostic.code;
    }
  }
  return {};
}

std::string first_dispatch_diagnostic_detail(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (!diagnostic.detail.empty()) {
      return diagnostic.detail;
    }
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (!diagnostic.message.empty()) {
      return diagnostic.message;
    }
  }
  return result.api_result.operation_id;
}

std::vector<std::pair<std::string, std::string>> first_dispatch_diagnostic_fields(
    const scratchbird::engine::sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code.empty() || diagnostic.code == "SB_ENGINE_API_OK") {
      continue;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(diagnostic.fields.size());
    for (const auto& field : diagnostic.fields) {
      fields.emplace_back(field.key, field.value);
    }
    return fields;
  }
  return {};
}

sb_engine_status_t operation_envelope_failure_status(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  if (dispatch_has_diagnostic(
          result,
          scratchbird::engine::internal_api::
              kExecutableObjectDiagnosticPreparedMetadataVersionMismatch)) {
    return SB_ENGINE_STATUS_CONFLICT;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_CLUSTER_AUTHORITY_UNAVAILABLE") ||
      dispatch_has_diagnostic(result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED")) {
    return SB_ENGINE_STATUS_CAPABILITY_DISABLED;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED")) {
    return SB_ENGINE_STATUS_SECURITY_DENIED;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED")) {
    return SB_ENGINE_STATUS_TRANSACTION_REQUIRED;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_UNKNOWN_OPERATION")) {
    return SB_ENGINE_STATUS_UNSUPPORTED;
  }
  return SB_ENGINE_STATUS_INVALID_ARGUMENT;
}

std::string operation_envelope_failure_code(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_ENVELOPE_REJECTED") ||
      dispatch_has_diagnostic(result, "SB_SBLR_SQL_TEXT_FORBIDDEN")) {
    return "SBLR.ENVELOPE.INVALID";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_CLUSTER_AUTHORITY_UNAVAILABLE")) {
    return "SBLR.CAPABILITY.FORBIDDEN";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED")) {
    return "SECURITY.IDENTITY.MISSING";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED")) {
    return "ENGINE.TRANSACTION.REQUIRED";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_UNKNOWN_OPERATION")) {
    return "SBLR.OPCODE.UNKNOWN";
  }
  if (const auto code = first_dispatch_diagnostic_code(result); !code.empty()) {
    return code;
  }
  return "SBLR.ENVELOPE.INVALID";
}

sb_engine_result_t make_result(sb_engine_result_class_t cls, std::string operation_id);
void finalize_diagnostics(sb_engine_result_t result);
sb_engine_status_t fail_result(sb_engine_status_t status,
                               sb_engine_result_t* out_result,
                               std::uint32_t numeric_code,
                               std::string code,
                               std::string message,
                               std::string detail = {},
                               std::vector<std::pair<std::string, std::string>> fields = {});

sb_engine_status_t dispatch_operation_envelope(sb_engine_session_t session,
                                               const sb_engine_request_context_v1_t& context,
                                               const scratchbird::engine::SblrExecutionEnvelope& envelope,
                                               const sb_engine_sblr_dispatch_params_v1_t& params,
                                               const PreparedMetadataBindingSnapshot*
                                                   prepared_metadata,
                                               sb_engine_result_t* out_result) {
  // SEARCH_KEY: SB_ENGINE_PUBLIC_ABI_ADMITTED_SBLR_ONLY
  // This ABI revision carries one byte pointer and therefore cannot carry the
  // canonical outer SBLR container, the separate 28-field SBEE record, and an
  // immutable server admission token. Fail closed until the versioned public
  // ABI surface can receive that token; never reparse the single raw pointer.
  (void)session;
  (void)context;
  (void)envelope;
  (void)params;
  (void)prepared_metadata;
  return fail_result(
      SB_ENGINE_STATUS_UNSUPPORTED,
      out_result,
      4032,
      "SBLR.ENVELOPE.FIELD_MISSING",
      "sblr.envelope.field_missing",
      "immutable_server_admission_token_required");
#if 0
  const auto* data = reinterpret_cast<const char*>(envelope.canonical_bytes.data());
  const std::string_view encoded(data, envelope.canonical_bytes.size());
  auto phase_last = EngineAbiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(8);
  const auto mark_phase = [&](std::string phase) {
    const auto now = EngineAbiSteadyClock::now();
    phase_micros.push_back({std::move(phase), EngineAbiElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto decoded_operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(encoded);
  if (decoded_operation.ok &&
      has_engine_only_prepared_metadata_operand(decoded_operation.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4013,
        "SBLR.PREPARED_METADATA.ENGINE_OPTION_FORBIDDEN",
        "sblr.prepared_metadata.engine_option_forbidden",
        "prepared metadata authority cannot be supplied by SBLR operands");
  }
  auto api_context = make_internal_context(session->engine, context);
  mark_phase("make_internal_context");
  scratchbird::engine::internal_api::EngineApiRequest api_request;
  if (prepared_metadata != nullptr) {
    const auto& metadata = *prepared_metadata;
    if (!decoded_operation.ok ||
        decoded_operation.envelope.operation_id !=
            "routine.procedure_invoke" ||
        operation_operand_value(decoded_operation.envelope,
                                "target_object_uuid") !=
            metadata.target_object_uuid) {
      return fail_result(
          SB_ENGINE_STATUS_SECURITY_DENIED,
          out_result,
          4016,
          "ENGINE.PREPARED_METADATA_BINDING.TARGET_MISMATCH",
          "engine.prepared_metadata_binding.target_mismatch",
          "binding is valid only for its UUID-bound procedure invocation");
    }
    api_context.statement_metadata_snapshot_engine_owned = true;
    api_context.statement_metadata_snapshot_uuid.canonical =
        metadata.metadata_snapshot_uuid;
    api_context
        .statement_metadata_snapshot_visible_through_local_transaction_id =
        metadata.metadata_visible_through_local_transaction_id;
    api_context
        .statement_metadata_snapshot_active_excluded_local_transaction_ids =
        metadata.active_excluded_local_transaction_ids;
    api_context
        .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
        metadata.in_doubt_excluded_local_transaction_ids;
    api_context.prepared_metadata_required_object_uuid.canonical =
        metadata.target_object_uuid;
    api_context.prepared_metadata_required_executable_generation =
        metadata.target_executable_generation;
    api_context.prepared_metadata_required_metadata_epoch =
        metadata.target_metadata_epoch;
    // These options are injected after decoding the untrusted SBLR envelope.
    // The executable-object runtime consumes the typed context fields above;
    // the options are engine-only trace evidence and never authority.
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.binding_consumed:true");
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.required_object_uuid:" +
        metadata.target_object_uuid);
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.required_executable_generation:" +
        std::to_string(metadata.target_executable_generation));
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.required_metadata_epoch:" +
        std::to_string(metadata.target_metadata_epoch));
  }
  if (params.data_packet_size_bytes != 0) {
    if (!text_line_field_equals(encoded, "operation_id", "dml.execute_native_bulk_ingest")) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                         out_result,
                         4011,
                         "SBLR.DATA_PACKET.OPERATION_MISMATCH",
                         "sblr.data_packet.operation_mismatch",
                         "native row packets are only admitted for dml.execute_native_bulk_ingest");
    }
    auto packet = decode_native_row_packet(params.data_packet_bytes,
                                           params.data_packet_size_bytes);
    if (!packet.ok) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                         out_result,
                         4012,
                         "SBLR.DATA_PACKET.INVALID",
                         "sblr.data_packet.invalid",
                         packet.detail);
    }
    api_request = std::move(packet.request);
  }
  auto dispatch_result =
      scratchbird::engine::sblr::DecodeAndDispatchSblrOperation(encoded,
                                                                api_context,
                                                                std::move(api_request));
  mark_phase("decode_and_dispatch_operation");
  if (prepared_metadata != nullptr) {
    dispatch_result.api_result.evidence.push_back(
        {"prepared_metadata_atomicity",
         "routed_owner_inventory_guard_exact_version_lease"});
  }
  if (!dispatch_result.accepted || !dispatch_result.api_result.ok) {
    const sb_engine_status_t status = operation_envelope_failure_status(dispatch_result);
    WriteEngineAbiPhaseTrace("operation_envelope",
                             dispatch_result.api_result.operation_id,
                             envelope.canonical_bytes.size(),
                             phase_micros);
    return fail_result(status,
                       out_result,
                       4010,
                       operation_envelope_failure_code(dispatch_result),
                       "sblr.operation_envelope.rejected",
                       first_dispatch_diagnostic_detail(dispatch_result),
                       first_dispatch_diagnostic_fields(dispatch_result));
  }

  auto* result = make_result(SB_ENGINE_RESULT_ROW_BATCH, dispatch_result.api_result.operation_id);
  mark_phase("make_result");
  const bool summary_only_requested =
      has_text_line_option(encoded, "result_payload_policy", "summary_only");
  const bool summary_only_import =
      dispatch_result.api_result.operation_id == "dml.execute_import_rows" &&
      summary_only_requested;
  const bool summary_only_native_bulk =
      dispatch_result.api_result.operation_id == "dml.execute_native_bulk_ingest" &&
      summary_only_requested;
  const bool summary_only_dml_write =
      summary_only_requested &&
      !summary_only_import &&
      !summary_only_native_bulk &&
      dispatch_result.api_result.operation_id.rfind("dml.", 0) == 0;
  // Command completion is independent of result-row presentation.  The
  // engine-owned DML summary remains the affected-row authority whether the
  // caller asks for full rows, an explicit summary, or accepts the default
  // write-result policy applied during neutral SBLR dispatch.
  result->affected_rows = dispatch_result.api_result.dml_summary.rows_changed;
  result->result_kind = dispatch_result.api_result.result_shape.result_kind;
  if (summary_only_import) {
    result->rows_produced = api_evidence_u64(
        dispatch_result.api_result,
        "import_inserted_rows",
        api_evidence_u64(dispatch_result.api_result,
                         "import_canonical_rows",
                         static_cast<std::uint64_t>(dispatch_result.api_result.result_shape.rows.size())));
    if (result->result_kind.empty()) {
      result->result_kind = "import_rows_summary";
    }
  } else if (summary_only_native_bulk) {
    result->rows_produced = dispatch_result.api_result.dml_summary.rows_changed;
    if (result->rows_produced == 0) {
      result->rows_produced = api_evidence_u64(
          dispatch_result.api_result,
          "direct_physical_bulk_row_count",
          static_cast<std::uint64_t>(dispatch_result.api_result.result_shape.rows.size()));
    }
    if (result->result_kind.empty()) {
      result->result_kind = "native_bulk_ingest_summary";
    }
    result->row_values = {
        "accepted_rows=" + std::to_string(result->rows_produced) +
        ";inserted_rows=" + std::to_string(result->rows_produced) +
        ";rejected_rows=0"};
    result->row_metadata_values = {
        "accepted_rows:uint64:not_null;inserted_rows:uint64:not_null;"
        "rejected_rows:uint64:not_null"};
  } else if (summary_only_dml_write) {
    result->rows_produced = 0;
    if (result->result_kind.empty()) {
      result->result_kind = "dml_write_summary";
    }
  } else {
    result->rows_produced = static_cast<std::uint64_t>(dispatch_result.api_result.result_shape.rows.size());
    result->row_values = api_row_values(dispatch_result.api_result);
    result->row_metadata_values = api_row_metadata_values(dispatch_result.api_result);
  }
  mark_phase("shape_result_rows");
  if (summary_only_native_bulk) {
    result->evidence_values = {
        "direct_physical_bulk_row_count:" + std::to_string(result->rows_produced),
        "result_payload_policy:summary_only"};
  } else {
    result->evidence_values = api_evidence_values(dispatch_result.api_result);
  }
  mark_phase("shape_evidence");
  if (summary_only_import || summary_only_native_bulk || summary_only_dml_write) {
    const std::uint64_t summary_payload_rows =
        summary_only_native_bulk
            ? static_cast<std::uint64_t>(result->row_values.size())
            : 0;
    result->payload = api_result_payload(dispatch_result.api_result.operation_id,
                                         result->result_kind,
                                         result->row_values,
                                         result->row_metadata_values,
                                         result->evidence_values,
                                         0,
                                         summary_payload_rows);
    append_transaction_context(&result->payload, dispatch_result.api_result);
  } else {
    result->payload = api_result_payload(dispatch_result.api_result);
  }
  mark_phase("build_result_payload");
  finalize_diagnostics(result);
  mark_phase("finalize_diagnostics");
  WriteEngineAbiPhaseTrace("operation_envelope",
                           dispatch_result.api_result.operation_id,
                           envelope.canonical_bytes.size(),
                           phase_micros);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
#endif
}

std::string behavior_payload() {
  const auto cluster_provider = scratchbird::engine::cluster_provider::DescribeClusterProvider();
  std::string payload =
      "abi=implemented;sblr_dispatch=admission_only;cluster_provider_name=";
  payload += cluster_provider.provider_name;
  payload += ";cluster_provider_type=";
  payload += cluster_provider.provider_type;
  payload += ";cluster_provider_version=";
  payload += cluster_provider.provider_version;
  payload += ";cluster_provider_support=";
  payload += cluster_provider.support_status;
  payload += ";cluster_provider_execution=";
  payload += cluster_provider.supports_execution ? "true" : "false";
  payload += ";cluster=";
  payload += cluster_provider.supports_execution ? "cluster_provider_enabled" : "noncluster_fail_closed";
  payload += ";cluster_provider_boundary=compile_gated_provider;"
      "llvm=capability_fail_closed;gpu=capability_fail_closed;udr=capability_report_only";
  for (const auto& row : scratchbird::engine::kSblrPriorityDRegistry) {
    payload += ";";
    payload += row.family_name;
    payload += "=";
    payload += scratchbird::engine::SblrBehaviorStatusName(row.behavior_status);
  }
  payload += ";";
  payload += scratchbird::engine::kSblrAccelerationRegistryRow.family_name;
  payload += "=";
  payload += scratchbird::engine::SblrBehaviorStatusName(
      scratchbird::engine::kSblrAccelerationRegistryRow.behavior_status);
  payload += ";";
  payload += scratchbird::engine::kSblrReferenceMetaRegistryRow.family_name;
  payload += "=";
  payload += scratchbird::engine::SblrBehaviorStatusName(scratchbird::engine::kSblrReferenceMetaRegistryRow.behavior_status);
  return payload;
}

void set_view(sb_engine_string_view_t& view, const std::string& text) {
  view.data = text.data();
  view.size_bytes = static_cast<std::uint64_t>(text.size());
}

sb_engine_result_t make_result(sb_engine_result_class_t cls, std::string operation_id = {}) {
  auto* result = new sb_engine_result_s();
  result->result_class = cls;
  result->operation_id = std::move(operation_id);
  return result;
}

void add_diagnostic(sb_engine_result_t result,
                    std::uint32_t numeric_code,
                    sb_engine_diagnostic_severity_t severity,
                    std::string code,
                    std::string message,
                    std::string detail = {},
                    std::vector<std::pair<std::string, std::string>> fields = {}) {
  if (result == nullptr) {
    return;
  }
  DiagnosticStorage storage;
  storage.view.struct_size = sizeof(sb_engine_diagnostic_view_t);
  storage.view.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  storage.view.numeric_code = numeric_code;
  storage.view.severity = severity;
  storage.code = std::move(code);
  storage.message = std::move(message);
  storage.detail = std::move(detail);
  storage.fields.reserve(fields.size());
  for (auto& field : fields) {
    DiagnosticFieldStorage field_storage;
    field_storage.key = std::move(field.first);
    field_storage.value = std::move(field.second);
    storage.fields.push_back(std::move(field_storage));
  }
  result->diagnostics.push_back(std::move(storage));
}

void finalize_diagnostics(sb_engine_result_t result) {
  if (result == nullptr) {
    return;
  }
  result->diagnostic_views.clear();
  result->diagnostic_views.reserve(result->diagnostics.size());
  for (auto& diagnostic : result->diagnostics) {
    set_view(diagnostic.view.symbolic_code, diagnostic.code);
    set_view(diagnostic.view.message_key, diagnostic.message);
    set_view(diagnostic.view.safe_detail, diagnostic.detail);
    diagnostic.view.reserved0 = 0;
    diagnostic.view.reserved1 = 0;
    result->diagnostic_views.push_back(diagnostic.view);
  }
}

sb_engine_status_t fail_result(sb_engine_status_t status,
                               sb_engine_result_t* out_result,
                               std::uint32_t numeric_code,
                               std::string code,
                               std::string message,
                               std::string detail,
                               std::vector<std::pair<std::string, std::string>> fields) {
  if (out_result != nullptr) {
    auto* result = make_result(SB_ENGINE_RESULT_DIAGNOSTIC_ONLY);
    add_diagnostic(result,
                   numeric_code,
                   SB_ENGINE_DIAGNOSTIC_ERROR,
                   std::move(code),
                   std::move(message),
                   std::move(detail),
                   std::move(fields));
    finalize_diagnostics(result);
    *out_result = result;
  }
  return status;
}

sb_engine_status_t check_struct(std::uint32_t struct_size,
                                std::uint32_t abi_version,
                                std::uint32_t minimum_size,
                                sb_engine_result_t* out_result) {
  if (struct_size < minimum_size) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1001, "ENGINE.ABI.STRUCT_SIZE_INVALID",
                       "engine.abi.struct_size_invalid");
  }
  if (!valid_abi(abi_version)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1002, "ENGINE.ABI.VERSION_UNSUPPORTED",
                       "engine.abi.version_unsupported");
  }
  return SB_ENGINE_STATUS_OK;
}

void clear_result(sb_engine_result_t* out_result) {
  if (out_result != nullptr) {
    *out_result = nullptr;
  }
}

}  // namespace

extern "C" {

std::uint32_t sb_engine_abi_version_packed(void) {
  return SB_ENGINE_ABI_VERSION_PACKED;
}

sb_engine_status_t sb_engine_abi_build_id(const char** out_data, std::uint64_t* out_size) {
  if (out_data == nullptr || out_size == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  *out_data = kBuildId;
  *out_size = static_cast<std::uint64_t>(std::strlen(kBuildId));
  return SB_ENGINE_STATUS_OK;
}

const char* sb_engine_status_name(sb_engine_status_t status) {
  switch (status) {
    case SB_ENGINE_STATUS_OK: return "OK";
    case SB_ENGINE_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case SB_ENGINE_STATUS_INVALID_HANDLE: return "INVALID_HANDLE";
    case SB_ENGINE_STATUS_UNSUPPORTED: return "UNSUPPORTED";
    case SB_ENGINE_STATUS_CAPABILITY_DISABLED: return "CAPABILITY_DISABLED";
    case SB_ENGINE_STATUS_SECURITY_DENIED: return "SECURITY_DENIED";
    case SB_ENGINE_STATUS_TRANSACTION_ACTIVE: return "TRANSACTION_ACTIVE";
    case SB_ENGINE_STATUS_TRANSACTION_REQUIRED: return "TRANSACTION_REQUIRED";
    case SB_ENGINE_STATUS_CONFLICT: return "CONFLICT";
    case SB_ENGINE_STATUS_NOT_FOUND: return "NOT_FOUND";
    case SB_ENGINE_STATUS_TIMEOUT: return "TIMEOUT";
    case SB_ENGINE_STATUS_RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case SB_ENGINE_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case SB_ENGINE_STATUS_ALREADY_RELEASED: return "ALREADY_RELEASED";
  }
  return "UNKNOWN";
}

sb_engine_status_t sb_engine_open(const sb_engine_open_params_v1_t* params,
                                  sb_engine_handle_t* out_engine,
                                  sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_engine == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1003, "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_engine = nullptr;
  if (params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_open_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  if (!valid_string_span(params->database_path_utf8, params->database_path_size)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1005, "ENGINE.OPEN.PATH_INVALID",
                       "engine.open.path_invalid");
  }
  if (params->mode < SB_ENGINE_OPEN_NORMAL || params->mode > SB_ENGINE_OPEN_VALIDATION_ONLY) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1006, "ENGINE.OPEN.MODE_INVALID",
                       "engine.open.mode_invalid");
  }
  auto* handle = new sb_engine_handle_s();
  if (params->database_path_utf8 != nullptr && params->database_path_size != 0) {
    handle->database_path.assign(params->database_path_utf8,
                                 params->database_path_utf8 + params->database_path_size);
    const auto snapshot = database_header_snapshot(handle->database_path);
    handle->database_uuid = snapshot.database_uuid;
    handle->database_page_size_bytes = snapshot.page_size_bytes;
  }
  *out_engine = handle;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_close(sb_engine_handle_t engine, sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  {
    std::lock_guard<std::mutex> guard(engine->mutex);
    engine->closed = true;
    engine->magic = 0;
  }
  delete engine;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_session_begin(sb_engine_handle_t engine,
                                           const sb_engine_session_params_v1_t* params,
                                           sb_engine_session_t* out_session,
                                           sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_session == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1003, "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_session = nullptr;
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_session_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  if (!nonzero_uuid(params->effective_user_uuid) || !nonzero_uuid(params->session_uuid)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 2001, "SECURITY.IDENTITY.MISSING",
                       "security.identity.missing");
  }
  if (!valid_string_span(params->default_language_utf8, params->default_language_size)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1008, "ENGINE.SESSION.LANGUAGE_INVALID",
                       "engine.session.language_invalid");
  }
  auto* session = new sb_engine_session_s();
  session->engine = engine;
  session->session_id = engine->next_session_id.fetch_add(1, std::memory_order_relaxed);
  session->effective_user_uuid = params->effective_user_uuid;
  session->public_session_uuid = params->session_uuid;
  session->trust_mode = params->trust_mode;
  *out_session = session;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_session_end(sb_engine_session_t session,
                                         const sb_engine_session_end_params_v1_t* params,
                                         sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params != nullptr) {
    auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_session_end_params_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
  }
  {
    std::lock_guard<std::mutex> guard(session->mutex);
    if (session->active_transactions != 0 && (params == nullptr || params->rollback_active_transactions == 0)) {
      return fail_result(SB_ENGINE_STATUS_TRANSACTION_ACTIVE, out_result, 3001, "ENGINE.SESSION.TRANSACTION_ACTIVE",
                         "engine.session.transaction_active");
    }
    if (session->open_streams != 0 && (params == nullptr || params->cancel_open_results == 0)) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 3002, "ENGINE.RESULT.STREAM_ACTIVE",
                         "engine.result.stream_active");
    }
    release_statement_context_receipts_for_session(session);
    release_prepared_metadata_bindings_for_session(session);
    session->closed = true;
    session->magic = 0;
  }
  delete session;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_transaction_begin(sb_engine_session_t session,
                                               const sb_engine_transaction_params_v1_t* params,
                                               sb_engine_transaction_t* out_transaction,
                                               sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_transaction == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1003, "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_transaction = nullptr;
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_transaction_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  auto* transaction = new sb_engine_transaction_s();
  transaction->session = session;
  {
    std::lock_guard<std::mutex> guard(session->mutex);
    ++session->active_transactions;
  }
  *out_transaction = transaction;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_transaction_commit(sb_engine_transaction_t transaction,
                                                const sb_engine_transaction_finish_params_v1_t* params,
                                                sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!valid_transaction(transaction)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params != nullptr) {
    auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_transaction_finish_params_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
  }
  auto* session = transaction->session;
  {
    std::lock_guard<std::mutex> session_guard(session->mutex);
    if (session->active_transactions > 0) {
      --session->active_transactions;
    }
  }
  transaction->closed = true;
  transaction->magic = 0;
  delete transaction;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_transaction_rollback(sb_engine_transaction_t transaction,
                                                  const sb_engine_transaction_finish_params_v1_t* params,
                                                  sb_engine_result_t* out_result) {
  return sb_engine_transaction_commit(transaction, params, out_result);
}

}  // extern "C"

namespace scratchbird::server_engine_bridge {

sb_engine_status_t AcquireStatementContextReceipt(
    sb_engine_session_t session,
    const StatementContextAcquireRequest* request,
    StatementContextReceiptHandle* out_receipt,
    StatementContextReceiptView* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_receipt == nullptr || out_view == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4033,
        "ENGINE.STATEMENT_CONTEXT.OUTPUT_REQUIRED",
        "engine.statement_context.output_required");
  }
  *out_receipt = {};
  *out_view = {};
  if (!valid_session(session)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_HANDLE,
        out_result,
        4034,
        "ENGINE.STATEMENT_CONTEXT.SESSION_INVALID",
        "engine.statement_context.session_invalid");
  }
  if (request == nullptr || request->engine_context == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4035,
        "ENGINE.STATEMENT_CONTEXT.REQUEST_REQUIRED",
        "engine.statement_context.request_required");
  }

  auto engine_context = *request->engine_context;
  const auto expected_trust_mode =
      session->trust_mode == SB_ENGINE_TRUST_EMBEDDED_TRUSTED
          ? scratchbird::engine::internal_api::EngineTrustMode::embedded_in_process
          : scratchbird::engine::internal_api::EngineTrustMode::server_isolated;
  if (engine_context.database_path.empty() ||
      engine_context.database_path != session->engine->database_path ||
      engine_context.database_uuid.canonical !=
          session->engine->database_uuid ||
      engine_context.principal_uuid.canonical !=
          uuid_to_canonical(session->effective_user_uuid) ||
      engine_context.session_uuid.canonical !=
          uuid_to_canonical(session->public_session_uuid) ||
      engine_context.trust_mode != expected_trust_mode) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4036,
        "ENGINE.STATEMENT_CONTEXT.SESSION_CONTEXT_MISMATCH",
        "engine.statement_context.session_context_mismatch");
  }
  if (!engine_context.statement_uuid.canonical.empty() ||
      !engine_context.statement_snapshot_uuid.canonical.empty() ||
      engine_context.statement_metadata_snapshot_engine_owned ||
      !engine_context.statement_metadata_snapshot_uuid.canonical.empty() ||
      !engine_context.catalog_epoch_uuid.canonical.empty() ||
      !engine_context.optimizer_capability_snapshot_uuid.canonical.empty() ||
      !engine_context.optimizer_resource_snapshot_uuid.canonical.empty() ||
      !engine_context.optimizer_route_snapshot_uuid.canonical.empty()) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4037,
        "ENGINE.STATEMENT_CONTEXT.CALLER_AUTHORITY_FORBIDDEN",
        "engine.statement_context.caller_authority_forbidden");
  }
  if (engine_context.local_transaction_id == 0 ||
      request->exact_transaction_uuid.empty() ||
      engine_context.transaction_uuid.canonical !=
          request->exact_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        4038,
        "ENGINE.STATEMENT_CONTEXT.EXACT_TRANSACTION_REQUIRED",
        "engine.statement_context.exact_transaction_required");
  }
  const auto parsed_transaction =
      scratchbird::core::uuid::ParseTypedUuid(
          scratchbird::core::platform::UuidKind::transaction,
          std::string(request->exact_transaction_uuid));
  if (!parsed_transaction.ok() ||
      scratchbird::core::uuid::UuidToString(
          parsed_transaction.value.value) != request->exact_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4039,
        "ENGINE.STATEMENT_CONTEXT.TRANSACTION_UUID_INVALID",
        "engine.statement_context.transaction_uuid_invalid");
  }
  if (!engine_context.security_context_present ||
      !engine_context.authorization_context.present ||
      !canonical_non_nil_uuid_text(
          engine_context.authorization_context.authority_uuid.canonical) ||
      engine_context.authorization_context.principal_uuid.canonical !=
          engine_context.principal_uuid.canonical ||
      engine_context.catalog_generation_id == 0 ||
      engine_context.security_epoch == 0 ||
      engine_context.resource_epoch == 0 ||
      engine_context.authorization_context.security_epoch !=
          engine_context.security_epoch ||
      engine_context.authorization_context.policy_epoch == 0 ||
      engine_context.authorization_context.catalog_generation_id !=
          engine_context.catalog_generation_id) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete");
  }

  // A fully materialized server policy may narrow these limits. When it does
  // not, issue the canonical bounded defaults here so neither the parser nor
  // SBLR becomes optimizer/resource authority merely to acquire a statement.
  if (engine_context.optimizer_route_epoch == 0) {
    engine_context.optimizer_route_epoch = engine_context.resource_epoch;
  }
  if (engine_context.optimizer_route_generation == 0) {
    engine_context.optimizer_route_generation =
        engine_context.catalog_generation_id;
  }
  if (engine_context.optimizer_memory_budget_bytes == 0) {
    engine_context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  }
  if (engine_context.optimizer_maximum_candidate_count == 0) {
    engine_context.optimizer_maximum_candidate_count = 131072;
  }
  if (engine_context.optimizer_maximum_memo_groups == 0) {
    engine_context.optimizer_maximum_memo_groups = 131072;
  }
  if (engine_context.optimizer_maximum_search_steps == 0) {
    engine_context.optimizer_maximum_search_steps = 1048576;
  }
  if (engine_context.optimizer_maximum_planning_time_ns == 0) {
    engine_context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  }

  const auto inventory_guard =
      scratchbird::engine::internal_api::AcquireTransactionInventoryGuard(
          engine_context.database_path);
  const auto loaded =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          engine_context.database_path);
  if (!loaded.ok()) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4041,
        "ENGINE.STATEMENT_CONTEXT.INVENTORY_UNAVAILABLE",
        "engine.statement_context.inventory_unavailable",
        loaded.diagnostic.diagnostic_code);
  }
  const auto exact_transaction =
      scratchbird::transaction::mga::LookupLocalTransaction(
          loaded.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              engine_context.local_transaction_id));
  if (!exact_transaction.ok() ||
      !statement_context_transaction_active(exact_transaction.entry.state) ||
      !exact_transaction.entry.identity.transaction_uuid.valid() ||
      scratchbird::core::uuid::UuidToString(
          exact_transaction.entry.identity.transaction_uuid.value) !=
          request->exact_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        4042,
        "ENGINE.STATEMENT_CONTEXT.TRANSACTION_NOT_ACTIVE",
        "engine.statement_context.transaction_not_active",
        std::to_string(engine_context.local_transaction_id));
  }

  std::unordered_set<std::string> distinct_identities;
  for (const auto& identity : {
           engine_context.database_uuid.canonical,
           engine_context.principal_uuid.canonical,
           engine_context.session_uuid.canonical,
           engine_context.transaction_uuid.canonical,
           engine_context.authorization_context.authority_uuid.canonical}) {
    if (!identity.empty()) distinct_identities.insert(identity);
  }

  std::string statement_uuid;
  if (!generate_distinct_statement_context_uuid(
          &distinct_identities, &statement_uuid)) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_uuid");
  }
  engine_context.request_id = "private-statement-context-acquire";
  engine_context.statement_uuid.canonical = statement_uuid;
  engine_context.statement_snapshot_uuid.canonical.clear();
  // QOW-SOURCE-RCP-075-ENGINE-STATEMENT-TIMESTAMP-ACQUISITION-V1
  // Receipt acquisition is the sole production clock boundary for the
  // statement timestamp. Caller/server materialization is deliberately
  // overwritten so no earlier layer can become TTL time authority.
  engine_context.statement_timestamp = current_utc_timestamp_text();
  engine_context.current_timestamp = engine_context.statement_timestamp;
  if (!canonical_statement_timestamp(engine_context.statement_timestamp)) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_timestamp_invalid");
  }
  if (engine_context.current_monotonic_ns.empty()) {
    engine_context.current_monotonic_ns = current_monotonic_ns_text();
  }

  scratchbird::engine::internal_api::EnginePublishStatementSnapshotRequest
      publish_request;
  publish_request.context = engine_context;
  const auto published =
      scratchbird::engine::internal_api::EnginePublishStatementSnapshot(
          publish_request);
  if (!published.ok) {
    const auto* diagnostic = published.diagnostics.empty()
                                 ? nullptr
                                 : &published.diagnostics.front();
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4045,
        diagnostic == nullptr
            ? "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_PUBLISH_FAILED"
            : diagnostic->code,
        diagnostic == nullptr
            ? "engine.statement_context.snapshot_publish_failed"
            : diagnostic->message_key,
        diagnostic == nullptr ? std::string{} : diagnostic->detail);
  }
  const auto& snapshot = published.snapshot_vector;
  const std::string statement_snapshot_uuid =
      published.statement_snapshot_uuid.canonical;
  if (!snapshot.complete || !snapshot.inventory_authoritative ||
      snapshot.snapshot_kind != scratchbird::transaction::mga::
                                    SnapshotVectorKind::statement_stable ||
      snapshot.owning_transaction.value !=
          engine_context.local_transaction_id ||
      scratchbird::core::uuid::UuidToString(
          snapshot.owning_transaction_uuid.value) !=
          request->exact_transaction_uuid ||
      published.statement_uuid.canonical != statement_uuid ||
      !distinct_identities.insert(statement_snapshot_uuid).second) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4046,
        "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_AUTHORITY_MISMATCH",
        "engine.statement_context.snapshot_authority_mismatch");
  }

  StatementContextReceiptView view;
  const auto issue_identity = [&](std::string* value) {
    return generate_distinct_statement_context_uuid(
        &distinct_identities, value);
  };
  if (!issue_identity(&view.receipt_uuid) ||
      !issue_identity(&view.statement_metadata_snapshot_uuid) ||
      !issue_identity(&view.catalog_epoch_uuid) ||
      !issue_identity(&view.optimizer_capability_snapshot_uuid) ||
      !issue_identity(&view.optimizer_resource_snapshot_uuid) ||
      !issue_identity(&view.optimizer_route_snapshot_uuid) ||
      !issue_identity(&view.bound_ast_uuid)) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "receipt_context_identity");
  }
  // Canonical aggregate function identity is catalog authority, not a
  // per-statement handle.  Publish the exact engine-owned registry rows so
  // repeated projected/HAVING references bind the same immutable functions.
  const auto aggregate_registry_errors =
      scratchbird::engine::executor::
          ValidateCanonicalAggregateRuntimeRegistryV1();
  const auto& aggregate_registry =
      scratchbird::engine::executor::CanonicalAggregateRuntimeRegistryV1();
  const auto* count_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::count);
  const auto* sum_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::sum);
  const auto* avg_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::avg);
  const auto* min_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::min);
  const auto* max_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::max);
  if (!aggregate_registry_errors.empty() || aggregate_registry.empty() ||
      aggregate_registry.size() > 192 || count_registry_entry == nullptr ||
      sum_registry_entry == nullptr ||
      avg_registry_entry == nullptr || min_registry_entry == nullptr ||
      max_registry_entry == nullptr || !count_registry_entry->executable ||
      !sum_registry_entry->executable || !avg_registry_entry->executable ||
      !min_registry_entry->executable || !max_registry_entry->executable) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.AGGREGATE_REGISTRY_UNAVAILABLE",
        "engine.statement_context.aggregate_registry_unavailable");
  }
  view.count_function_uuid = count_registry_entry->function_uuid;
  view.sum_function_uuid = sum_registry_entry->function_uuid;
  view.avg_function_uuid = avg_registry_entry->function_uuid;
  view.min_function_uuid = min_registry_entry->function_uuid;
  view.max_function_uuid = max_registry_entry->function_uuid;
  view.aggregate_function_profiles.reserve(aggregate_registry.size());
  for (const auto& entry : aggregate_registry) {
    if (entry.abi_version != 1 || entry.builtin_id.empty() ||
        entry.function_uuid.empty() || !entry.executable) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(
          SB_ENGINE_STATUS_INTERNAL_ERROR,
          out_result,
          4044,
          "ENGINE.STATEMENT_CONTEXT.AGGREGATE_REGISTRY_UNAVAILABLE",
          "engine.statement_context.aggregate_registry_unavailable");
    }
    view.aggregate_function_profiles.push_back(
        {entry.abi_version, entry.builtin_id, entry.function_uuid,
         entry.executable});
  }

  // QOW-SOURCE-WIN-001-STATEMENT-CONTEXT-REGISTRY-V1: native window
  // identities remain engine-owned. The parser receives an exact bounded
  // projection and cannot invent, rename, or substitute a function UUID.
  const auto window_registry =
      scratchbird::engine::executor::CanonicalWindowRuntimeRegistryV1();
  if (window_registry.size() != 11) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.WINDOW_REGISTRY_UNAVAILABLE",
        "engine.statement_context.window_registry_unavailable");
  }
  view.window_function_profiles.reserve(window_registry.size());
  std::unordered_set<std::string> window_builtin_ids;
  std::unordered_set<std::string> window_function_uuids;
  for (const auto& entry : window_registry) {
    if (entry.abi_version != 1 ||
        entry.function == scratchbird::engine::executor::
                              CanonicalWindowRuntimeFunction::unknown ||
        !entry.builtin_id.starts_with("sb.window.") ||
        entry.function_uuid.empty() || entry.aggregate_function.has_value() ||
        !window_builtin_ids.insert(entry.builtin_id).second ||
        !window_function_uuids.insert(entry.function_uuid).second) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(
          SB_ENGINE_STATUS_INTERNAL_ERROR,
          out_result,
          4044,
          "ENGINE.STATEMENT_CONTEXT.WINDOW_REGISTRY_UNAVAILABLE",
          "engine.statement_context.window_registry_unavailable");
    }
    view.window_function_profiles.push_back(
        {entry.abi_version, entry.builtin_id, entry.function_uuid, true});
  }

  std::string numeric_type_uuid;
  std::string text_type_uuid;
  std::string boolean_type_uuid;
  std::string json_type_uuid;
  std::string text_list_type_uuid;
  if (!issue_identity(&numeric_type_uuid) ||
      !issue_identity(&text_type_uuid) ||
      !issue_identity(&boolean_type_uuid) ||
      !issue_identity(&json_type_uuid) ||
      !issue_identity(&text_list_type_uuid)) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_descriptor_type_identity");
  }
  constexpr std::uint16_t kDescriptorSlotsPerProfile = 32;
  for (const auto kind : {
           StatementDescriptorProfileKind::kNumericNonNull,
           StatementDescriptorProfileKind::kNumericNullable,
           StatementDescriptorProfileKind::kTextNonNull,
           StatementDescriptorProfileKind::kTextNullable,
           StatementDescriptorProfileKind::kBooleanNonNull,
           StatementDescriptorProfileKind::kBooleanNullable,
           StatementDescriptorProfileKind::kJsonNonNull,
           StatementDescriptorProfileKind::kJsonNullable,
           StatementDescriptorProfileKind::kTextListNonNull,
           StatementDescriptorProfileKind::kTextListNullable}) {
    const bool numeric =
        kind == StatementDescriptorProfileKind::kNumericNonNull ||
        kind == StatementDescriptorProfileKind::kNumericNullable;
    const bool text =
        kind == StatementDescriptorProfileKind::kTextNonNull ||
        kind == StatementDescriptorProfileKind::kTextNullable;
    const bool json =
        kind == StatementDescriptorProfileKind::kJsonNonNull ||
        kind == StatementDescriptorProfileKind::kJsonNullable;
    const bool text_list =
        kind == StatementDescriptorProfileKind::kTextListNonNull ||
        kind == StatementDescriptorProfileKind::kTextListNullable;
    const bool nullable =
        kind == StatementDescriptorProfileKind::kNumericNullable ||
        kind == StatementDescriptorProfileKind::kTextNullable ||
        kind == StatementDescriptorProfileKind::kBooleanNullable ||
        kind == StatementDescriptorProfileKind::kJsonNullable ||
        kind == StatementDescriptorProfileKind::kTextListNullable;
    for (std::uint16_t slot = 0;
         slot < kDescriptorSlotsPerProfile;
         ++slot) {
      StatementDescriptorProfile profile;
      profile.profile_kind = kind;
      profile.slot = slot;
      profile.type_uuid =
          numeric ? numeric_type_uuid
                  : (text ? text_type_uuid
                          : (json ? json_type_uuid
                                  : (text_list ? text_list_type_uuid
                                               : boolean_type_uuid)));
      profile.nullable = nullable;
      if (!issue_identity(&profile.descriptor_uuid)) {
        scratchbird::transaction::mga::RevokePublishedSnapshotVector(
            snapshot.snapshot_uuid);
        return fail_result(
            SB_ENGINE_STATUS_INTERNAL_ERROR,
            out_result,
            4044,
            "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
            "engine.statement_context.identity_unavailable",
            "statement_descriptor_profile_identity");
      }
      view.descriptor_profiles.push_back(std::move(profile));
    }
  }

  // QOW-SOURCE-RCP-077-STATEMENT-REAL64-DESCRIPTORS-V8: the core catalog
  // owns the REAL64 type identity; the statement-context issuer owns the two
  // distinct result-slot descriptor identities. No parser-side pairing or
  // descriptor reuse is permitted.
  const auto core_manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  const auto real64_count =
      core_manifest.ok()
          ? std::ranges::count_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "real64"; })
          : 0;
  const auto real64_row =
      core_manifest.ok()
          ? std::ranges::find_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "real64"; })
          : core_manifest.manifest.descriptor_rows.end();
  if (!core_manifest.ok() || real64_count != 1 ||
      real64_row == core_manifest.manifest.descriptor_rows.end() ||
      !real64_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.REAL64_DESCRIPTOR_UNAVAILABLE",
        "engine.statement_context.real64_descriptor_unavailable");
  }
  const auto real64_type_uuid = scratchbird::core::uuid::UuidToString(
      real64_row->descriptor_uuid.value);
  std::array<std::string, 2> real64_descriptor_uuids;
  if (real64_type_uuid.empty() ||
      !issue_identity(&real64_descriptor_uuids[0]) ||
      !issue_identity(&real64_descriptor_uuids[1]) ||
      real64_descriptor_uuids[0] == real64_descriptor_uuids[1]) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_real64_descriptor_identity");
  }
  for (std::uint16_t slot = 0; slot < real64_descriptor_uuids.size(); ++slot) {
    StatementDescriptorProfile profile;
    profile.profile_kind =
        StatementDescriptorProfileKind::kReal64NonNull;
    profile.slot = slot;
    profile.descriptor_uuid = std::move(real64_descriptor_uuids[slot]);
    profile.type_uuid = real64_type_uuid;
    view.descriptor_profiles.push_back(std::move(profile));
  }

  view.statement_uuid = statement_uuid;
  view.statement_timestamp = engine_context.statement_timestamp;
  if (!canonical_statement_timestamp(view.statement_timestamp) ||
      view.statement_timestamp != engine_context.current_timestamp) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_timestamp_carrier_mismatch");
  }
  view.owning_transaction_uuid =
      std::string(request->exact_transaction_uuid);
  view.statement_snapshot_uuid = statement_snapshot_uuid;
  view.security_context_uuid =
      engine_context.authorization_context.authority_uuid.canonical;
  view.owning_local_transaction_id =
      snapshot.owning_transaction.value;
  view.visible_committed_high_watermark =
      snapshot.visible_committed_high_watermark;
  view.oldest_active_local_transaction_id =
      snapshot.oldest_active_transaction.value;
  view.oldest_interesting_local_transaction_id =
      snapshot.oldest_interesting_transaction.value;
  view.oldest_snapshot_local_transaction_id =
      snapshot.oldest_snapshot_transaction.value;
  view.retention_horizon_local_transaction_id =
      snapshot.retention_horizon_transaction.value;
  view.publication_inventory_next_local_transaction_id =
      snapshot.publication_inventory_next_local_transaction_id;
  view.active_excluded_local_transaction_ids =
      snapshot.active_excluded_local_transaction_ids;
  view.in_doubt_excluded_local_transaction_ids =
      snapshot.in_doubt_excluded_local_transaction_ids;
  view.inventory_authoritative = snapshot.inventory_authoritative;
  view.snapshot_complete = snapshot.complete;
  view.catalog_generation_id = engine_context.catalog_generation_id;
  view.security_epoch = engine_context.security_epoch;
  view.resource_epoch = engine_context.resource_epoch;
  view.optimizer_route_epoch = engine_context.optimizer_route_epoch;
  view.optimizer_route_generation =
      engine_context.optimizer_route_generation;
  view.optimizer_memory_budget_bytes =
      engine_context.optimizer_memory_budget_bytes;
  view.optimizer_maximum_candidate_count =
      engine_context.optimizer_maximum_candidate_count;
  view.optimizer_maximum_memo_groups =
      engine_context.optimizer_maximum_memo_groups;
  view.optimizer_maximum_search_steps =
      engine_context.optimizer_maximum_search_steps;
  view.optimizer_maximum_planning_time_ns =
      engine_context.optimizer_maximum_planning_time_ns;
  view.optimizer_spill_allowed = engine_context.optimizer_spill_allowed;

  engine_context.statement_snapshot_uuid.canonical =
      view.statement_snapshot_uuid;
  engine_context.snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  engine_context.statement_metadata_snapshot_engine_owned = true;
  engine_context.statement_metadata_snapshot_uuid.canonical =
      view.statement_metadata_snapshot_uuid;
  engine_context
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  engine_context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids =
      view.active_excluded_local_transaction_ids;
  engine_context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      view.in_doubt_excluded_local_transaction_ids;
  engine_context.catalog_epoch_uuid.canonical = view.catalog_epoch_uuid;
  engine_context.optimizer_capability_snapshot_uuid.canonical =
      view.optimizer_capability_snapshot_uuid;
  engine_context.optimizer_resource_snapshot_uuid.canonical =
      view.optimizer_resource_snapshot_uuid;
  engine_context.optimizer_route_snapshot_uuid.canonical =
      view.optimizer_route_snapshot_uuid;
  engine_context.trace_tags.push_back("private_statement_context_receipt");

  auto receipt = std::unique_ptr<StatementContextReceiptOpaque>(
      new (std::nothrow) StatementContextReceiptOpaque());
  if (!receipt) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
        out_result,
        4047,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_ALLOCATION_FAILED",
        "engine.statement_context.receipt_allocation_failed");
  }
  receipt->engine = session->engine;
  receipt->session = session;
  receipt->view = view;
  receipt->snapshot_vector = snapshot;
  receipt->engine_context = std::move(engine_context);
  const auto receipt_id = g_next_statement_context_receipt_id.fetch_add(
      1, std::memory_order_relaxed);
  if (receipt_id == 0) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
        out_result,
        4048,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_ID_EXHAUSTED",
        "engine.statement_context.receipt_id_exhausted");
  }
  {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    if (!g_live_statement_context_receipts
             .emplace(receipt_id, std::move(receipt))
             .second) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(
          SB_ENGINE_STATUS_CONFLICT,
          out_result,
          4049,
          "ENGINE.STATEMENT_CONTEXT.RECEIPT_ID_COLLISION",
          "engine.statement_context.receipt_id_collision");
    }
  }
  *out_view = view;
  *out_receipt = StatementContextReceiptHandle{receipt_id};
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t ReleaseStatementContextReceipt(
    StatementContextReceiptHandle receipt) {
  if (!receipt) return SB_ENGINE_STATUS_INVALID_HANDLE;
  scratchbird::core::platform::TypedUuid published_snapshot_uuid;
  std::unique_ptr<StatementContextReceiptOpaque> released;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    const auto live =
        g_live_statement_context_receipts.find(receipt.opaque_id);
    if (live == g_live_statement_context_receipts.end()) {
      return receipt.opaque_id <
                     g_next_statement_context_receipt_id.load(
                         std::memory_order_relaxed)
                 ? SB_ENGINE_STATUS_ALREADY_RELEASED
                 : SB_ENGINE_STATUS_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> receipt_guard(live->second->mutex);
    if (live->second->released ||
        live->second->magic != kStatementContextReceiptMagic) {
      return SB_ENGINE_STATUS_ALREADY_RELEASED;
    }
    live->second->released = true;
    live->second->magic = 0;
    published_snapshot_uuid = live->second->snapshot_vector.snapshot_uuid;
    released = std::move(live->second);
    g_live_statement_context_receipts.erase(live);
  }
  scratchbird::transaction::mga::RevokePublishedSnapshotVector(
      published_snapshot_uuid);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t DispatchStatementContextReceipt(
    const StatementContextDispatchRequest* request,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (request == nullptr || !request->receipt || out_result == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4050,
        "ENGINE.STATEMENT_CONTEXT.DISPATCH_REQUEST_INVALID",
        "engine.statement_context.dispatch_request_invalid");
  }
  if (!request->data_packet.empty()) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4051,
        "SBLR.DATA_PACKET.OPERATION_MISMATCH",
        "sblr.data_packet.operation_mismatch",
        "query.execute does not accept an out-of-band data packet");
  }

  std::unique_lock<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(
      request->receipt.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_HANDLE,
        out_result,
        4052,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_NOT_LIVE",
        "engine.statement_context.receipt_not_live");
  }
  auto* receipt = live->second.get();
  std::unique_lock<std::mutex> receipt_guard(receipt->mutex);
  registry_guard.unlock();
  if (receipt->released || receipt->magic != kStatementContextReceiptMagic ||
      (request->engine_session != nullptr &&
       request->engine_session != receipt->session)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_HANDLE,
        out_result,
        4053,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_MISMATCH",
        "engine.statement_context.receipt_mismatch");
  }

  const auto hash_matches = [](const std::vector<std::uint8_t>& bytes,
                               const std::array<std::uint8_t, 32>& expected) {
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
    return digest.ok() && digest.digest == expected;
  };
  if (request->canonical_container_bytes.empty() ||
      request->canonical_execution_envelope_bytes.empty() ||
      request->canonical_operation_bytes.empty() ||
      !hash_matches(request->canonical_container_bytes,
                    request->container_sha256) ||
      !hash_matches(request->canonical_execution_envelope_bytes,
                    request->execution_envelope_sha256) ||
      !hash_matches(request->canonical_operation_bytes,
                    request->operation_sha256)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4054,
        "SBLR.INGRESS_REVALIDATION_FAILED",
        "sblr.ingress_revalidation_failed",
        "canonical admission bytes or SHA-256 evidence differ");
  }

  std::vector<std::uint8_t> admission_binding;
  constexpr std::string_view kAdmissionDomain =
      "ScratchBird.SBLR.AdmissionToken.V1";
  admission_binding.insert(admission_binding.end(),
                           kAdmissionDomain.begin(),
                           kAdmissionDomain.end());
  admission_binding.insert(admission_binding.end(),
                           request->container_sha256.begin(),
                           request->container_sha256.end());
  admission_binding.insert(admission_binding.end(),
                           request->execution_envelope_sha256.begin(),
                           request->execution_envelope_sha256.end());
  admission_binding.insert(admission_binding.end(),
                           request->operation_sha256.begin(),
                           request->operation_sha256.end());
  for (const auto* value : {&request->authenticated_principal_uuid,
                            &request->catalog_snapshot_uuid,
                            &request->engine_mga_statement_uuid,
                            &request->engine_mga_snapshot_uuid}) {
    admission_binding.insert(admission_binding.end(),
                             value->begin(), value->end());
    admission_binding.push_back(0);
  }
  scratchbird::engine::SblrAppendU64(admission_binding,
                                     request->catalog_epoch);
  scratchbird::engine::SblrAppendU64(admission_binding,
                                     request->security_epoch);
  scratchbird::engine::SblrAppendU64(admission_binding,
                                     request->resource_epoch);
  const auto binding_digest =
      scratchbird::core::hash::ComputeSha256Digest(admission_binding);
  if (!binding_digest.ok() ||
      binding_digest.digest != request->admission_binding_sha256) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4055,
        "SBLR.INGRESS_REVALIDATION_FAILED",
        "sblr.ingress_revalidation_failed",
        "immutable admission binding digest differs");
  }

  const auto container = scratchbird::engine::DecodeSblrContainerBytes(
      request->canonical_container_bytes.data(),
      request->canonical_container_bytes.size());
  const auto ingress =
      scratchbird::engine::DecodeSblrExecutionEnvelopeV1Bytes(
          request->canonical_execution_envelope_bytes.data(),
          request->canonical_execution_envelope_bytes.size());
  if (container.status != scratchbird::engine::SblrCodecStatus::ok ||
      ingress.status != scratchbird::engine::SblrCodecStatus::ok) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4056,
        "SBLR.INGRESS_REVALIDATION_FAILED",
        "sblr.ingress_revalidation_failed",
        "canonical outer container or SBEE re-decode failed");
  }
  scratchbird::engine::SblrExecutionEnvelopeSemanticView ingress_view;
  if (!scratchbird::engine::SblrValidateExecutionEnvelopeFields(
          ingress.envelope, &ingress_view) ||
      ingress_view.payload_kind !=
          scratchbird::engine::SblrPayloadKind::operation_envelope ||
      ingress_view.operation_ref_kind != 1 ||
      ingress_view.operation_inline_data == nullptr ||
      ingress_view.operation_inline_size !=
          request->canonical_operation_bytes.size() ||
      container.container.operation_payload !=
          request->canonical_operation_bytes ||
      !std::equal(request->canonical_operation_bytes.begin(),
                  request->canonical_operation_bytes.end(),
                  ingress_view.operation_inline_data)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4057,
        "SBLR.ENVELOPE.CHECKSUM_MISMATCH",
        "sblr.envelope.checksum_mismatch",
        "outer SBLR and SBEE do not carry the admitted exact SBOP bytes");
  }
  const std::string_view operation_bytes(
      reinterpret_cast<const char*>(request->canonical_operation_bytes.data()),
      request->canonical_operation_bytes.size());
  const auto operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(operation_bytes);
  if (!operation.ok || operation.envelope.operation_id != "query.execute" ||
      operation.envelope.opcode_code != 0x1207 ||
      operation.envelope.opcode != "SBLR_QUERY_EXECUTE") {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4058,
        "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
        "sblr.operation.opcode_identity_mismatch",
        "private statement receipt dispatch admits query.execute only");
  }

  const auto uuid_text = [](const std::uint8_t* bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) {
        text.push_back('-');
      }
      text.push_back(kHex[bytes[index] >> 4]);
      text.push_back(kHex[bytes[index] & 0x0f]);
    }
    return text;
  };
  const auto& anchor = container.container.canonical_anchor;
  const auto& view = receipt->view;
  const auto& context = receipt->engine_context;
  const auto& dialect_field = ingress.envelope.fields[10];
  const auto& user_field = ingress.envelope.fields[11];
  const bool exact_receipt_binding =
      request->engine_mga_statement_uuid == view.statement_uuid &&
      request->engine_mga_snapshot_uuid == view.statement_snapshot_uuid &&
      request->catalog_snapshot_uuid ==
          view.statement_metadata_snapshot_uuid &&
      request->catalog_epoch == view.catalog_generation_id &&
      request->security_epoch == view.security_epoch &&
      request->resource_epoch == view.resource_epoch &&
      request->authenticated_principal_uuid ==
          context.principal_uuid.canonical &&
      uuid_text(anchor.data()) == context.database_uuid.canonical &&
      uuid_text(anchor.data() + 76) == view.catalog_epoch_uuid &&
      uuid_text(anchor.data() + 116) == view.statement_uuid &&
      operation.envelope.parser_package_uuid == uuid_text(anchor.data() + 32) &&
      operation.envelope.registry_snapshot_uuid == view.catalog_epoch_uuid &&
      ingress.envelope.fields[0].size() == 16 &&
      uuid_text(ingress.envelope.fields[0].data()) == view.statement_uuid &&
      dialect_field.size() == 17 && dialect_field[0] == 1 &&
      std::equal(dialect_field.begin() + 1, dialect_field.end(),
                 anchor.begin() + 16) &&
      user_field.size() == 17 && user_field[0] == 1 &&
      uuid_text(user_field.data() + 1) ==
          request->authenticated_principal_uuid;
  if (!exact_receipt_binding) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4059,
        "ENGINE.STATEMENT_CONTEXT.DISPATCH_BINDING_MISMATCH",
        "engine.statement_context.dispatch_binding_mismatch");
  }

  // SBOP v1 freezes operation identity and typed operands but does not encode
  // duplicate authority booleans.  The engine opcode registry owns this
  // requirement; project it only after the exact query.execute identity and
  // receipt binding above have both been revalidated.
  auto dispatch_operation = operation.envelope;
  dispatch_operation.requires_transaction_context = true;

  scratchbird::engine::internal_api::EngineResolveStatementSnapshotRequest
      snapshot_request;
  snapshot_request.context = context;
  const auto current_snapshot =
      scratchbird::engine::internal_api::EngineResolveStatementSnapshot(
          snapshot_request);
  if (!current_snapshot.ok || !current_snapshot.snapshot_vector.complete ||
      !current_snapshot.snapshot_vector.inventory_authoritative ||
      current_snapshot.snapshot_vector.snapshot_uuid.kind !=
          receipt->snapshot_vector.snapshot_uuid.kind ||
      current_snapshot.snapshot_vector.snapshot_uuid.value !=
          receipt->snapshot_vector.snapshot_uuid.value ||
      current_snapshot.snapshot_vector.owning_transaction.value !=
          view.owning_local_transaction_id ||
      current_snapshot.snapshot_vector.visible_committed_high_watermark !=
          view.visible_committed_high_watermark) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4060,
        "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_STALE",
        "engine.statement_context.snapshot_stale");
  }

  scratchbird::engine::internal_api::EngineApiRequest api_request;
  const auto dispatched =
      scratchbird::engine::sblr::DispatchSblrOperation(
          {context, std::move(dispatch_operation),
           std::move(api_request)});
  if (!dispatched.accepted || !dispatched.api_result.ok) {
    return fail_result(
        operation_envelope_failure_status(dispatched),
        out_result,
        4063,
        operation_envelope_failure_code(dispatched),
        "sblr.operation_envelope.rejected",
        first_dispatch_diagnostic_detail(dispatched),
        first_dispatch_diagnostic_fields(dispatched));
  }

  auto* result = make_result(SB_ENGINE_RESULT_ROW_BATCH,
                             dispatched.api_result.operation_id);
  result->affected_rows = dispatched.api_result.dml_summary.rows_changed;
  result->result_kind = dispatched.api_result.result_shape.result_kind;
  result->rows_produced = static_cast<std::uint64_t>(
      dispatched.api_result.result_shape.rows.size());
  result->row_values = api_row_values(dispatched.api_result);
  result->row_metadata_values =
      api_row_metadata_values(dispatched.api_result);
  result->evidence_values = api_evidence_values(dispatched.api_result);
  result->payload = api_result_payload(dispatched.api_result);
  finalize_diagnostics(result);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
}

bool CopyEngineDiagnosticFields(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    std::vector<EngineDiagnosticField>* fields) {
  if (fields == nullptr) return false;
  fields->clear();
  if (!valid_result(result) || diagnostic_index >= result->diagnostics.size()) {
    return false;
  }
  const auto& diagnostic = result->diagnostics[diagnostic_index];
  fields->reserve(diagnostic.fields.size());
  for (const auto& field : diagnostic.fields) {
    fields->push_back({field.key, field.value});
  }
  return true;
}

void SetPreparedMetadataBindingDispatchTestHookForTesting(
    PreparedMetadataBindingDispatchTestHook hook,
    void* context) {
  std::lock_guard<std::mutex> guard(
      g_prepared_metadata_dispatch_test_hook_mutex);
  g_prepared_metadata_dispatch_test_hook = hook;
  g_prepared_metadata_dispatch_test_hook_context = context;
}

sb_engine_status_t CreatePreparedMetadataBinding(
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t* prepare_context,
    std::string_view sealed_prepare_transaction_uuid,
    const sb_engine_sblr_dispatch_params_v1_t* invoke_params,
    PreparedMetadataBindingHandle* out_binding,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_binding == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       1003,
                       "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_binding = nullptr;
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,
                       out_result,
                       1007,
                       "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (prepare_context == nullptr || invoke_params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       1004,
                       "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(prepare_context->struct_size,
                             prepare_context->abi_version,
                             sizeof(sb_engine_request_context_v1_t),
                             out_result);
  if (status != SB_ENGINE_STATUS_OK) return status;
  status = check_struct(invoke_params->struct_size,
                        invoke_params->abi_version,
                        sizeof(sb_engine_sblr_dispatch_params_v1_t),
                        out_result);
  if (status != SB_ENGINE_STATUS_OK) return status;
  return fail_result(
      SB_ENGINE_STATUS_UNSUPPORTED,
      out_result,
      4032,
      "SBLR.ENVELOPE.FIELD_MISSING",
      "sblr.envelope.field_missing",
      "prepared binding requires an immutable server admission token");
#if 0
  if (!nonzero_uuid(prepare_context->effective_user_uuid) ||
      !nonzero_uuid(prepare_context->session_uuid) ||
      prepare_context->rights_set_ref == 0) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        2001,
        "SECURITY.IDENTITY.MISSING",
        "security.identity.missing",
        "prepared metadata binding requires principal, session, and rights context");
  }
  if (!same_uuid(prepare_context->effective_user_uuid,
                 session->effective_user_uuid) ||
      !same_uuid(prepare_context->session_uuid,
                 session->public_session_uuid) ||
      prepare_context->trust_mode != session->trust_mode) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4028,
        "ENGINE.PREPARED_METADATA_BINDING.SESSION_IDENTITY_MISMATCH",
        "engine.prepared_metadata_binding.session_identity_mismatch",
        "prepare identity must match the engine session identity");
  }
  if (prepare_context->transaction_ref == 0) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        3003,
        "ENGINE.PREPARED_METADATA_BINDING.TRANSACTION_REQUIRED",
        "engine.prepared_metadata_binding.transaction_required",
        "prepare_context.transaction_ref");
  }
  if (invoke_params->reserved0 != 0 || invoke_params->reserved1 != 0 ||
      invoke_params->data_packet_size_bytes != 0 ||
      invoke_params->data_packet_bytes != nullptr ||
      invoke_params->envelope_size_bytes == 0 ||
      invoke_params->envelope_bytes == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4017,
        "ENGINE.PREPARED_METADATA_BINDING.PARAMS_INVALID",
        "engine.prepared_metadata_binding.params_invalid",
        "binding create admits one envelope without data or reserved handles");
  }

  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      invoke_params->envelope_bytes, invoke_params->envelope_size_bytes);
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded.envelope.payload_kind !=
          scratchbird::engine::SblrPayloadKind::operation_envelope ||
      !looks_like_sblr_operation_envelope(decoded.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4018,
        "ENGINE.PREPARED_METADATA_BINDING.SBLR_INVALID",
        "engine.prepared_metadata_binding.sblr_invalid",
        decoded.diagnostic_code.empty()
            ? "operation_envelope_required"
            : std::string(decoded.diagnostic_code));
  }
  const auto* canonical_data = reinterpret_cast<const char*>(
      decoded.envelope.canonical_bytes.data());
  const std::string_view canonical(
      canonical_data, decoded.envelope.canonical_bytes.size());
  const auto operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(canonical);
  if (!operation.ok ||
      operation.envelope.operation_id != "routine.procedure_invoke" ||
      operation.envelope.contains_sql_text ||
      !operation.envelope.parser_resolved_names_to_uuids ||
      !operation.envelope.requires_security_context ||
      !operation.envelope.requires_transaction_context ||
      has_engine_only_prepared_metadata_operand(operation.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED,
        out_result,
        4019,
        "ENGINE.PREPARED_METADATA_BINDING.ROUTINE_INVOKE_REQUIRED",
        "engine.prepared_metadata_binding.routine_invoke_required",
        "only engine-validated UUID-bound procedure invocation is supported");
  }
  const std::string target_object_uuid =
      operation_operand_value(operation.envelope, "target_object_uuid");
  const std::string target_object_kind =
      operation_operand_value(operation.envelope, "target_object_kind");
  if (target_object_uuid.empty() || target_object_kind != "procedure" ||
      !scratchbird::core::uuid::ParseUuid(target_object_uuid).ok()) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4020,
        "ENGINE.PREPARED_METADATA_BINDING.UUID_TARGET_REQUIRED",
        "engine.prepared_metadata_binding.uuid_target_required",
        target_object_uuid.empty() ? "target_object_uuid"
                                   : target_object_uuid);
  }

  const auto inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          session->engine->database_path);
  if (!inventory.ok()) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4021,
        "ENGINE.PREPARED_METADATA_BINDING.INVENTORY_UNAVAILABLE",
        "engine.prepared_metadata_binding.inventory_unavailable",
        inventory.diagnostic.diagnostic_code);
  }
  const auto prepare_transaction =
      scratchbird::transaction::mga::LookupLocalTransaction(
          inventory.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              prepare_context->transaction_ref));
  using scratchbird::transaction::mga::TransactionState;
  if (!prepare_transaction.ok() ||
      (prepare_transaction.entry.state != TransactionState::active &&
       prepare_transaction.entry.state != TransactionState::read_only_active &&
       prepare_transaction.entry.state != TransactionState::preparing &&
       prepare_transaction.entry.state != TransactionState::prepared)) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        4022,
        "ENGINE.PREPARED_METADATA_BINDING.TRANSACTION_NOT_ACTIVE",
        "engine.prepared_metadata_binding.transaction_not_active",
        std::to_string(prepare_context->transaction_ref));
  }
  if (sealed_prepare_transaction_uuid.empty() ||
      !prepare_transaction.entry.identity.transaction_uuid.valid() ||
      scratchbird::core::uuid::UuidToString(
          prepare_transaction.entry.identity.transaction_uuid.value) !=
          sealed_prepare_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4032,
        "ENGINE.PREPARED_METADATA_BINDING.EXACT_MGA_SELECTOR_MISMATCH",
        "engine.prepared_metadata_binding.exact_mga_selector_mismatch",
        "prepare local transaction ID and sealed UUID do not match");
  }

  const std::uint64_t high_water =
      inventory.inventory.next_local_transaction_id == 0
          ? 0
          : inventory.inventory.next_local_transaction_id - 1;
  std::vector<std::uint64_t> active_excluded;
  std::vector<std::uint64_t> in_doubt_excluded;
  for (const auto& entry : inventory.inventory.entries) {
    if (!entry.identity.local_id.valid() ||
        entry.identity.local_id.value > high_water) {
      continue;
    }
    if (active_metadata_snapshot_exclusion(entry.state)) {
      active_excluded.push_back(entry.identity.local_id.value);
    } else if (in_doubt_metadata_snapshot_exclusion(entry.state)) {
      in_doubt_excluded.push_back(entry.identity.local_id.value);
    }
  }
  std::sort(active_excluded.begin(), active_excluded.end());
  std::sort(in_doubt_excluded.begin(), in_doubt_excluded.end());

  const std::string metadata_snapshot_uuid =
      new_prepared_metadata_snapshot_uuid();
  if (metadata_snapshot_uuid.empty()) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4023,
        "ENGINE.PREPARED_METADATA_BINDING.SNAPSHOT_ID_UNAVAILABLE",
        "engine.prepared_metadata_binding.snapshot_id_unavailable");
  }
  auto metadata_context =
      make_internal_context(session->engine, *prepare_context);
  metadata_context.statement_metadata_snapshot_engine_owned = true;
  metadata_context.statement_metadata_snapshot_uuid.canonical =
      metadata_snapshot_uuid;
  metadata_context
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      high_water;
  metadata_context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids =
      active_excluded;
  metadata_context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      in_doubt_excluded;
  const auto lifecycle =
      scratchbird::engine::internal_api::LoadExecutableObjectLifecycleState(
          metadata_context);
  if (!lifecycle.ok) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4024,
        lifecycle.diagnostic.code,
        lifecycle.diagnostic.message_key,
        lifecycle.diagnostic.detail);
  }
  const scratchbird::engine::internal_api::EngineExecutableObjectRecord*
      pinned_object = nullptr;
  for (const auto& object : lifecycle.state.objects) {
    if (object.object_uuid == target_object_uuid) {
      pinned_object = &object;
      break;
    }
  }
  if (pinned_object == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_NOT_FOUND,
        out_result,
        4025,
        "ENGINE.PREPARED_METADATA_BINDING.OBJECT_NOT_VISIBLE",
        "engine.prepared_metadata_binding.object_not_visible",
        target_object_uuid);
  }
  const auto creator =
      scratchbird::transaction::mga::LookupLocalTransaction(
          inventory.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              pinned_object->creator_tx));
  if (!creator.ok() ||
      (creator.entry.state != TransactionState::committed &&
       creator.entry.state != TransactionState::archived)) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4026,
        "ENGINE.PREPARED_METADATA_BINDING.OBJECT_NOT_COMMITTED",
        "engine.prepared_metadata_binding.object_not_committed",
        target_object_uuid);
  }
  if (pinned_object->object_kind != "procedure" ||
      pinned_object->lifecycle_state != "active" ||
      pinned_object->deleted || pinned_object->invalidated ||
      pinned_object->executor_kind == "metadata_only" ||
      pinned_object->executable_generation == 0 ||
      pinned_object->metadata_epoch == 0) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4027,
        "ENGINE.PREPARED_METADATA_BINDING.OBJECT_NOT_EXECUTABLE",
        "engine.prepared_metadata_binding.object_not_executable",
        target_object_uuid);
  }

  auto* binding = new PreparedMetadataBindingOpaque();
  binding->engine = session->engine;
  binding->session = session;
  binding->database_path = session->engine->database_path;
  binding->database_uuid = session->engine->database_uuid;
  binding->effective_user_uuid = prepare_context->effective_user_uuid;
  binding->session_uuid = prepare_context->session_uuid;
  binding->parser_package_uuid = prepare_context->parser_package_uuid;
  binding->dialect_profile_uuid = prepare_context->dialect_profile_uuid;
  binding->trust_mode = prepare_context->trust_mode;
  binding->context_flags = prepare_context->flags;
  binding->rights_set_ref = prepare_context->rights_set_ref;
  binding->capability_set_ref = prepare_context->capability_set_ref;
  binding->source_artifact_set_ref =
      prepare_context->source_artifact_set_ref;
  binding->encoded_sblr_envelope.assign(
      invoke_params->envelope_bytes,
      invoke_params->envelope_bytes + invoke_params->envelope_size_bytes);
  binding->metadata_snapshot_uuid = metadata_snapshot_uuid;
  binding->metadata_visible_through_local_transaction_id = high_water;
  binding->active_excluded_local_transaction_ids =
      std::move(active_excluded);
  binding->in_doubt_excluded_local_transaction_ids =
      std::move(in_doubt_excluded);
  binding->target_object_uuid = target_object_uuid;
  binding->target_executable_generation =
      pinned_object->executable_generation;
  binding->target_metadata_epoch = pinned_object->metadata_epoch;
  binding->target_creator_local_transaction_id = pinned_object->creator_tx;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_prepared_metadata_binding_registry_mutex);
    g_prepared_metadata_bindings.insert(binding);
  }
  *out_binding = binding;

  if (out_result != nullptr) {
    auto* result = make_result(SB_ENGINE_RESULT_COMMAND_COMPLETION,
                               "sblr.prepared_metadata_binding.create");
    result->payload =
        "metadata_snapshot_uuid=" + metadata_snapshot_uuid + "\n" +
        "target_object_uuid=" + target_object_uuid + "\n" +
        "executable_generation=" +
        std::to_string(binding->target_executable_generation) + "\n" +
        "metadata_epoch=" +
        std::to_string(binding->target_metadata_epoch) + "\n";
    finalize_diagnostics(result);
    *out_result = result;
  }
  return SB_ENGINE_STATUS_OK;
#endif
}

sb_engine_status_t ReleasePreparedMetadataBinding(
    PreparedMetadataBindingHandle binding) {
  if (binding == nullptr) return SB_ENGINE_STATUS_OK;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_prepared_metadata_binding_registry_mutex);
    const auto found = g_prepared_metadata_bindings.find(binding);
    if (found == g_prepared_metadata_bindings.end()) {
      return SB_ENGINE_STATUS_INVALID_HANDLE;
    }
    {
      std::lock_guard<std::mutex> binding_guard(binding->mutex);
      if (binding->released ||
          binding->magic != kPreparedMetadataBindingMagic) {
        return SB_ENGINE_STATUS_ALREADY_RELEASED;
      }
      binding->released = true;
      binding->magic = 0;
    }
    g_prepared_metadata_bindings.erase(found);
  }
  delete binding;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t DispatchWithPreparedMetadataBinding(
    sb_engine_session_t session,
    sb_engine_transaction_t transaction,
    const sb_engine_request_context_v1_t* context,
    const sb_engine_sblr_dispatch_params_v1_t* params,
    PreparedMetadataBindingHandle binding,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) { return SB_ENGINE_STATUS_INVALID_ARGUMENT; }
  if (!valid_session(session) || binding == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,
                       out_result,
                       1007,
                       "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (transaction != nullptr &&
      (!valid_transaction(transaction) || transaction->session != session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,
                       out_result,
                       1007,
                       "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (context == nullptr || params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       1004,
                       "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(context->struct_size,
                             context->abi_version,
                             sizeof(sb_engine_request_context_v1_t),
                             out_result);
  if (status != SB_ENGINE_STATUS_OK) { return status; }
  status = check_struct(params->struct_size,
                        params->abi_version,
                        sizeof(sb_engine_sblr_dispatch_params_v1_t),
                        out_result);
  if (status != SB_ENGINE_STATUS_OK) { return status; }
  return fail_result(
      SB_ENGINE_STATUS_UNSUPPORTED,
      out_result,
      4032,
      "SBLR.ENVELOPE.FIELD_MISSING",
      "sblr.envelope.field_missing",
      "prepared dispatch requires an immutable server admission token");
#if 0
  if (params->reserved0 != 0 || params->reserved1 != 0) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       4014,
                       "ENGINE.ABI.RESERVED_FIELD_INVALID",
                       "engine.abi.reserved_field_invalid",
                       "sblr_dispatch_params.reserved0_or_reserved1");
  }
  if (!nonzero_uuid(context->effective_user_uuid) ||
      !nonzero_uuid(context->session_uuid)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED,
                       out_result,
                       2001,
                       "SECURITY.IDENTITY.MISSING",
                       "security.identity.missing");
  }
  if (params->envelope_size_bytes == 0 || params->envelope_bytes == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4001,
        "SBLR.ENVELOPE.INVALID",
        "sblr.envelope.invalid",
        "prepared metadata dispatch requires one operation envelope");
  }
  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      params->envelope_bytes, params->envelope_size_bytes);
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok) {
    return fail_result(
        decoded.status ==
                scratchbird::engine::SblrCodecStatus::version_unsupported
            ? SB_ENGINE_STATUS_UNSUPPORTED
            : SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4001,
        std::string(decoded.diagnostic_code),
        std::string(decoded.message_key));
  }
  if (!looks_like_sblr_operation_envelope(decoded.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED,
        out_result,
        4029,
        "ENGINE.PREPARED_METADATA_BINDING.OPERATION_ENVELOPE_REQUIRED",
        "engine.prepared_metadata_binding.operation_envelope_required");
  }
  // PreparedMetadataBinding is a private routed-server bridge, not a public or
  // embedded ABI handle. The server's database-owner lock excludes another
  // mutating process; this engine-internal guard orders same-process durable
  // transaction publication. Keep it through dispatch so the exact version
  // revalidated below is the immutable version acquired and executed.
  const auto inventory_guard =
      scratchbird::engine::internal_api::AcquireTransactionInventoryGuard(
          session->engine->database_path);
  PreparedMetadataBindingSnapshot prepared_metadata;
  std::string revalidation_detail;
  const auto revalidation =
      revalidate_prepared_metadata_binding_current_version(
          binding,
          session,
          *context,
          *params,
          &prepared_metadata,
          &revalidation_detail);
  if (revalidation != PreparedMetadataCurrentVersionStatus::ok) {
    if (revalidation == PreparedMetadataCurrentVersionStatus::stale) {
      return fail_result(
          SB_ENGINE_STATUS_CONFLICT,
          out_result,
          4030,
          "ENGINE.PREPARED_METADATA_BINDING.STALE",
          "engine.prepared_metadata_binding.stale",
          revalidation_detail);
    }
    if (revalidation == PreparedMetadataCurrentVersionStatus::unavailable) {
      return fail_result(
          SB_ENGINE_STATUS_INTERNAL_ERROR,
          out_result,
          4031,
          "ENGINE.PREPARED_METADATA_BINDING.REVALIDATION_UNAVAILABLE",
          "engine.prepared_metadata_binding.revalidation_unavailable",
          revalidation_detail);
    }
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4015,
        "ENGINE.PREPARED_METADATA_BINDING.CONTEXT_MISMATCH",
        "engine.prepared_metadata_binding.context_mismatch",
        revalidation_detail);
  }
  invoke_prepared_metadata_dispatch_test_hook(
      "exact_version_acquired_under_inventory_guard");
  return dispatch_operation_envelope(
      session,
      *context,
      decoded.envelope,
      *params,
      &prepared_metadata,
      out_result);
#endif
}

}  // namespace scratchbird::server_engine_bridge

extern "C" {

sb_engine_status_t sb_engine_dispatch_sblr(sb_engine_session_t session,
                                           sb_engine_transaction_t transaction,
                                           const sb_engine_request_context_v1_t* context,
                                           const sb_engine_sblr_dispatch_params_v1_t* params,
                                           sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (transaction != nullptr && !valid_transaction(transaction)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (context == nullptr || params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(context->struct_size, context->abi_version, sizeof(sb_engine_request_context_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_sblr_dispatch_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  if (params->reserved0 != 0 || params->reserved1 != 0) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       4014,
                       "ENGINE.ABI.RESERVED_FIELD_INVALID",
                       "engine.abi.reserved_field_invalid",
                       "sblr_dispatch_params.reserved0_or_reserved1");
  }
  if (params->envelope_size_bytes != 0) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED,
        out_result,
        4032,
        "SBLR.ENVELOPE.FIELD_MISSING",
        "sblr.envelope.field_missing",
        "public ABI revision requires an immutable server admission token");
  }
  if (!nonzero_uuid(context->effective_user_uuid) || !nonzero_uuid(context->session_uuid)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 2001, "SECURITY.IDENTITY.MISSING",
                       "security.identity.missing");
  }
  if (params->envelope_size_bytes != 0 && params->envelope_bytes == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4001, "SBLR.ENVELOPE.INVALID",
                       "sblr.envelope.invalid", "null envelope pointer with non-zero length");
  }
  if (params->envelope_size_bytes == 0) {
    auto* result = make_result(SB_ENGINE_RESULT_CAPABILITY_REPORT, "sblr.dispatch.capability");
    result->payload = "SBLR dispatch facade active; empty envelope treated as capability probe";
    finalize_diagnostics(result);
    *out_result = result;
    return SB_ENGINE_STATUS_OK;
  }
  auto phase_last = EngineAbiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(4);
  const auto mark_phase = [&](std::string phase) {
    const auto now = EngineAbiSteadyClock::now();
    phase_micros.push_back({std::move(phase), EngineAbiElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto decoded =
      scratchbird::engine::DecodeSblrEnvelopeBytes(params->envelope_bytes, params->envelope_size_bytes);
  mark_phase("decode_sblr_envelope_bytes");
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok) {
    WriteEngineAbiPhaseTrace("dispatch_sblr",
                             "decode_rejected",
                             static_cast<std::size_t>(params->envelope_size_bytes),
                             phase_micros);
    const std::string code(decoded.diagnostic_code);
    const std::string key(decoded.message_key);
    if (decoded.status == scratchbird::engine::SblrCodecStatus::version_unsupported) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4003, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::reference_meta_forbidden) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4004, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::descriptor_invalid) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4005, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::opcode_unknown) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4006, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::checksum_invalid) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4007, code, key);
    }
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4001, code, key);
  }
  if (looks_like_sblr_operation_envelope(decoded.envelope)) {
    const auto status = dispatch_operation_envelope(
        session, *context, decoded.envelope, *params, nullptr, out_result);
    mark_phase("dispatch_operation_envelope");
    WriteEngineAbiPhaseTrace("dispatch_sblr",
                             "operation_envelope",
                             static_cast<std::size_t>(params->envelope_size_bytes),
                             phase_micros);
    return status;
  }
  const auto* row =
      scratchbird::engine::FindSblrPriorityDRegistryRow(decoded.envelope.family, decoded.envelope.opcode);
  if (row == nullptr) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4006, "SBLR.OPCODE.UNKNOWN",
                       "sblr.opcode.unknown");
  }
  if (row->behavior_status == scratchbird::engine::SblrBehaviorStatus::noncluster_fail_closed ||
      row->behavior_status == scratchbird::engine::SblrBehaviorStatus::capability_fail_closed ||
      row->behavior_status == scratchbird::engine::SblrBehaviorStatus::edition_fail_closed) {
    return fail_result(SB_ENGINE_STATUS_CAPABILITY_DISABLED, out_result, 4008, std::string(row->diagnostic_code),
                       "sblr.capability.forbidden", std::string(row->family_name));
  }
  if (row->behavior_status == scratchbird::engine::SblrBehaviorStatus::implemented) {
    auto* result = make_result(SB_ENGINE_RESULT_COMMAND_COMPLETION, std::string(row->family_name));
    result->payload = "accepted";
    finalize_diagnostics(result);
    *out_result = result;
    return SB_ENGINE_STATUS_OK;
  }
  return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4002, "SBLR.EXECUTION.ADMISSION_ONLY",
                     "sblr.execution.admission_only", std::string(row->family_name));
}

sb_engine_status_t sb_engine_result_release(sb_engine_result_t result) {
  if (result == nullptr) {
    return SB_ENGINE_STATUS_OK;
  }
  if (result->magic != kResultMagic) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  {
    std::lock_guard<std::mutex> guard(result->mutex);
    if (result->released) {
      return SB_ENGINE_STATUS_ALREADY_RELEASED;
    }
    result->released = true;
    result->magic = 0;
  }
  delete result;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_class(sb_engine_result_t result, sb_engine_result_class_t* out_class) {
  if (out_class == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  *out_class = result->result_class;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_completion(sb_engine_result_t result,
                                               sb_engine_command_completion_view_v1_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  *out_view = {};
  out_view->struct_size = sizeof(*out_view);
  out_view->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  set_view(out_view->operation_id, result->operation_id);
  out_view->affected_rows = result->affected_rows;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_summary(sb_engine_result_t result,
                                            sb_engine_execution_summary_view_v1_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  *out_view = {};
  out_view->struct_size = sizeof(*out_view);
  out_view->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  out_view->rows_produced = result->rows_produced;
  out_view->diagnostics_count = static_cast<std::uint64_t>(result->diagnostics.size());
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_diagnostics(sb_engine_result_t result,
                                                sb_engine_diagnostic_set_view_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  finalize_diagnostics(result);
  *out_view = {};
  out_view->struct_size = sizeof(*out_view);
  out_view->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  out_view->diagnostics = result->diagnostic_views.empty() ? nullptr : result->diagnostic_views.data();
  out_view->diagnostic_count = static_cast<std::uint64_t>(result->diagnostic_views.size());
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_payload(sb_engine_result_t result, sb_engine_string_view_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  set_view(*out_view, result->payload);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_next_batch(sb_engine_result_t result,
                                               const sb_engine_batch_request_v1_t* request,
                                               sb_engine_row_batch_view_v1_t* out_batch) {
  if (out_batch == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  if (request != nullptr) {
    if (request->struct_size < sizeof(sb_engine_batch_request_v1_t) ||
        request->abi_version != SB_ENGINE_ABI_VERSION_PACKED) {
      return SB_ENGINE_STATUS_INVALID_ARGUMENT;
    }
  }
  *out_batch = {};
  out_batch->struct_size = sizeof(*out_batch);
  out_batch->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::lock_guard<std::mutex> guard(result->mutex);
  const std::uint64_t total_rows = static_cast<std::uint64_t>(result->row_values.size());
  const std::uint64_t remaining =
      total_rows > result->next_row_index ? total_rows - result->next_row_index : 0;
  if (remaining == 0 || result->result_class != SB_ENGINE_RESULT_ROW_BATCH) {
    result->payload.clear();
    out_batch->end_of_stream = 1;
    return SB_ENGINE_STATUS_OK;
  }
  const std::uint64_t requested_rows =
      request != nullptr && request->max_rows != 0 ? request->max_rows : remaining;
  const std::uint64_t row_count = std::min(requested_rows, remaining);
  const std::uint64_t first_row = result->next_row_index;
  result->payload = api_result_payload(result->operation_id,
                                       result->result_kind,
                                       result->row_values,
                                       result->row_metadata_values,
                                       result->evidence_values,
                                       first_row,
                                       row_count);
  result->next_row_index += row_count;
  out_batch->row_count = row_count;
  out_batch->end_of_stream = result->next_row_index >= total_rows ? 1 : 0;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_describe_capabilities(sb_engine_handle_t engine,
                                                   const sb_engine_capability_request_v1_t* request,
                                                   sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (request != nullptr) {
    auto status = check_struct(request->struct_size, request->abi_version, sizeof(sb_engine_capability_request_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
  }
  auto* result = make_result(SB_ENGINE_RESULT_CAPABILITY_REPORT, "engine.describe_capabilities");
  result->payload = behavior_payload();
  finalize_diagnostics(result);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_metric_root(sb_engine_handle_t engine,
                                         const sb_engine_metric_request_v1_t* request,
                                         sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (request != nullptr) {
    auto status = check_struct(request->struct_size, request->abi_version, sizeof(sb_engine_metric_request_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
    if (!valid_string_span(request->root_path_utf8, request->root_path_size)) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 5001, "ENGINE.METRIC.ROOT_INVALID",
                         "engine.metric.root_invalid");
    }
    const std::string_view root_path(request->root_path_utf8,
                                     static_cast<std::size_t>(request->root_path_size));
    if (root_path != "sys.metrics.engine") {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 5001, "ENGINE.METRIC.ROOT_INVALID",
                         "engine.metric.root_invalid");
    }
  }
  auto* result = make_result(SB_ENGINE_RESULT_METRIC_ROOT, "engine.metric_root");
  result->payload = "sys.metrics.engine.abi;sys.metrics.engine.dispatch;sys.metrics.sblr.envelope";
  finalize_diagnostics(result);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
}

}  // extern "C"
