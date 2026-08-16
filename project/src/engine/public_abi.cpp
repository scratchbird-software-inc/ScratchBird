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
#include "core/agents/resource_governance_admission.hpp"
#include "datatype_catalog_manifest.hpp"
#include "executor_foundation.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "database_format.hpp"
#include "hash_digest.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "optimizer/model_family_coordinator.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "sblr_literal_runtime.hpp"
#include "sblr_parameter_runtime.hpp"
#include "sblr_parameter_set_registry.hpp"
#include "sblr_prepared_coordination_registry.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_opcode_stream.hpp"
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
  scratchbird::engine::sblr::QueryExecuteResultHandleV1
      query_execute_result_handle;
  bool query_execute_result_handle_validated = false;
  bool admitted_query_row_stream_renderer = false;
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
  std::unique_ptr<scratchbird::core::agents::
                      ResourceGovernanceReservationLedger>
      package_resource_ledger;
  scratchbird::core::agents::ResourceGovernanceQuotaDescriptor
      package_resource_descriptor;
  bool package_resource_descriptor_initialized = false;
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
  bool literal_prebind_negotiated = false;
  bool literal_binding_finalized = false;
  bool literal_admission_consumed = false;
  std::array<std::uint8_t,32> literal_demand_sha256{};
  std::array<std::uint8_t,32> literal_ordered_profile_sha256{};
  std::vector<std::uint8_t> literal_canonical_sblq;
  std::vector<scratchbird::engine::sblr::SblrLiteralDemandV1> literal_demands;
  std::array<std::uint8_t,32> literal_bound_ast_sha256{};
  std::array<std::uint8_t,32> literal_sbxn_sha256{};
  std::string literal_final_receipt_uuid;
  std::string literal_admission_token_uuid;
  std::array<std::uint8_t,32> literal_admission_token_binding_sha256{};
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      literal_executor_availability_snapshot;
  bool parameter_prebind_negotiated = false;
  bool parameter_binding_finalized = false;
  bool parameter_admission_consumed = false;
  std::array<std::uint8_t, 32> parameter_demand_sha256{};
  std::array<std::uint8_t, 32> parameter_mapping_sha256{};
  std::array<std::uint8_t, 32> parameter_sbpn_sha256{};
  std::vector<scratchbird::engine::sblr::SblrParameterDemandV1>
      parameter_demands;
  scratchbird::engine::internal_api::SblrParameterSetSnapshot
      parameter_set_snapshot;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      parameter_executor_availability_snapshot;
  std::string parameter_final_receipt_uuid;
  std::string parameter_admission_token_uuid;
  std::array<std::uint8_t, 32> parameter_admission_binding_sha256{};
};

struct StatementPackageAdmissionReservationOpaque {
  std::uint64_t receipt_id = 0;
  sb_engine_session_t session = nullptr;
  StatementSblrPayloadKind payload_kind = StatementSblrPayloadKind::kInvalid;
  std::uint64_t payload_size = 0;
  std::uint32_t record_count = 0;
  std::uint64_t resource_policy_generation = 0;
  std::array<std::uint8_t, 32> payload_sha256{};
  std::string ledger_token_id;
};

struct StatementParameterCoordinationOpaque {
  std::uint64_t private_handle = 0;
  sb_engine_session_t session = nullptr;
  std::string database_uuid;
  std::string session_uuid;
  std::string transaction_uuid;
  StatementParameterExecutionMode mode = StatementParameterExecutionMode::kDirect;
  std::string public_coordination_uuid;
  std::string operation_uuid;
  std::uint64_t generation = 0;
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation = 0;
  bool context_acquired = false;
  bool sealed = false;
  bool terminal = false;
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
std::map<std::uint64_t, scratchbird::server_engine_bridge::
    StatementPackageAdmissionReservationOpaque>
    g_package_admission_reservations;
std::atomic<std::uint64_t> g_next_package_admission_reservation_id{1};
std::map<std::uint64_t, scratchbird::server_engine_bridge::
    StatementParameterCoordinationOpaque> g_parameter_coordinations;
std::atomic<std::uint64_t> g_next_parameter_coordination_handle{1};

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
    for (auto it = g_package_admission_reservations.begin();
         it != g_package_admission_reservations.end();) {
      if (it->second.session != session) {
        ++it;
        continue;
      }
      if (session->package_resource_ledger != nullptr &&
          !it->second.ledger_token_id.empty()) {
        (void)session->package_resource_ledger->Release(
            it->second.ledger_token_id,
            scratchbird::core::agents::
                ResourceGovernanceReservationReleaseReason::kShutdown);
      }
      it = g_package_admission_reservations.erase(it);
    }
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
  session->package_resource_ledger = std::make_unique<
      scratchbird::core::agents::ResourceGovernanceReservationLedger>(
      "engine.session.package:" + std::to_string(session->session_id));
  auto& package_policy = session->package_resource_descriptor;
  package_policy.descriptor_id =
      "engine.runtime.sblr_package.query_memory_arena.v1";
  package_policy.family = scratchbird::core::agents::
      ResourceGovernanceFamily::kQueryMemoryArena;
  package_policy.source = scratchbird::core::agents::
      ResourceGovernanceDescriptorSource::kRuntimePolicy;
  package_policy.source_path_or_label =
      "engine.builtin.runtime_policy.sblr_package.v1";
  package_policy.descriptor_generation = 0;
  package_policy.expected_generation = 0;
  package_policy.over_limit_action = scratchbird::core::agents::
      ResourceGovernanceAction::kFailClosed;
  package_policy.benchmark_clean = true;
  package_policy.runtime_dependency_present = true;
  constexpr std::int64_t kPackageResourceDimensionLimit = 1'000'000;
  package_policy.limits.memory_bytes = 256 * 1024 * 1024;
  package_policy.limits.device_memory_bytes = 1;
  package_policy.limits.pinned_memory_bytes = 1;
  package_policy.limits.io_bytes = 64 * 1024 * 1024;
  package_policy.limits.io_ops = kPackageResourceDimensionLimit;
  package_policy.limits.worker_threads = 64;
  package_policy.limits.backlog_items = 4096;
  package_policy.limits.candidate_rows = kPackageResourceDimensionLimit;
  package_policy.limits.cache_entries = kPackageResourceDimensionLimit;
  package_policy.limits.batch_rows = kPackageResourceDimensionLimit;
  package_policy.limits.fragments = 262144;
  package_policy.limits.lanes = 64;
  package_policy.limits.time_budget_microseconds = 300'000'000;
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

sb_engine_status_t BeginStatementParameterExecutionCoordinationV1(
    const StatementParameterCoordinationBeginRequestV1* request,
    StatementParameterCoordinationViewV1* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (request == nullptr || out_view == nullptr ||
      request->engine_session == nullptr || request->engine_context == nullptr ||
      request->mode != StatementParameterExecutionMode::kPrepared ||
      request->operation_uuid.empty() ||
      !request->public_prepared_uuid.empty() ||
      !request->public_dynamic_package_uuid.empty()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4077,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_coordination.begin_invalid");
  }
  const auto operation = scratchbird::core::uuid::ParseUuid(
      request->operation_uuid);
  const auto& context = *request->engine_context;
  if (!operation.ok() || context.database_uuid.canonical.empty() ||
      context.session_uuid.canonical.empty() ||
      context.transaction_uuid.canonical.empty()) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4077,
                       "SECURITY.ACCESS_DENIED",
                       "sblr.parameter_coordination.ownership_invalid");
  }
  auto registry_context = context;
  registry_context.statement_metadata_snapshot_engine_owned = true;
  registry_context.trace_tags.push_back("private_prepared_coordination");
  const auto issued = scratchbird::engine::internal_api::
      BeginSblrPreparedCoordination(registry_context, request->operation_uuid);
  if (!issued.ok) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4077,
                       issued.diagnostic.code, issued.diagnostic.message_key,
                       issued.diagnostic.detail);
  }
  std::lock_guard<std::mutex> guard(g_statement_context_receipt_registry_mutex);
  const auto handle = issued.snapshot.private_handle;
  if (handle == 0 || g_parameter_coordinations.contains(handle)) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4077,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_coordination.handle_unavailable");
  }
  StatementParameterCoordinationOpaque coordination;
  coordination.private_handle = handle;
  coordination.session = request->engine_session;
  coordination.database_uuid = context.database_uuid.canonical;
  coordination.session_uuid = context.session_uuid.canonical;
  coordination.transaction_uuid = context.transaction_uuid.canonical;
  coordination.mode = request->mode;
  coordination.public_coordination_uuid = issued.snapshot.coordination_uuid;
  coordination.operation_uuid = request->operation_uuid;
  coordination.generation = issued.snapshot.coordinator_generation;
  coordination.prepared_statement_uuid =
      issued.snapshot.provisional_prepared_uuid;
  coordination.prepared_generation =
      issued.snapshot.provisional_prepared_generation;
  if (!g_parameter_coordinations.emplace(handle, coordination).second) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4077,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_coordination.handle_collision");
  }
  out_view->private_handle = handle;
  out_view->public_coordination_uuid = coordination.public_coordination_uuid;
  out_view->operation_uuid = request->operation_uuid;
  out_view->coordinator_generation = coordination.generation;
  out_view->prepared_statement_uuid = coordination.prepared_statement_uuid;
  out_view->prepared_generation = coordination.prepared_generation;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t SealPreparedStatementParameterTemplateV1(
    std::uint64_t private_handle,
    const std::vector<std::uint8_t>& canonical_sbpt,
    StatementParameterCoordinationViewV1* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (private_handle == 0 || out_view == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4078,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_coordination.seal_invalid");
  }
  const auto decoded = scratchbird::engine::sblr::
      DecodeSblrPreparedParameterTemplateV1(canonical_sbpt.data(),
                                            canonical_sbpt.size());
  if (!decoded.ok) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4078,
                       decoded.diagnostic_id,
                       "sblr.parameter_template.decode_invalid",
                       decoded.detail);
  }
  const auto uuid_text=[](const auto& bytes){
    scratchbird::core::platform::Uuid u{};
    std::copy(bytes.begin(),bytes.end(),u.bytes.begin());
    return scratchbird::core::uuid::UuidToString(u);
  };
  StatementParameterCoordinationOpaque coordination;
  {
    std::lock_guard<std::mutex> guard(g_statement_context_receipt_registry_mutex);
    const auto found = g_parameter_coordinations.find(private_handle);
    if (found == g_parameter_coordinations.end()) {
      return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4078,
                         "SECURITY.ACCESS_DENIED",
                         "sblr.parameter_coordination.hidden");
    }
    coordination = found->second;
  }
  if (coordination.public_coordination_uuid !=
          uuid_text(decoded.value.public_coordination_uuid) ||
      coordination.operation_uuid != uuid_text(decoded.value.operation_uuid) ||
      coordination.prepared_statement_uuid !=
          uuid_text(decoded.value.provisional_prepared_uuid) ||
      coordination.prepared_generation !=
          decoded.value.provisional_prepared_generation ||
      coordination.terminal || coordination.sealed ||
      !coordination.context_acquired) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4078,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_coordination.seal_stale");
  }
  const auto& schema=decoded.value.canonical_schema4015;
  if(schema.size()<98)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.schema4015_truncated");
  std::size_t offset=0;offset+=16;
  if(!std::all_of(schema.begin()+16,schema.begin()+32,[](auto b){return b==0;})||schema[32]!=0||schema[33]!=1)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.schema4015_selector_invalid");
  offset=74;
  const auto read_blob=[&](std::vector<std::uint8_t>* out){
    if(offset>schema.size()||schema.size()-offset<8)return false;
    const auto n=scratchbird::engine::SblrReadU64(schema.data()+offset);offset+=8;
    if(n>schema.size()-offset)return false;
    out->assign(schema.begin()+static_cast<std::ptrdiff_t>(offset),
                schema.begin()+static_cast<std::ptrdiff_t>(offset+n));offset+=n;return true;};
  std::vector<std::uint8_t> container_bytes,execution_bytes,data_bytes;
  if(!read_blob(&container_bytes)||!read_blob(&execution_bytes)||!read_blob(&data_bytes)||
     offset!=schema.size()||container_bytes.empty()||execution_bytes.empty()||!data_bytes.empty())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.schema4015_extent_invalid");
  const auto container=scratchbird::engine::DecodeSblrContainerBytes(container_bytes.data(),container_bytes.size());
  const auto ingress=scratchbird::engine::DecodeSblrExecutionEnvelopeV1Bytes(execution_bytes.data(),execution_bytes.size());
  scratchbird::engine::SblrExecutionEnvelopeSemanticView ingress_view;
  if(container.status!=scratchbird::engine::SblrCodecStatus::ok||
     ingress.status!=scratchbird::engine::SblrCodecStatus::ok||
     !scratchbird::engine::SblrValidateExecutionEnvelopeFields(ingress.envelope,&ingress_view)||
     container.container.operation_payload.empty())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.canonical_envelope_invalid");
  const std::string_view operation_bytes(
      reinterpret_cast<const char*>(container.container.operation_payload.data()),
      container.container.operation_payload.size());
  auto stream=scratchbird::engine::sblr::DecodeSblrOpcodeStream(operation_bytes);
  if(!stream.ok||stream.stream.operations.empty())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbos_invalid");
  const std::vector<std::uint8_t>* sbpn=nullptr;
  for(const auto& operation:stream.stream.operations)for(const auto& operand:operation.operands){
    if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::parameter_node_table){
      if(sbpn!=nullptr)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
        "SBLR.OPERAND_INVALID","sblr.parameter_template.multiple_sbpn");sbpn=&operand.value_body;}
  }
  if(sbpn==nullptr)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbpn_missing");
  static constexpr std::string_view sbpn_domain="ScratchBird.SblrParameterNodeTable.V1";
  std::vector<std::uint8_t> sbpn_material(sbpn_domain.begin(),sbpn_domain.end());
  sbpn_material.insert(sbpn_material.end(),sbpn->begin(),sbpn->end());
  const auto sbpn_sha=scratchbird::core::hash::ComputeSha256Digest(sbpn_material);
  if(!sbpn_sha.ok()||sbpn_sha.digest!=decoded.value.sbpn_sha256)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbpn_hash_invalid");
  scratchbird::engine::sblr::SblrParameterAdmissionV1 admission;std::string detail;
  if(!scratchbird::engine::sblr::DecodeSblrParameterAdmissionV1(
       decoded.value.canonical_sbpa.data(),decoded.value.canonical_sbpa.size(),&admission,&detail)||
     uuid_text(admission.parameter_set_descriptor_uuid)!=uuid_text(decoded.value.parameter_set_descriptor_uuid)||
     admission.descriptor_generation!=decoded.value.descriptor_generation||
     uuid_text(admission.execution_uuid)!=coordination.operation_uuid||
     uuid_text(admission.prepared_uuid)!=coordination.prepared_statement_uuid||
     admission.prepared_generation!=coordination.prepared_generation)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbpa_invalid",detail);
  std::unique_lock<std::mutex> live_registry_guard(
      g_statement_context_receipt_registry_mutex);
  StatementContextReceiptOpaque* receipt=nullptr;
  for(auto& [id,candidate]:g_live_statement_context_receipts){
    if(candidate->parameter_final_receipt_uuid==uuid_text(admission.final_receipt_uuid)&&
       candidate->parameter_admission_token_uuid==uuid_text(admission.admission_token_uuid)){
      if(receipt!=nullptr)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
        "SBLR.PARAMETER.STALE","sblr.parameter_template.receipt_ambiguous");receipt=candidate.get();}}
  if(receipt==nullptr)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      "SBLR.PARAMETER.STALE","sblr.parameter_template.receipt_stale");
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  auto parameter_identity=scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity{};
  parameter_identity.executor_id=scratchbird::engine::internal_api::kSblrParameterExecutorId;
  parameter_identity.opcode_code=scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
  parameter_identity.opcode_version=scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
  parameter_identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
  parameter_identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
  parameter_identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current_executor;
  const auto executor=scratchbird::engine::internal_api::RevalidateSblrExecutorAvailability(
      receipt->engine_context,parameter_identity,receipt->parameter_executor_availability_snapshot,&current_executor);
  if(executor.error||decoded.value.executor_availability_generation!=
       receipt->view.parameter_executor_availability_generation||
     current_executor.generation!=decoded.value.executor_availability_generation||
     receipt->released||!receipt->parameter_binding_finalized||receipt->parameter_admission_consumed||
     receipt->parameter_sbpn_sha256!=decoded.value.sbpn_sha256||
     receipt->parameter_set_snapshot.catalog_generation!=decoded.value.catalog_generation||
     receipt->parameter_set_snapshot.security_epoch!=decoded.value.security_epoch||
     receipt->parameter_set_snapshot.resource_epoch!=decoded.value.resource_epoch||
     receipt->view.statement_snapshot_uuid!=uuid_text(decoded.value.mga_snapshot_uuid))
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      executor.error?executor.code:"SBLR.PARAMETER.STALE",
      "sblr.parameter_template.live_binding_stale",executor.detail);
  const auto evidence_hex=scratchbird::core::hash::HexLower(
      decoded.value.prepared_template_binding_sha256);
  auto registry_context=receipt->engine_context;
  registry_context.statement_metadata_snapshot_engine_owned=true;
  registry_context.trace_tags.push_back("private_prepared_coordination");
  const auto expected_coordination_generation=coordination.generation;
  const auto sealed=scratchbird::engine::internal_api::SealSblrPreparedCoordination(
      registry_context,coordination.public_coordination_uuid,coordination.operation_uuid,
      coordination.generation,coordination.prepared_statement_uuid,
      coordination.prepared_generation,"sha256:"+evidence_hex);
  if(!sealed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      sealed.diagnostic.code,sealed.diagnostic.message_key,sealed.diagnostic.detail);
  receipt->parameter_admission_consumed=true;
  coordination.sealed=true;coordination.generation=sealed.snapshot.coordinator_generation;
  const auto found=g_parameter_coordinations.find(private_handle);
  if(found==g_parameter_coordinations.end()||found->second.sealed||
     found->second.generation!=expected_coordination_generation)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      "SBLR.PARAMETER.STALE","sblr.parameter_coordination.publish_race");
  found->second=coordination;
  out_view->private_handle = private_handle;
  out_view->public_coordination_uuid = coordination.public_coordination_uuid;
  out_view->operation_uuid = coordination.operation_uuid;
  out_view->coordinator_generation = coordination.generation;
  out_view->prepared_statement_uuid = coordination.prepared_statement_uuid;
  out_view->prepared_generation = coordination.prepared_generation;
  return SB_ENGINE_STATUS_OK;
}

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
  std::optional<StatementParameterCoordinationOpaque> parameter_coordination;
  const auto& selector = request->parameter_execution_selector;
  const bool direct_selector =
      selector.version == 1 && selector.reserved == 0 &&
      selector.mode == StatementParameterExecutionMode::kDirect &&
      selector.prepared_binding_handle == 0 &&
      selector.batch_execution_handle == 0 &&
      selector.dynamic_package_handle == 0;
  if (!direct_selector) {
    if (selector.version != 1 || selector.reserved != 0 ||
        selector.mode != StatementParameterExecutionMode::kPrepared ||
        selector.prepared_binding_handle == 0 ||
        selector.batch_execution_handle != 0 ||
        selector.dynamic_package_handle != 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4039,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_execution_selector.invalid");
    }
    StatementParameterCoordinationOpaque coordination;
    {
      std::lock_guard<std::mutex> guard(
          g_statement_context_receipt_registry_mutex);
      const auto found = g_parameter_coordinations.find(
          selector.prepared_binding_handle);
      if (found == g_parameter_coordinations.end() ||
          found->second.session != session ||
          found->second.database_uuid != engine_context.database_uuid.canonical ||
          found->second.session_uuid != engine_context.session_uuid.canonical) {
        return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4039,
                           "SECURITY.ACCESS_DENIED",
                           "sblr.parameter_execution_selector.hidden");
      }
      coordination = found->second;
    }
    if (coordination.transaction_uuid != request->exact_transaction_uuid ||
        coordination.terminal || coordination.sealed ||
        coordination.context_acquired) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4039,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_execution_selector.stale");
    }
    const auto expected_coordination_generation = coordination.generation;
    auto registry_context = engine_context;
    registry_context.statement_metadata_snapshot_engine_owned = true;
    registry_context.trace_tags.push_back("private_prepared_coordination");
    const auto acquired = scratchbird::engine::internal_api::
        AcquireSblrPreparedCoordination(
            registry_context, coordination.public_coordination_uuid,
            coordination.operation_uuid, coordination.generation);
    if (!acquired.ok) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4039,
                         acquired.diagnostic.code,
                         acquired.diagnostic.message_key,
                         acquired.diagnostic.detail);
    }
    coordination.context_acquired = true;
    coordination.generation = acquired.snapshot.coordinator_generation;
    {
      std::lock_guard<std::mutex> guard(
          g_statement_context_receipt_registry_mutex);
      const auto found = g_parameter_coordinations.find(
          selector.prepared_binding_handle);
      if (found == g_parameter_coordinations.end() ||
          found->second.generation != expected_coordination_generation ||
          found->second.context_acquired) {
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4039,
                           "SBLR.PARAMETER.STALE",
                           "sblr.parameter_execution_selector.publish_race");
      }
      found->second = coordination;
    }
    parameter_coordination = coordination;
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

  // QOW-SOURCE-RCP-078-STATEMENT-SEARCH-DESCRIPTORS-V9: V9 extends the
  // exact V8 prefix with four engine-issued result-slot identities. Core
  // datatype catalog rows own the UUID/UINT64 type identities; neither the
  // parser nor a model-family provider may manufacture or relabel them.
  const auto uuid_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uuid"; });
  const auto uint64_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uint64"; });
  const auto uuid_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uuid"; });
  const auto uint64_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uint64"; });
  if (uuid_count != 1 || uint64_count != 1 ||
      uuid_row == core_manifest.manifest.descriptor_rows.end() ||
      uint64_row == core_manifest.manifest.descriptor_rows.end() ||
      !uuid_row->descriptor_uuid.valid() ||
      !uint64_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_v9_descriptor_type_cohort");
  }
  const auto uuid_type_uuid = scratchbird::core::uuid::UuidToString(
      uuid_row->descriptor_uuid.value);
  const auto uint64_type_uuid = scratchbird::core::uuid::UuidToString(
      uint64_row->descriptor_uuid.value);
  std::array<std::string, 4> search_descriptor_uuids;
  if (uuid_type_uuid.empty() || uint64_type_uuid.empty() ||
      uuid_type_uuid == uint64_type_uuid ||
      !issue_identity(&search_descriptor_uuids[0]) ||
      !issue_identity(&search_descriptor_uuids[1]) ||
      !issue_identity(&search_descriptor_uuids[2]) ||
      !issue_identity(&search_descriptor_uuids[3])) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_search_descriptor_identity");
  }
  for (std::uint16_t slot = 0; slot < 2; ++slot) {
    StatementDescriptorProfile profile;
    profile.profile_kind = StatementDescriptorProfileKind::kUuidNonNull;
    profile.slot = slot;
    profile.descriptor_uuid = std::move(search_descriptor_uuids[slot]);
    profile.type_uuid = uuid_type_uuid;
    view.descriptor_profiles.push_back(std::move(profile));
  }
  for (std::uint16_t slot = 0; slot < 2; ++slot) {
    StatementDescriptorProfile profile;
    profile.profile_kind = StatementDescriptorProfileKind::kUint64NonNull;
    profile.slot = slot;
    profile.descriptor_uuid = std::move(search_descriptor_uuids[slot + 2]);
    profile.type_uuid = uint64_type_uuid;
    view.descriptor_profiles.push_back(std::move(profile));
  }

  // QOW-SOURCE-RCP-079-STATEMENT-MULTILEG-DESCRIPTORS-V10: append ten
  // exact 32-slot pools to the immutable V9 prefix. Type identity remains
  // core-catalog-owned; every result descriptor identity is issued here by
  // the engine before any model-family provider or data access can begin.
  const auto boolean_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "boolean"; });
  const auto geometry_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "geometry"; });
  const auto boolean_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "boolean"; });
  const auto geometry_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "geometry"; });
  if (boolean_count != 1 || geometry_count != 1 ||
      boolean_row == core_manifest.manifest.descriptor_rows.end() ||
      geometry_row == core_manifest.manifest.descriptor_rows.end() ||
      !boolean_row->descriptor_uuid.valid() ||
      !geometry_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_v10_descriptor_type_cohort");
  }
  const auto boolean_catalog_type_uuid = scratchbird::core::uuid::UuidToString(
      boolean_row->descriptor_uuid.value);
  const auto geometry_type_uuid = scratchbird::core::uuid::UuidToString(
      geometry_row->descriptor_uuid.value);
  const std::array<std::string, 5> multileg_type_uuids = {
      uuid_type_uuid, uint64_type_uuid, real64_type_uuid,
      boolean_catalog_type_uuid, geometry_type_uuid};
  if (std::ranges::any_of(multileg_type_uuids,
                         [](const auto& value) { return value.empty(); }) ||
      std::unordered_set<std::string>(multileg_type_uuids.begin(),
                                      multileg_type_uuids.end()).size() != 5) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_v10_descriptor_type_identity");
  }
  struct MultilegProfilePair {
    StatementDescriptorProfileKind non_null_kind;
    StatementDescriptorProfileKind nullable_kind;
    const std::string* type_uuid;
  };
  const std::array<MultilegProfilePair, 5> multileg_profile_pairs = {{
      {StatementDescriptorProfileKind::kMultilegUuidNonNull,
       StatementDescriptorProfileKind::kMultilegUuidNullable,
       &multileg_type_uuids[0]},
      {StatementDescriptorProfileKind::kMultilegUint64NonNull,
       StatementDescriptorProfileKind::kMultilegUint64Nullable,
       &multileg_type_uuids[1]},
      {StatementDescriptorProfileKind::kMultilegReal64NonNull,
       StatementDescriptorProfileKind::kMultilegReal64Nullable,
       &multileg_type_uuids[2]},
      {StatementDescriptorProfileKind::kMultilegBooleanNonNull,
       StatementDescriptorProfileKind::kMultilegBooleanNullable,
       &multileg_type_uuids[3]},
      {StatementDescriptorProfileKind::kMultilegGeometryNonNull,
       StatementDescriptorProfileKind::kMultilegGeometryNullable,
       &multileg_type_uuids[4]},
  }};
  for (const auto& pair : multileg_profile_pairs) {
    for (const auto kind : {pair.non_null_kind, pair.nullable_kind}) {
      const bool nullable = kind == pair.nullable_kind;
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        StatementDescriptorProfile profile;
        profile.profile_kind = kind;
        profile.slot = slot;
        profile.type_uuid = *pair.type_uuid;
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
              "statement_multileg_descriptor_identity");
        }
        view.descriptor_profiles.push_back(std::move(profile));
      }
    }
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
  // DATATYPE-TYPE-CODEC-IDENTITY-REGISTRY-V1 is the sole literal descriptor
  // identity snapshot. Keep it distinct from the general statement catalog
  // epoch and publish it only as the preliminary V11 bootstrap authority.
  view.literal_catalog_snapshot_uuid =
      "019d0000-0000-7000-8000-00000000d701";
  view.literal_catalog_generation = 1;
  view.literal_registry_generation = 1;
  if (parameter_coordination.has_value()) {
    view.parameter_prepared_statement_uuid =
        parameter_coordination->prepared_statement_uuid;
    view.parameter_prepared_generation =
        parameter_coordination->prepared_generation;
  }
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
      parameter_executor_identity;
  parameter_executor_identity.executor_id =
      scratchbird::engine::internal_api::kSblrParameterExecutorId;
  parameter_executor_identity.opcode_code =
      scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
  parameter_executor_identity.opcode_version =
      scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
  parameter_executor_identity.operand_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
  parameter_executor_identity.result_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
  parameter_executor_identity.result_descriptor_version =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
  const auto parameter_executor = scratchbird::engine::internal_api::
      LoadSblrExecutorAvailabilitySnapshot(engine_context,
                                           parameter_executor_identity);
  if (!parameter_executor.ok || !parameter_executor.snapshot.installed ||
      parameter_executor.snapshot.generation == 0) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4046,
        parameter_executor.diagnostic.code.empty()
            ? "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
            : parameter_executor.diagnostic.code,
        "sblr.parameter_preliminary.executor_evidence_missing",
        parameter_executor.diagnostic.detail);
  }
  view.parameter_executor_availability_generation =
      parameter_executor.snapshot.generation;
  view.security_epoch = engine_context.security_epoch;
  view.resource_epoch = engine_context.resource_epoch;
  {
    static constexpr std::string_view kDomain =
        "ScratchBird.SblrParameterPreliminaryExecutionMode.V1";
    std::vector<std::uint8_t> binding(kDomain.begin(), kDomain.end());
    scratchbird::engine::SblrAppendU16(binding, 3);
    scratchbird::engine::SblrAppendU16(binding, 0);
    const auto append_uuid = [&](std::string_view text) {
      if (text.empty()) {
        binding.insert(binding.end(), 16, 0);
        return true;
      }
      const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
      if (!parsed.ok()) return false;
      binding.insert(binding.end(), parsed.value.bytes.begin(),
                     parsed.value.bytes.end());
      return true;
    };
    if (!append_uuid(view.receipt_uuid) ||
        !append_uuid(view.literal_catalog_snapshot_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_preliminary.identity_invalid");
    }
    scratchbird::engine::SblrAppendU64(binding,
                                       view.literal_catalog_generation);
    scratchbird::engine::SblrAppendU64(binding, view.security_epoch);
    scratchbird::engine::SblrAppendU64(binding, view.resource_epoch);
    if (!append_uuid(view.statement_snapshot_uuid) ||
        !append_uuid(view.parameter_prepared_statement_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_preliminary.identity_invalid");
    }
    scratchbird::engine::SblrAppendU64(
        binding, view.parameter_prepared_generation);
    if (!append_uuid(view.parameter_batch_uuid)) return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
        "SBLR.OPERAND_INVALID", "sblr.parameter_preliminary.identity_invalid");
    scratchbird::engine::SblrAppendU64(binding,
                                       view.parameter_batch_generation);
    if (!append_uuid(view.parameter_dynamic_package_uuid)) return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
        "SBLR.OPERAND_INVALID", "sblr.parameter_preliminary.identity_invalid");
    scratchbird::engine::SblrAppendU64(
        binding, view.parameter_dynamic_generation);
    scratchbird::engine::SblrAppendU64(
        binding, view.parameter_executor_availability_generation);
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(binding);
    if (!digest.ok()) return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
        "SBLR.OPERAND_INVALID", "sblr.parameter_preliminary.binding_failed");
    view.parameter_preliminary_execution_mode_binding_sha256 = digest.digest;
  }
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
  view.cluster_context_active = engine_context.cluster_authority_available;
  view.cluster_transaction_active = engine_context.cluster_transaction_active;
  view.route_fence_present = engine_context.route_fence_present;

  {
    std::lock_guard<std::mutex> session_guard(session->mutex);
    if (!session->package_resource_descriptor_initialized) {
      session->package_resource_descriptor.descriptor_generation =
          view.resource_epoch;
      session->package_resource_descriptor.expected_generation =
          view.resource_epoch;
      session->package_resource_descriptor_initialized = true;
    } else if (session->package_resource_descriptor.descriptor_generation !=
                   view.resource_epoch ||
               session->package_resource_descriptor.expected_generation !=
                   view.resource_epoch) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4047,
                         "RESOURCE.BUDGET_EXCEEDED",
                         "engine.statement_context.resource_epoch_stale");
    }
  }

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
  receipt->parameter_executor_availability_snapshot =
      parameter_executor.snapshot;
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
    for (auto reservation = g_package_admission_reservations.begin();
         reservation != g_package_admission_reservations.end();) {
      if (reservation->second.receipt_id != receipt.opaque_id) {
        ++reservation;
        continue;
      }
      if (reservation->second.session != nullptr &&
          reservation->second.session->package_resource_ledger != nullptr &&
          !reservation->second.ledger_token_id.empty()) {
        (void)reservation->second.session->package_resource_ledger->Release(
            reservation->second.ledger_token_id,
            scratchbird::core::agents::
                ResourceGovernanceReservationReleaseReason::kDisconnect);
      }
      reservation = g_package_admission_reservations.erase(reservation);
    }
    published_snapshot_uuid = live->second->snapshot_vector.snapshot_uuid;
    released = std::move(live->second);
    g_live_statement_context_receipts.erase(live);
  }
  scratchbird::transaction::mga::RevokePublishedSnapshotVector(
      published_snapshot_uuid);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t NegotiateStatementLiteralDescriptorsV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbln,
    std::vector<std::uint8_t>* out_canonical_sblq,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sblq == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4070,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_prebind.request_invalid");
  }
  out_canonical_sblq->clear();
  const auto decoded=scratchbird::engine::sblr::DecodeSblrLiteralPrebindRequestV1(
      canonical_sbln.data(),canonical_sbln.size());
  if(!decoded.ok){
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4070,
                       decoded.diagnostic_id,
                       "sblr.literal_prebind.structural_invalid",decoded.detail);
  }
  const auto uuid_bytes=[](const std::string& text,
                           std::array<std::uint8_t,16>* out){
    const auto parsed=scratchbird::core::uuid::ParseUuid(text);
    if(!parsed.ok()||out==nullptr)return false;
    std::copy(parsed.value.bytes.begin(),parsed.value.bytes.end(),out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live=g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if(live==g_live_statement_context_receipts.end()){
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,out_result,4071,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_prebind.receipt_stale");
  }
  auto* receipt=live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t,16> receipt_uuid{},catalog_uuid{},mga_uuid{};
  if(receipt->released||receipt->literal_prebind_negotiated||
     !uuid_bytes(receipt->view.receipt_uuid,&receipt_uuid)||
     !uuid_bytes(receipt->view.literal_catalog_snapshot_uuid,&catalog_uuid)||
     !uuid_bytes(receipt->view.statement_snapshot_uuid,&mga_uuid)||
     decoded.request.preliminary_receipt_uuid!=receipt_uuid||
     decoded.request.catalog_snapshot_uuid!=catalog_uuid||
     decoded.request.catalog_generation!=receipt->view.literal_catalog_generation||
     decoded.request.security_epoch!=receipt->view.security_epoch||
     decoded.request.resource_epoch!=receipt->view.resource_epoch||
     decoded.request.mga_snapshot_uuid!=mga_uuid){
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4071,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_prebind.binding_stale");
  }
  for(const auto& demand:decoded.request.demands){
    if(!scratchbird::engine::sblr::IsAdmittedBigintLiteralDemandV1(demand)){
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4072,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_prebind.demand_unregistered");
    }
  }
  const auto identity=scratchbird::core::datatypes::
      LookupDatatypeTypeCodecIdentityV1(
          receipt->view.literal_catalog_snapshot_uuid,
          receipt->view.literal_catalog_generation,
          receipt->view.literal_registry_generation,
          "019d0000-0000-7000-8000-00000000d711",1);
  if(!identity.ok){
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4072,
                       identity.diagnostic_id,
                       "sblr.literal_prebind.registry_lookup_failed");
  }
  scratchbird::engine::sblr::SblrLiteralPrebindResultV1 response;
  response.preliminary_receipt_uuid=receipt_uuid;
  response.catalog_snapshot_uuid=catalog_uuid;
  response.catalog_generation=receipt->view.literal_catalog_generation;
  response.security_epoch=receipt->view.security_epoch;
  response.resource_epoch=receipt->view.resource_epoch;
  response.mga_snapshot_uuid=mga_uuid;
  response.demand_sha256=decoded.request.demand_sha256;
  if(!decoded.request.demands.empty()){
    std::unordered_set<std::string> identities{receipt->view.receipt_uuid,
        identity.row.descriptor_uuid,identity.row.type_uuid};
    std::string profile_uuid_text;
    if(!generate_distinct_statement_context_uuid(&identities,&profile_uuid_text)){
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                         "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                         "sblr.literal_prebind.profile_identity_unavailable");
    }
    scratchbird::engine::sblr::SblrLiteralStatementDescriptorProfileV1 profile;
    if(!uuid_bytes(profile_uuid_text,&profile.profile_uuid)||
       !uuid_bytes(identity.row.descriptor_uuid,&profile.descriptor_uuid)||
       !uuid_bytes(identity.row.type_uuid,&profile.type_uuid)){
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_prebind.registry_identity_invalid");
    }
    profile.statement_receipt_uuid=receipt_uuid;
    profile.catalog_snapshot_uuid=catalog_uuid;
    profile.catalog_generation=identity.row.catalog_generation;
    profile.descriptor_generation=identity.row.descriptor_generation;
    profile.codec_id=identity.row.codec_id;profile.codec_version=identity.row.codec_version;
    profile.codec_generation=identity.row.codec_generation;profile.nullable=false;
    profile.profile_binding_sha256=
        scratchbird::engine::sblr::ComputeSblrLiteralDescriptorProfileBindingV1(
            profile,receipt->view.security_epoch,receipt->view.resource_epoch);
    const auto sblp=scratchbird::engine::sblr::EncodeSblrLiteralDescriptorProfileV1(profile);
    if(sblp.empty()){
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_prebind.profile_encoding_failed");
    }
    for(const auto& demand:decoded.request.demands){response.mappings.push_back({demand.occurrence_id,sblp});}
  }
  response.ordered_profile_sha256=
      scratchbird::engine::sblr::ComputeSblrLiteralOrderedProfilesSha256V1(response.mappings);
  auto encoded=scratchbird::engine::sblr::EncodeSblrLiteralPrebindResultV1(response);
  if(encoded.empty()){
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_prebind.result_encoding_failed");
  }
  receipt->literal_prebind_negotiated=true;
  receipt->literal_demand_sha256=response.demand_sha256;
  receipt->literal_ordered_profile_sha256=response.ordered_profile_sha256;
  receipt->literal_canonical_sblq=encoded;
  receipt->literal_demands=decoded.request.demands;
  *out_canonical_sblq=std::move(encoded);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t FinalizeStatementLiteralBindingV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sblf,
    std::vector<std::uint8_t>* out_canonical_sbla,
    sb_engine_result_t* out_result){
  clear_result(out_result);if(!receipt_handle||out_canonical_sbla==nullptr)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,"SBLR.OPERAND_INVALID","sblr.literal_finalize.request_invalid");
  scratchbird::engine::sblr::SblrLiteralFinalizeRequestV1 request;
  if(!scratchbird::engine::sblr::DecodeSblrLiteralFinalizeRequestV1(
         canonical_sblf.data(),canonical_sblf.size(),&request))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.structural_or_sbba_invalid");
  scratchbird::engine::sblr::SblrLiteralBoundAstV1 bound;
  if(!scratchbird::engine::sblr::DecodeSblrLiteralBoundAstV1(
         request.canonical_sbba.data(),request.canonical_sbba.size(),&bound))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.sbba_invalid");
  const auto uuid_bytes=[](const std::string& text,
                           std::array<std::uint8_t,16>* out){
    const auto parsed=scratchbird::core::uuid::ParseUuid(text);
    if(!parsed.ok()||out==nullptr)return false;
    std::copy(parsed.value.bytes.begin(),parsed.value.bytes.end(),out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live=g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if(live==g_live_statement_context_receipts.end())
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,out_result,4075,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_finalize.receipt_stale");
  auto* receipt=live->second.get();std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t,16> preliminary{},mga{};
  if(!uuid_bytes(receipt->view.receipt_uuid,&preliminary)||
     !uuid_bytes(receipt->view.statement_snapshot_uuid,&mga)||
     receipt->released||!receipt->literal_prebind_negotiated||
     receipt->literal_binding_finalized||
     request.preliminary_receipt_uuid!=preliminary||
     bound.preliminary_receipt_uuid!=preliminary||
     request.demand_sha256!=receipt->literal_demand_sha256||
     bound.demand_sha256!=receipt->literal_demand_sha256||
     request.ordered_profile_sha256!=receipt->literal_ordered_profile_sha256||
     request.catalog_generation!=receipt->view.literal_catalog_generation||
     request.security_epoch!=receipt->view.security_epoch||
     request.resource_epoch!=receipt->view.resource_epoch||
     request.mga_snapshot_uuid!=mga||
     bound.nodes.size()!=receipt->literal_demands.size())
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_finalize.live_binding_mismatch");
  for(std::size_t index=0;index<bound.nodes.size();++index){
    const auto& node=bound.nodes[index];const auto demand_it=std::find_if(
        receipt->literal_demands.begin(),receipt->literal_demands.end(),
        [&](const auto& candidate){return candidate.occurrence_id==node.occurrence_id;});
    if(demand_it==receipt->literal_demands.end()||
       node.lexical_sha256!=demand_it->lexical_sha256||
       node.nullable!=demand_it->nullable)
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
                         "SBLR.OPERAND_INVALID",
                         "sblr.literal_finalize.demand_sbba_bijection_failed");
    const auto identity=scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
        receipt->view.literal_catalog_snapshot_uuid,
        receipt->view.literal_catalog_generation,
        receipt->view.literal_registry_generation,
        "019d0000-0000-7000-8000-00000000d711",1);
    std::array<std::uint8_t,16> descriptor{},type{};
    if(!identity.ok||!uuid_bytes(identity.row.descriptor_uuid,&descriptor)||
       !uuid_bytes(identity.row.type_uuid,&type)||node.descriptor_uuid!=descriptor||
       node.descriptor_generation!=identity.row.descriptor_generation||
       node.type_uuid!=type)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.registry_sbba_mismatch");
  }
  const auto table=scratchbird::engine::sblr::DecodeSblrExpressionNodeTableV1(
      request.canonical_sbxn.data(),request.canonical_sbxn.size());
  if((request.canonical_sbxn.empty()&&!bound.nodes.empty())||
     (!request.canonical_sbxn.empty()&&
      (!table.ok||table.table.nodes.size()!=bound.nodes.size())))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.sbba_sbxn_count_mismatch");
  std::size_t mapping_offset=160;
  for(std::size_t index=0;index<bound.nodes.size();++index){
    const auto& ast=bound.nodes[index];const auto node_it=std::find_if(
        table.table.nodes.begin(),table.table.nodes.end(),
        [&](const auto& candidate){return candidate.node_id==ast.node_id;});
    if(node_it==table.table.nodes.end())return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
        "SBLR.OPERAND_INVALID","sblr.literal_finalize.sbxn_node_missing");
    const auto& node=*node_it;
    const auto& sblq=receipt->literal_canonical_sblq;
    if(mapping_offset>sblq.size()||sblq.size()-mapping_offset<12)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.profile_mapping_truncated");
    const auto occurrence=scratchbird::engine::SblrReadU64(sblq.data()+mapping_offset);
    const auto profile_bytes=scratchbird::engine::SblrReadU32(sblq.data()+mapping_offset+8);
    mapping_offset+=12;
    if(profile_bytes>sblq.size()-mapping_offset)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.profile_mapping_extent_invalid");
    const auto profile=scratchbird::engine::sblr::DecodeSblrLiteralDescriptorProfileV1(
        sblq.data()+mapping_offset,profile_bytes);mapping_offset+=profile_bytes;
    if(!profile.ok||occurrence!=ast.occurrence_id||
       profile.profile.profile_uuid!=ast.profile_uuid||
       profile.profile.descriptor_uuid!=ast.descriptor_uuid||
       profile.profile.descriptor_generation!=ast.descriptor_generation||
       profile.profile.type_uuid!=ast.type_uuid||
       profile.profile.profile_binding_sha256!=
         scratchbird::engine::sblr::ComputeSblrLiteralDescriptorProfileBindingV1(
             profile.profile,receipt->view.security_epoch,receipt->view.resource_epoch)||
       node.node_id!=ast.node_id||
       node.parent_operand_ordinal!=ast.parent_operand_ordinal||
       node.descriptor_uuid!=ast.descriptor_uuid||
       node.descriptor_generation!=ast.descriptor_generation)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.profile_sbba_sbxn_bijection_failed");
  }
  if(mapping_offset!=receipt->literal_canonical_sblq.size())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.extra_profile_mapping");
  std::unordered_set<std::string> issued{receipt->view.receipt_uuid};
  std::string final_uuid,token_uuid;
  if(!generate_distinct_statement_context_uuid(&issued,&final_uuid)||
     !generate_distinct_statement_context_uuid(&issued,&token_uuid))
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4076,
                       "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                       "sblr.literal_finalize.identity_unavailable");
  scratchbird::engine::sblr::SblrLiteralAdmissionV1 admission;
  admission.preliminary_receipt_uuid=preliminary;
  if(!uuid_bytes(final_uuid,&admission.final_receipt_uuid)||
     !uuid_bytes(token_uuid,&admission.admission_token_uuid))
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4076,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_finalize.identity_invalid");
  admission.demand_sha256=request.demand_sha256;
  admission.ordered_profile_sha256=request.ordered_profile_sha256;
  admission.bound_ast_sha256=request.bound_ast_sha256;
  admission.sbxn_sha256=request.sbxn_sha256;
  admission.catalog_generation=request.catalog_generation;
  admission.security_epoch=request.security_epoch;
  admission.resource_epoch=request.resource_epoch;
  admission.mga_snapshot_uuid=request.mga_snapshot_uuid;
  auto encoded=scratchbird::engine::sblr::EncodeSblrLiteralAdmissionV1(&admission);
  if(encoded.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4076,
      "DATATYPE.DESCRIPTOR_INVALID","sblr.literal_finalize.admission_encoding_failed");
  const auto executor_snapshot=scratchbird::engine::internal_api::
      LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context);
  if(!executor_snapshot.ok||!executor_snapshot.snapshot.installed||
     executor_snapshot.snapshot.availability_state!=scratchbird::engine::internal_api::
         SblrExecutorAvailabilityState::installed)
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4076,
      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
      "sblr.literal_finalize.executor_evidence_missing");
  receipt->literal_binding_finalized=true;
  receipt->literal_bound_ast_sha256=request.bound_ast_sha256;
  receipt->literal_sbxn_sha256=request.sbxn_sha256;
  receipt->literal_final_receipt_uuid=final_uuid;
  receipt->literal_admission_token_uuid=token_uuid;
  receipt->literal_admission_token_binding_sha256=admission.admission_token_binding_sha256;
  receipt->literal_executor_availability_snapshot=executor_snapshot.snapshot;
  *out_canonical_sbla=std::move(encoded);return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t NegotiateStatementParameterDescriptorsV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbpr,
    std::vector<std::uint8_t>* out_canonical_sbpg,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sbpg == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4080,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.request_invalid");
  }
  out_canonical_sbpg->clear();
  const auto prevalidated = scratchbird::engine::sblr::
      PrevalidateSblrParameterNegotiateRequestV1(canonical_sbpr.data(),
                                                  canonical_sbpr.size());
  if (prevalidated == scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::resource_exceeded) {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    const auto live = g_live_statement_context_receipts.find(
        receipt_handle.opaque_id);
    if (live == g_live_statement_context_receipts.end()) {
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4080,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_negotiate.receipt_stale");
    }
    auto* receipt = live->second.get();
    std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
    const auto receipt_uuid = scratchbird::core::uuid::ParseUuid(
        receipt->view.receipt_uuid);
    const auto mga_uuid = scratchbird::core::uuid::ParseUuid(
        receipt->view.statement_snapshot_uuid);
    if (receipt->released || receipt->parameter_prebind_negotiated ||
        !receipt_uuid.ok() || !mga_uuid.ok() ||
        !std::equal(receipt_uuid.value.bytes.begin(),
                    receipt_uuid.value.bytes.end(), canonical_sbpr.begin()+16) ||
        scratchbird::engine::SblrReadU64(canonical_sbpr.data()+32) !=
            receipt->view.literal_catalog_generation ||
        scratchbird::engine::SblrReadU64(canonical_sbpr.data()+40) !=
            receipt->view.security_epoch ||
        scratchbird::engine::SblrReadU64(canonical_sbpr.data()+48) !=
            receipt->view.resource_epoch ||
        !std::equal(mga_uuid.value.bytes.begin(), mga_uuid.value.bytes.end(),
                    canonical_sbpr.begin()+56)) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4080,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_negotiate.live_binding_mismatch");
    }
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4080,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_negotiate.resource_budget_exceeded");
  }
  if (prevalidated != scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::ok) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4080,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.structural_invalid");
  }
  scratchbird::engine::sblr::SblrParameterNegotiateRequestV1 request;
  std::string decode_detail;
  if (!scratchbird::engine::sblr::DecodeSblrParameterNegotiateRequestV1(
          canonical_sbpr.data(), canonical_sbpr.size(), &request,
          &decode_detail)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4080,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.structural_invalid",
                       decode_detail);
  }
  const auto uuid_bytes = [](const std::string& text,
                             std::array<std::uint8_t, 16>* out) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok() || out == nullptr) return false;
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4081,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_negotiate.receipt_stale");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t, 16> receipt_uuid{}, mga_uuid{};
  if (receipt->released || receipt->parameter_prebind_negotiated ||
      !uuid_bytes(receipt->view.receipt_uuid, &receipt_uuid) ||
      !uuid_bytes(receipt->view.statement_snapshot_uuid, &mga_uuid) ||
      request.preliminary_receipt_uuid != receipt_uuid ||
      request.mga_snapshot_uuid != mga_uuid ||
      request.catalog_generation != receipt->view.literal_catalog_generation ||
      request.security_epoch != receipt->view.security_epoch ||
      request.resource_epoch != receipt->view.resource_epoch) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4081,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_negotiate.live_binding_mismatch");
  }
  if (request.demands.empty()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4081,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.empty_demand_forbidden");
  }
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
      parameter_executor_identity;
  parameter_executor_identity.executor_id =
      scratchbird::engine::internal_api::kSblrParameterExecutorId;
  parameter_executor_identity.opcode_code =
      scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
  parameter_executor_identity.opcode_version =
      scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
  parameter_executor_identity.operand_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
  parameter_executor_identity.result_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
  parameter_executor_identity.result_descriptor_version =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      current_parameter_executor;
  const auto parameter_executor = scratchbird::engine::internal_api::
      RevalidateSblrExecutorAvailability(
          receipt->engine_context, parameter_executor_identity,
          receipt->parameter_executor_availability_snapshot,
          &current_parameter_executor);
  if (parameter_executor.error ||
      current_parameter_executor.generation !=
          receipt->view.parameter_executor_availability_generation) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4081,
                       parameter_executor.error
                           ? parameter_executor.code
                           : "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
                       "sblr.parameter_negotiate.executor_unavailable",
                       parameter_executor.detail);
  }
  const auto identity = scratchbird::core::datatypes::
      LookupDatatypeTypeCodecIdentityV1(
          receipt->view.literal_catalog_snapshot_uuid,
          receipt->view.literal_catalog_generation,
          receipt->view.literal_registry_generation,
          "019d0000-0000-7000-8000-00000000d711", 1);
  if (!identity.ok) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4082,
                       identity.diagnostic_id,
                       "sblr.parameter_negotiate.demand_registry_lookup_failed");
  }
  scratchbird::engine::internal_api::SblrParameterSetIssueRequest issue;
  issue.statement_receipt_uuid = receipt->view.receipt_uuid;
  std::unordered_set<std::string> issued{receipt->view.receipt_uuid};
  if (!generate_distinct_statement_context_uuid(&issued, &issue.execution_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                       "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                       "sblr.parameter_negotiate.execution_identity_unavailable");
  }
  issue.reason_code = "parameter_descriptor_negotiation_v1";
  issue.prepared_statement_uuid =
      receipt->view.parameter_prepared_statement_uuid;
  issue.prepared_generation = receipt->view.parameter_prepared_generation;
  issue.batch_uuid = receipt->view.parameter_batch_uuid;
  issue.batch_generation = receipt->view.parameter_batch_generation;
  issue.dynamic_package_uuid =
      receipt->view.parameter_dynamic_package_uuid;
  issue.dynamic_generation = receipt->view.parameter_dynamic_generation;
  for (const auto& demand : request.demands) {
    if (demand.context_code != 1 || demand.requested_direction != 1 ||
        demand.nullable_demand > 1 || demand.marker_ordinal == 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4082,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_negotiate.demand_code_unregistered");
    }
    issue.slots.push_back({identity.row.descriptor_uuid,
                           identity.row.descriptor_generation,
                           scratchbird::engine::internal_api::
                               SblrParameterDirection::in,
                           demand.nullable_demand == 1});
  }
  const auto issued_set = scratchbird::engine::internal_api::
      IssueSblrParameterSet(receipt->engine_context, issue);
  if (!issued_set.ok) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4082,
                       issued_set.diagnostic.code,
                       issued_set.diagnostic.message_key,
                       issued_set.diagnostic.detail);
  }
  scratchbird::engine::sblr::SblrParameterNegotiateResultV1 response;
  if (!uuid_bytes(receipt->view.receipt_uuid,
                  &response.preliminary_receipt_uuid) ||
      !uuid_bytes(issued_set.snapshot.parameter_set_descriptor_uuid,
                  &response.parameter_set_descriptor_uuid) ||
      !uuid_bytes(issued_set.snapshot.execution_uuid,
                  &response.execution_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.parameter_negotiate.issued_identity_invalid");
  }
  response.descriptor_generation = issued_set.snapshot.descriptor_generation;
  for (std::size_t i = 0; i < request.demands.size(); ++i) {
    const auto& slot = issued_set.snapshot.slots[i];
    scratchbird::engine::sblr::SblrParameterMappingV1 mapping;
    mapping.occurrence_id = request.demands[i].occurrence_id;
    mapping.slot_ordinal = slot.slot_ordinal;
    if (!uuid_bytes(slot.slot_uuid, &mapping.slot_uuid) ||
        !uuid_bytes(slot.datatype_descriptor_uuid,
                    &mapping.datatype_descriptor_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.parameter_negotiate.slot_identity_invalid");
    }
    mapping.datatype_descriptor_generation =
        slot.datatype_descriptor_generation;
    mapping.direction = static_cast<std::uint8_t>(slot.direction);
    mapping.nullable = slot.nullable ? 1 : 0;
    response.mappings.push_back(mapping);
  }
  response.mapping_sha256 =
      scratchbird::engine::sblr::ComputeSblrParameterMappingSha256V1(
          response.mappings);
  auto encoded = scratchbird::engine::sblr::
      EncodeSblrParameterNegotiateResultV1(response);
  if (encoded.empty()) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.result_encoding_failed");
  }
  receipt->parameter_prebind_negotiated = true;
  receipt->parameter_demand_sha256 = request.demand_sha256;
  receipt->parameter_mapping_sha256 = response.mapping_sha256;
  receipt->parameter_demands = request.demands;
  receipt->parameter_set_snapshot = issued_set.snapshot;
  *out_canonical_sbpg = std::move(encoded);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t FinalizeStatementParameterBindingV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbpf,
    std::vector<std::uint8_t>* out_canonical_sbpa,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sbpa == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4083,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.request_invalid");
  }
  out_canonical_sbpa->clear();
  const auto prevalidated = scratchbird::engine::sblr::
      PrevalidateSblrParameterFinalizeRequestV1(canonical_sbpf.data(),
                                                 canonical_sbpf.size());
  if (prevalidated == scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::resource_exceeded) {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    const auto live = g_live_statement_context_receipts.find(
        receipt_handle.opaque_id);
    if (live == g_live_statement_context_receipts.end()) {
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4083,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_finalize.receipt_stale");
    }
    auto* receipt = live->second.get();
    std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
    const auto preliminary = scratchbird::core::uuid::ParseUuid(
        receipt->view.receipt_uuid);
    const auto descriptor = scratchbird::core::uuid::ParseUuid(
        receipt->parameter_set_snapshot.parameter_set_descriptor_uuid);
    const auto execution = scratchbird::core::uuid::ParseUuid(
        receipt->parameter_set_snapshot.execution_uuid);
    if (receipt->released || !receipt->parameter_prebind_negotiated ||
        receipt->parameter_binding_finalized || !preliminary.ok() ||
        !descriptor.ok() || !execution.ok() ||
        !std::equal(preliminary.value.bytes.begin(), preliminary.value.bytes.end(),
                    canonical_sbpf.begin()+16) ||
        !std::equal(descriptor.value.bytes.begin(), descriptor.value.bytes.end(),
                    canonical_sbpf.begin()+32) ||
        scratchbird::engine::SblrReadU64(canonical_sbpf.data()+48) !=
            receipt->parameter_set_snapshot.descriptor_generation ||
        !std::equal(execution.value.bytes.begin(), execution.value.bytes.end(),
                    canonical_sbpf.begin()+56) ||
        !std::equal(receipt->parameter_demand_sha256.begin(),
                    receipt->parameter_demand_sha256.end(),
                    canonical_sbpf.begin()+72) ||
        !std::equal(receipt->parameter_mapping_sha256.begin(),
                    receipt->parameter_mapping_sha256.end(),
                    canonical_sbpf.begin()+104)) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4083,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_finalize.live_binding_mismatch");
    }
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4083,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_finalize.resource_budget_exceeded");
  }
  if (prevalidated != scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::ok) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4083,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.structural_invalid");
  }
  scratchbird::engine::sblr::SblrParameterFinalizeRequestV1 request;
  std::string decode_detail;
  if (!scratchbird::engine::sblr::DecodeSblrParameterFinalizeRequestV1(
          canonical_sbpf.data(), canonical_sbpf.size(), &request,
          &decode_detail)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4083,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.structural_invalid",
                       decode_detail);
  }
  const auto uuid_bytes = [](const std::string& text,
                             std::array<std::uint8_t, 16>* out) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok() || out == nullptr) return false;
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4084,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_finalize.receipt_stale");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t, 16> preliminary{}, descriptor{}, execution{};
  const auto& snapshot = receipt->parameter_set_snapshot;
  if (receipt->released || !receipt->parameter_prebind_negotiated ||
      receipt->parameter_binding_finalized ||
      !uuid_bytes(receipt->view.receipt_uuid, &preliminary) ||
      !uuid_bytes(snapshot.parameter_set_descriptor_uuid, &descriptor) ||
      !uuid_bytes(snapshot.execution_uuid, &execution) ||
      request.preliminary_receipt_uuid != preliminary ||
      request.parameter_set_descriptor_uuid != descriptor ||
      request.execution_uuid != execution ||
      request.descriptor_generation != snapshot.descriptor_generation ||
      request.demand_sha256 != receipt->parameter_demand_sha256 ||
      request.mapping_sha256 != receipt->parameter_mapping_sha256) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4084,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_finalize.live_binding_mismatch");
  }
  const auto table = scratchbird::engine::sblr::DecodeSblrParameterNodeTableV1(
      request.canonical_sbpn.data(), request.canonical_sbpn.size());
  std::vector<std::uint8_t> sbpn_hash_material;
  constexpr std::string_view kSbpnHashDomain =
      "ScratchBird.SblrParameterNodeTable.V1";
  sbpn_hash_material.insert(sbpn_hash_material.end(), kSbpnHashDomain.begin(),
                            kSbpnHashDomain.end());
  sbpn_hash_material.insert(sbpn_hash_material.end(), request.canonical_sbpn.begin(),
                            request.canonical_sbpn.end());
  const auto sbpn_hash = scratchbird::core::hash::ComputeSha256Digest(
      sbpn_hash_material.data(), sbpn_hash_material.size());
  if (!table.ok || table.table.nodes.size() != snapshot.slots.size() ||
      !sbpn_hash.ok() || request.sbpn_sha256 != sbpn_hash.digest) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4084,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.sbpn_invalid");
  }
  std::vector<bool> seen_slots(snapshot.slots.size(), false);
  for (const auto& node : table.table.nodes) {
    if (node.slot_ordinal >= snapshot.slots.size() ||
        seen_slots[node.slot_ordinal]) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4084,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_finalize.slot_bijection_invalid");
    }
    seen_slots[node.slot_ordinal] = true;
    const auto& slot = snapshot.slots[node.slot_ordinal];
    std::array<std::uint8_t, 16> slot_descriptor{}, datatype{};
    if (!uuid_bytes(snapshot.parameter_set_descriptor_uuid, &slot_descriptor) ||
        !uuid_bytes(slot.datatype_descriptor_uuid, &datatype) ||
        node.slot_ordinal != slot.slot_ordinal ||
        node.parameter_set_descriptor_uuid != slot_descriptor ||
        node.parameter_set_generation != snapshot.descriptor_generation ||
        node.datatype_descriptor_uuid != datatype ||
        node.datatype_descriptor_generation !=
            slot.datatype_descriptor_generation) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4084,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_finalize.sbpn_registry_mismatch");
    }
  }
  scratchbird::engine::internal_api::SblrParameterSetSnapshot current;
  const auto revalidated = scratchbird::engine::internal_api::
      RevalidateSblrParameterSet(
          receipt->engine_context, snapshot, receipt->view.receipt_uuid,
          snapshot.execution_uuid, snapshot.prepared_statement_uuid,
          snapshot.prepared_generation, snapshot.batch_uuid,
          snapshot.batch_generation, snapshot.dynamic_package_uuid,
          snapshot.dynamic_generation, &current);
  if (revalidated.code != "OK") {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4084,
                       revalidated.code, revalidated.message_key,
                       revalidated.detail);
  }
  std::unordered_set<std::string> issued{receipt->view.receipt_uuid,
                                         snapshot.execution_uuid};
  std::string final_uuid, token_uuid;
  if (!generate_distinct_statement_context_uuid(&issued, &final_uuid) ||
      !generate_distinct_statement_context_uuid(&issued, &token_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                       "sblr.parameter_finalize.identity_unavailable");
  }
  scratchbird::engine::sblr::SblrParameterAdmissionV1 admission;
  if (!uuid_bytes(final_uuid, &admission.final_receipt_uuid) ||
      !uuid_bytes(token_uuid, &admission.admission_token_uuid) ||
      !uuid_bytes(snapshot.parameter_set_descriptor_uuid,
                  &admission.parameter_set_descriptor_uuid) ||
      !uuid_bytes(snapshot.execution_uuid, &admission.execution_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.identity_invalid");
  }
  admission.descriptor_generation = snapshot.descriptor_generation;
  if ((!snapshot.prepared_statement_uuid.empty() &&
       !uuid_bytes(snapshot.prepared_statement_uuid, &admission.prepared_uuid)) ||
      (!snapshot.batch_uuid.empty() &&
       !uuid_bytes(snapshot.batch_uuid, &admission.batch_uuid)) ||
      (!snapshot.dynamic_package_uuid.empty() &&
       !uuid_bytes(snapshot.dynamic_package_uuid, &admission.dynamic_uuid))) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.route_identity_invalid");
  }
  admission.prepared_generation = snapshot.prepared_generation;
  admission.batch_generation = snapshot.batch_generation;
  admission.dynamic_generation = snapshot.dynamic_generation;
  auto encoded =
      scratchbird::engine::sblr::EncodeSblrParameterAdmissionV1(&admission);
  if (encoded.empty()) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.admission_encoding_failed");
  }
  receipt->parameter_binding_finalized = true;
  receipt->parameter_sbpn_sha256 = request.sbpn_sha256;
  receipt->parameter_final_receipt_uuid = final_uuid;
  receipt->parameter_admission_token_uuid = token_uuid;
  receipt->parameter_admission_binding_sha256 = admission.binding_sha256;
  receipt->parameter_set_snapshot = current;
  *out_canonical_sbpa = std::move(encoded);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t AcquireStatementPackageAdmissionReservation(
    const StatementPackageAdmissionReservationRequest* request,
    StatementPackageAdmissionReservationHandle* out_handle,
    StatementPackageAdmissionReservationView* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (request == nullptr || out_handle == nullptr || out_view == nullptr ||
      !request->receipt || request->canonical_payload_bytes == nullptr ||
      request->payload_kind != StatementSblrPayloadKind::kOpcodeStream) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.request_invalid");
  }
  *out_handle = {};
  *out_view = {};
  const auto* data = request->canonical_payload_bytes;
  const auto size = request->canonical_payload_size;
  if (size < 80 || size > scratchbird::engine::sblr::kSblrOperationMaximumBytes ||
      scratchbird::engine::SblrReadU32(data) !=
          scratchbird::engine::sblr::kSblrOpcodeStreamMagic ||
      scratchbird::engine::SblrReadU16(data + 4) != 1 ||
      scratchbird::engine::SblrReadU16(data + 6) != 0 ||
      scratchbird::engine::SblrReadU16(data + 8) != 64 ||
      scratchbird::engine::SblrReadU16(data + 10) != 0 ||
      scratchbird::engine::SblrReadU32(data + 12) != 0 ||
      scratchbird::engine::SblrReadU32(data + size - 16) !=
          scratchbird::engine::sblr::kSblrOpcodeStreamTrailerMagic ||
      scratchbird::engine::SblrReadU64(data + size - 8) != size ||
      scratchbird::engine::SblrCrc32c(data, size - 12) !=
          scratchbird::engine::SblrReadU32(data + size - 12)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.OPERAND_INVALID",
                       "sblr.package_reservation.structure_invalid");
  }
  const auto count = scratchbird::engine::SblrReadU32(data + 16);
  const auto records_size = scratchbird::engine::SblrReadU64(data + 56);
  if (count < 2 ||
      count > scratchbird::engine::sblr::kSblrOperationMaximumOperands ||
      records_size != size - 80) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.OPERAND_INVALID",
                       "sblr.package_reservation.extent_invalid");
  }
  std::size_t offset = 64;
  for (std::uint32_t index = 0; index != count; ++index) {
    if (offset > size - 16 || size - 16 - offset < 8) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                         "SBLR.OPERAND_INVALID",
                         "sblr.package_reservation.record_invalid");
    }
    const auto record_size = scratchbird::engine::SblrReadU64(data + offset);
    if (record_size > size - 16 - offset - 8) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                         "SBLR.OPERAND_INVALID",
                         "sblr.package_reservation.record_invalid");
    }
    offset += 8 + static_cast<std::size_t>(record_size);
  }
  if (offset != size - 16) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.OPERAND_INVALID",
                       "sblr.package_reservation.trailing_bytes");
  }

  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live =
      g_live_statement_context_receipts.find(request->receipt.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4064,
                       "ENGINE.STATEMENT_CONTEXT.RECEIPT_NOT_LIVE",
                       "engine.statement_context.receipt_not_live");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  auto* session = receipt->session;
  if (session == nullptr || session->package_resource_ledger == nullptr ||
      !session->package_resource_descriptor_initialized) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.package_reservation.policy_unavailable");
  }
  scratchbird::core::agents::ResourceGovernanceReservationAcquireRequest acquire;
  acquire.admission.operation_id = "engine.op.package_begin_end";
  acquire.admission.expected_family = scratchbird::core::agents::
      ResourceGovernanceFamily::kQueryMemoryArena;
  acquire.admission.descriptor = session->package_resource_descriptor;
  auto& quota = acquire.admission.requested;
  const auto max = static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max());
  if (size > max || receipt->view.optimizer_memory_budget_bytes > max - size) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.package_reservation.overflow");
  }
  quota.memory_bytes = static_cast<std::int64_t>(
      size + receipt->view.optimizer_memory_budget_bytes);
  quota.io_bytes = static_cast<std::int64_t>(size);
  quota.io_ops = quota.worker_threads = quota.backlog_items = 1;
  quota.candidate_rows = quota.cache_entries = quota.batch_rows = 1;
  quota.fragments = count;
  quota.lanes = quota.time_budget_microseconds = 1;
  acquire.owner_scope = receipt->view.receipt_uuid;
  auto reserved = session->package_resource_ledger->Acquire(std::move(acquire));
  if (!reserved.ok || !reserved.reservation_created) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.package_reservation.refused",
                       reserved.diagnostic_detail);
  }
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(data, size);
  if (!digest.ok()) {
    (void)session->package_resource_ledger->Release(
        reserved.reservation.token_id);
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4064,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.hash_failed");
  }
  const auto id = g_next_package_admission_reservation_id.fetch_add(1);
  StatementPackageAdmissionReservationOpaque stored;
  stored.receipt_id = request->receipt.opaque_id;
  stored.session = session;
  stored.payload_kind = request->payload_kind;
  stored.payload_size = size;
  stored.record_count = count;
  stored.resource_policy_generation =
      session->package_resource_descriptor.descriptor_generation;
  stored.payload_sha256 = digest.digest;
  stored.ledger_token_id = reserved.reservation.token_id;
  const auto id_admission = ClassifyStatementPackageReservationId(
      id, g_package_admission_reservations.contains(id));
  const bool inserted =
      id_admission == StatementPackageReservationIdAdmission::kAdmitted &&
      g_package_admission_reservations.emplace(id, stored).second;
  if (!inserted) {
    const auto released = session->package_resource_ledger->Release(
        reserved.reservation.token_id,
        scratchbird::core::agents::
            ResourceGovernanceReservationReleaseReason::kRelease);
    if (!released.ok || !released.released) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4064,
                         "RESOURCE.RESERVATION_RELEASE_FAILED",
                         "sblr.package_reservation.id_failure_release_failed");
    }
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       id_admission ==
                               StatementPackageReservationIdAdmission::kZeroExhausted
                           ? "sblr.package_reservation.id_exhausted"
                           : "sblr.package_reservation.id_collision");
  }
  out_handle->opaque_id = id;
  out_view->payload_kind = stored.payload_kind;
  out_view->payload_size = stored.payload_size;
  out_view->record_count = stored.record_count;
  out_view->resource_policy_generation = stored.resource_policy_generation;
  out_view->payload_sha256 = stored.payload_sha256;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t ReadStatementQueryExecuteResultHandle(
    sb_engine_result_t result,
    StatementQueryExecuteResultHandleView* out_handle) {
  if (out_handle == nullptr) return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  *out_handle = {};
  if (!valid_result(result)) return SB_ENGINE_STATUS_INVALID_HANDLE;
  std::lock_guard<std::mutex> guard(result->mutex);
  if (!result->query_execute_result_handle_validated ||
      !result->admitted_query_row_stream_renderer) {
    return SB_ENGINE_STATUS_CONFLICT;
  }
  out_handle->execution_uuid =
      result->query_execute_result_handle.execution_uuid;
  out_handle->result_set_uuid =
      result->query_execute_result_handle.result_set_uuid;
  out_handle->row_descriptor_uuid =
      result->query_execute_result_handle.row_descriptor_uuid;
  out_handle->snapshot_uuid =
      result->query_execute_result_handle.snapshot_uuid;
  if (out_handle->execution_uuid.empty() || out_handle->result_set_uuid.empty() ||
      out_handle->row_descriptor_uuid.empty() || out_handle->snapshot_uuid.empty()) {
    *out_handle = {};
    return SB_ENGINE_STATUS_CONFLICT;
  }
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t ReleaseStatementPackageAdmissionReservation(
    StatementPackageAdmissionReservationHandle handle,
    StatementPackageReservationReleaseReason reason) {
  if (!handle) return SB_ENGINE_STATUS_INVALID_HANDLE;
  std::lock_guard<std::mutex> guard(g_statement_context_receipt_registry_mutex);
  const auto found = g_package_admission_reservations.find(handle.opaque_id);
  if (found == g_package_admission_reservations.end()) {
    return SB_ENGINE_STATUS_ALREADY_RELEASED;
  }
  const auto map_reason = [&] {
    using R = scratchbird::core::agents::ResourceGovernanceReservationReleaseReason;
    switch (reason) {
      case StatementPackageReservationReleaseReason::kCancel: return R::kCancel;
      case StatementPackageReservationReleaseReason::kTimeout: return R::kTimeout;
      case StatementPackageReservationReleaseReason::kDisconnect: return R::kDisconnect;
      case StatementPackageReservationReleaseReason::kShutdown: return R::kShutdown;
      default: return R::kRelease;
    }
  }();
  auto stored = std::move(found->second);
  g_package_admission_reservations.erase(found);
  if (stored.session == nullptr || stored.session->package_resource_ledger == nullptr)
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  const auto released = stored.session->package_resource_ledger->Release(
      stored.ledger_token_id, map_reason);
  return released.ok && released.released ? SB_ENGINE_STATUS_OK
                                          : SB_ENGINE_STATUS_INVALID_HANDLE;
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
        "receipt-bound operation or opcode-stream dispatch does not accept an out-of-band data packet");
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
  StatementPackageAdmissionReservationOpaque consumed_reservation;
  if (request->package_admission_reservation) {
    const auto reserved = g_package_admission_reservations.find(
        request->package_admission_reservation.opaque_id);
    if (reserved == g_package_admission_reservations.end() ||
        reserved->second.receipt_id != request->receipt.opaque_id) {
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4053,
                         "SBLR.INGRESS_REVALIDATION_FAILED",
                         "sblr.package_reservation.missing_or_replayed");
    }
    consumed_reservation = std::move(reserved->second);
    g_package_admission_reservations.erase(reserved);
  }
  registry_guard.unlock();
  using ResourceReleaseReason = scratchbird::core::agents::
      ResourceGovernanceReservationReleaseReason;
  struct PackageResourceReservationGuard {
    scratchbird::core::agents::ResourceGovernanceReservationLedger* ledger;
    std::string token_id;
    ResourceReleaseReason reason = ResourceReleaseReason::kRelease;
    ~PackageResourceReservationGuard() {
      if (ledger != nullptr && !token_id.empty()) {
        (void)ledger->Release(token_id, reason);
      }
    }
  } resource_guard{
      consumed_reservation.session == nullptr
          ? nullptr
          : consumed_reservation.session->package_resource_ledger.get(),
      std::move(consumed_reservation.ledger_token_id)};
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

  const auto& view = receipt->view;
  const auto& context = receipt->engine_context;
  const bool package_candidate =
      request->admitted_payload_kind == StatementSblrPayloadKind::kOpcodeStream;
  if (package_candidate &&
      (!request->package_admission_reservation ||
       consumed_reservation.payload_kind != request->admitted_payload_kind ||
       consumed_reservation.payload_size !=
           request->canonical_operation_bytes.size())) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4053,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.classification_mismatch");
  }
  std::uint32_t structurally_bounded_record_count = 0;
  if (package_candidate) {
    // Allocation-safe SBOS prevalidation. No payload-controlled allocation or
    // semantic SBOP decode is permitted before the session reservation.
    const auto& bytes = request->canonical_operation_bytes;
    const auto read_u16 = [&](std::size_t offset, std::uint16_t* value) {
      if (offset > bytes.size() || bytes.size() - offset < 2) return false;
      *value = static_cast<std::uint16_t>(bytes[offset]) |
               (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
      return true;
    };
    const auto read_u32 = [&](std::size_t offset, std::uint32_t* value) {
      if (offset > bytes.size() || bytes.size() - offset < 4) return false;
      *value = 0;
      for (unsigned n = 0; n != 4; ++n) {
        *value |= static_cast<std::uint32_t>(bytes[offset + n]) << (n * 8);
      }
      return true;
    };
    const auto read_u64 = [&](std::size_t offset, std::uint64_t* value) {
      if (offset > bytes.size() || bytes.size() - offset < 8) return false;
      *value = 0;
      for (unsigned n = 0; n != 8; ++n) {
        *value |= static_cast<std::uint64_t>(bytes[offset + n]) << (n * 8);
      }
      return true;
    };
    std::uint32_t magic = 0, flags = 0, count = 0, reserved = 0;
    std::uint32_t trailer_magic = 0, trailer_crc = 0;
    std::uint16_t major = 0, minor = 0, header_size = 0, reserved16 = 0;
    std::uint64_t records_size = 0, total_size = 0;
    bool bounded = bytes.size() >=
                       scratchbird::engine::sblr::kSblrOpcodeStreamHeaderSize +
                           scratchbird::engine::sblr::kSblrOpcodeStreamTrailerSize &&
                   bytes.size() <=
                       scratchbird::engine::sblr::kSblrOperationMaximumBytes &&
                   read_u32(0, &magic) && read_u16(4, &major) &&
                   read_u16(6, &minor) && read_u16(8, &header_size) &&
                   read_u16(10, &reserved16) && read_u32(12, &flags) &&
                   read_u32(16, &count) && read_u32(20, &reserved) &&
                   read_u64(56, &records_size) &&
                   read_u32(bytes.size() - 16, &trailer_magic) &&
                   read_u32(bytes.size() - 12, &trailer_crc) &&
                   read_u64(bytes.size() - 8, &total_size) &&
                   magic == scratchbird::engine::sblr::kSblrOpcodeStreamMagic &&
                   major == 1 && minor == 0 &&
                   header_size ==
                       scratchbird::engine::sblr::kSblrOpcodeStreamHeaderSize &&
                   reserved16 == 0 && flags == 0 && reserved == 0 &&
                   count >= 2 &&
                   count <= scratchbird::engine::sblr::kSblrOperationMaximumOperands &&
                   records_size == bytes.size() - 80 &&
                   trailer_magic ==
                       scratchbird::engine::sblr::kSblrOpcodeStreamTrailerMagic &&
                   total_size == bytes.size() &&
                   scratchbird::engine::SblrCrc32c(bytes.data(),
                                                    bytes.size() - 12) ==
                       trailer_crc;
    std::size_t offset =
        scratchbird::engine::sblr::kSblrOpcodeStreamHeaderSize;
    for (std::uint32_t index = 0; bounded && index != count; ++index) {
      std::uint64_t record_size = 0;
      bounded = read_u64(offset, &record_size) &&
                offset <= bytes.size() - 16 &&
                bytes.size() - 16 - offset >= 8 &&
                record_size <= bytes.size() - 16 - offset - 8;
      if (bounded) {
        offset += 8 + static_cast<std::size_t>(record_size);
      }
    }
    bounded = bounded && offset == bytes.size() - 16;
    if (!bounded) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4060,
                         "SBLR.OPERAND_INVALID",
                         "sblr.opcode_stream.structural_prevalidation_failed");
    }
    structurally_bounded_record_count = count;
  }

  const auto cancellation_observed = [&]() {
    const bool cancelled = package_candidate &&
        context.query_cancellation_requested &&
        context.query_cancellation_requested();
    if (cancelled) resource_guard.reason = ResourceReleaseReason::kCancel;
    return cancelled;
  };
  if (cancellation_observed()) {
    return fail_result(SB_ENGINE_STATUS_TIMEOUT, out_result, 4062,
                       "PROCESS.CANCELLED",
                       "sblr.opcode_stream.cancelled_before_decode");
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
  bool literal_token_validated=false;
  bool parameter_token_validated=false;
  std::optional<scratchbird::engine::sblr::SblrParameterValueSetV1>
      admitted_parameter_values;
  if (!request->literal_execution_binding.empty()) {
    const auto& binding=request->literal_execution_binding;
    const auto text_uuid_bytes=[](const std::string& text,
                                  const std::uint8_t* expected){
      const auto parsed=scratchbird::core::uuid::ParseUuid(text);
      return parsed.ok()&&std::equal(parsed.value.bytes.begin(),
                                    parsed.value.bytes.end(),expected);
    };
    if(binding.size()!=176||
       !std::equal(binding.begin(),binding.begin()+4,
                   reinterpret_cast<const std::uint8_t*>("SBEL"))||
       scratchbird::engine::SblrReadU16(binding.data()+4)!=1||
       scratchbird::engine::SblrReadU16(binding.data()+6)!=176||
       scratchbird::engine::SblrReadU32(binding.data()+8)!=176||
       scratchbird::engine::SblrReadU32(binding.data()+12)!=0){
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4054,
                         "SBLR.OPERAND_INVALID",
                         "sblr.literal_execution_binding.structural_invalid");
    }
    if(receipt->literal_admission_consumed)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4054,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.literal_execution_binding.token_replayed");
    const char* literal_binding_mismatch = nullptr;
    if (!receipt->literal_binding_finalized) {
      literal_binding_mismatch = "finalize_state_absent";
    } else if (!text_uuid_bytes(receipt->literal_final_receipt_uuid,
                                binding.data() + 16)) {
      literal_binding_mismatch = "final_receipt_uuid_mismatch";
    } else if (!text_uuid_bytes(receipt->literal_admission_token_uuid,
                                binding.data() + 32)) {
      literal_binding_mismatch = "admission_token_uuid_mismatch";
    } else if (!std::equal(
                   binding.begin() + 48, binding.begin() + 80,
                   receipt->literal_admission_token_binding_sha256.begin())) {
      literal_binding_mismatch = "admission_token_binding_sha256_mismatch";
    } else if (!std::equal(binding.begin() + 80, binding.begin() + 112,
                           receipt->literal_bound_ast_sha256.begin())) {
      literal_binding_mismatch = "bound_ast_sha256_mismatch";
    } else if (!std::equal(binding.begin() + 112, binding.begin() + 144,
                           receipt->literal_sbxn_sha256.begin())) {
      literal_binding_mismatch = "sbxn_sha256_mismatch";
    } else if (!std::equal(binding.begin() + 144, binding.end(),
                           request->operation_sha256.begin())) {
      literal_binding_mismatch = "sbos_sha256_mismatch";
    }
    if (literal_binding_mismatch != nullptr) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4054,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         std::string("sblr.literal_execution_binding.") +
                             literal_binding_mismatch);
    }
    literal_token_validated=true;
  }
  if (!request->parameter_execution_binding.empty() ||
      !request->parameter_value_set.empty()) {
    const auto& binding = request->parameter_execution_binding;
    const auto text_uuid_bytes = [](const std::string& text,
                                    const std::uint8_t* expected) {
      const auto parsed = scratchbird::core::uuid::ParseUuid(text);
      return parsed.ok() && std::equal(parsed.value.bytes.begin(),
                                      parsed.value.bytes.end(), expected);
    };
    if (binding.size() != 176 || request->parameter_value_set.empty() ||
        !std::equal(binding.begin(), binding.begin() + 4,
                    reinterpret_cast<const std::uint8_t*>("SBPE")) ||
        scratchbird::engine::SblrReadU16(binding.data() + 4) != 1 ||
        scratchbird::engine::SblrReadU16(binding.data() + 6) != 176 ||
        scratchbird::engine::SblrReadU32(binding.data() + 8) != 176 ||
        scratchbird::engine::SblrReadU32(binding.data() + 12) != 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4054,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_execution_binding.structural_invalid");
    }
    if (receipt->parameter_admission_consumed) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4054,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.parameter_execution_binding.token_replayed");
    }
    const auto& snapshot = receipt->parameter_set_snapshot;
    const auto value_set = scratchbird::engine::sblr::
        DecodeSblrParameterValueSetV1(request->parameter_value_set.data(),
                                      request->parameter_value_set.size());
    const auto value_sha = scratchbird::core::hash::ComputeSha256Digest(
        request->parameter_value_set);
    const bool optional_pairs_match =
        text_uuid_bytes(snapshot.prepared_statement_uuid.empty()
                            ? "00000000-0000-0000-0000-000000000000"
                            : snapshot.prepared_statement_uuid,
                        binding.data() + 72) &&
        scratchbird::engine::SblrReadU64(binding.data() + 88) ==
            snapshot.prepared_generation &&
        text_uuid_bytes(snapshot.batch_uuid.empty()
                            ? "00000000-0000-0000-0000-000000000000"
                            : snapshot.batch_uuid,
                        binding.data() + 96) &&
        scratchbird::engine::SblrReadU64(binding.data() + 112) ==
            snapshot.batch_generation &&
        text_uuid_bytes(snapshot.dynamic_package_uuid.empty()
                            ? "00000000-0000-0000-0000-000000000000"
                            : snapshot.dynamic_package_uuid,
                        binding.data() + 120) &&
        scratchbird::engine::SblrReadU64(binding.data() + 136) ==
            snapshot.dynamic_generation;
    if (!receipt->parameter_binding_finalized || !value_set.ok ||
        !value_sha.ok() || !optional_pairs_match ||
        !text_uuid_bytes(snapshot.execution_uuid, binding.data() + 16) ||
        !text_uuid_bytes(receipt->parameter_final_receipt_uuid,
                         binding.data() + 32) ||
        !text_uuid_bytes(snapshot.parameter_set_descriptor_uuid,
                         binding.data() + 48) ||
        scratchbird::engine::SblrReadU64(binding.data() + 64) !=
            snapshot.descriptor_generation ||
        !std::equal(binding.begin() + 144, binding.end(),
                    value_sha.digest.begin()) ||
        !text_uuid_bytes(snapshot.parameter_set_descriptor_uuid,
                         value_set.value.parameter_set_descriptor_uuid.data()) ||
        value_set.value.descriptor_generation != snapshot.descriptor_generation ||
        !text_uuid_bytes(snapshot.execution_uuid,
                         value_set.value.execution_uuid.data()) ||
        !text_uuid_bytes(receipt->parameter_final_receipt_uuid,
                         value_set.value.statement_receipt_uuid.data()) ||
        value_set.value.records.size() != snapshot.slots.size()) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4054,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_execution_binding.live_binding_mismatch");
    }
    for (std::size_t i = 0; i < value_set.value.records.size(); ++i) {
      const auto& value = value_set.value.records[i];
      const auto& slot = snapshot.slots[i];
      if (value.slot_ordinal != slot.slot_ordinal ||
          !text_uuid_bytes(slot.slot_uuid, value.slot_uuid.data()) ||
          !text_uuid_bytes(slot.datatype_descriptor_uuid,
                           value.datatype_descriptor_uuid.data()) ||
          value.datatype_descriptor_generation !=
              slot.datatype_descriptor_generation ||
          static_cast<std::uint8_t>(value.direction) !=
              static_cast<std::uint8_t>(slot.direction) ||
          value.direction == scratchbird::engine::sblr::
                                 SblrParameterDirectionV1::out ||
          value.state == scratchbird::engine::sblr::
                             SblrParameterValueStateV1::unbound ||
          (value.state == scratchbird::engine::sblr::
                              SblrParameterValueStateV1::null_value &&
           !slot.nullable)) {
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4054,
                           "SBLR.PARAMETER.UNBOUND",
                           "sblr.parameter_value_set.slot_invalid");
      }
    }
    parameter_token_validated = true;
    admitted_parameter_values = value_set.value;
  }
  if (package_candidate &&
      (consumed_reservation.payload_sha256 != request->operation_sha256 ||
       consumed_reservation.record_count != structurally_bounded_record_count ||
       consumed_reservation.resource_policy_generation != view.resource_epoch)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4054,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.binding_mismatch");
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
  if (request->package_executor_evidence.executor_evidence_generation == 0) {
    admission_binding.push_back(0);
  } else {
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->package_admission_reservation.opaque_id);
    admission_binding.push_back(
        static_cast<std::uint8_t>(request->admitted_payload_kind));
    scratchbird::engine::SblrAppendU64(
        admission_binding, consumed_reservation.payload_size);
    scratchbird::engine::SblrAppendU32(
        admission_binding, consumed_reservation.record_count);
    scratchbird::engine::SblrAppendU64(
        admission_binding, consumed_reservation.resource_policy_generation);
    admission_binding.push_back(
        static_cast<std::uint8_t>(request->gateway_evidence.source));
    admission_binding.push_back(
        static_cast<std::uint8_t>(request->gateway_evidence.disposition));
    scratchbird::engine::SblrAppendU64(
        admission_binding,
        request->gateway_evidence.provider_observation_generation);
    admission_binding.insert(
        admission_binding.end(),
        request->gateway_evidence.canonical_payload_sha256.begin(),
        request->gateway_evidence.canonical_payload_sha256.end());
    for (const auto* value : {
             &request->gateway_evidence.route_snapshot_uuid,
             &request->gateway_evidence.security_snapshot_uuid}) {
      admission_binding.insert(admission_binding.end(), value->begin(),
                               value->end());
      admission_binding.push_back(0);
    }
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->gateway_evidence.route_epoch);
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->gateway_evidence.route_generation);
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->gateway_evidence.security_epoch);
    scratchbird::engine::SblrAppendU64(
        admission_binding,
        request->gateway_evidence.security_observation_generation);
    admission_binding.push_back(
        request->gateway_evidence.cluster_context_active ? 1 : 0);
    admission_binding.push_back(
        request->gateway_evidence.cluster_transaction_active ? 1 : 0);
    admission_binding.push_back(
        request->gateway_evidence.route_fence_present ? 1 : 0);
    for (const auto* executor_id : {
             &request->package_executor_evidence.begin_executor_id,
             &request->package_executor_evidence.end_executor_id}) {
      admission_binding.insert(admission_binding.end(), executor_id->begin(),
                               executor_id->end());
      admission_binding.push_back(0);
    }
    admission_binding.insert(
        admission_binding.end(),
        request->package_executor_evidence.registry_snapshot_uuid.begin(),
        request->package_executor_evidence.registry_snapshot_uuid.end());
    admission_binding.push_back(0);
    scratchbird::engine::SblrAppendU64(
        admission_binding,
        request->package_executor_evidence.executor_evidence_generation);
    admission_binding.insert(
        admission_binding.end(),
        request->package_executor_evidence.canonical_payload_sha256.begin(),
        request->package_executor_evidence.canonical_payload_sha256.end());
  }
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
      ((ingress_view.payload_kind ==
            scratchbird::engine::SblrPayloadKind::operation_envelope &&
        (ingress_view.operation_ref_kind != 1 ||
         ingress_view.operation_inline_data == nullptr ||
         ingress_view.operation_inline_size !=
             request->canonical_operation_bytes.size())) ||
       (ingress_view.payload_kind ==
            scratchbird::engine::SblrPayloadKind::opcode_stream &&
        (ingress_view.opcode_ref_kind != 1 ||
         ingress_view.opcode_inline_data == nullptr ||
         ingress_view.opcode_inline_size !=
             request->canonical_operation_bytes.size()))) ||
      container.container.operation_payload !=
          request->canonical_operation_bytes ||
      !std::equal(request->canonical_operation_bytes.begin(),
                  request->canonical_operation_bytes.end(),
                  ingress_view.payload_kind ==
                          scratchbird::engine::SblrPayloadKind::opcode_stream
                      ? ingress_view.opcode_inline_data
                      : ingress_view.operation_inline_data)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4057,
        "SBLR.ENVELOPE.CHECKSUM_MISMATCH",
        "sblr.envelope.checksum_mismatch",
        "outer SBLR and SBEE do not carry the admitted exact payload bytes");
  }
  const std::string_view operation_bytes(
      reinterpret_cast<const char*>(request->canonical_operation_bytes.data()),
      request->canonical_operation_bytes.size());
  const bool opcode_stream = ingress_view.payload_kind ==
      scratchbird::engine::SblrPayloadKind::opcode_stream;
  scratchbird::engine::sblr::SblrDecodeResult operation;
  scratchbird::engine::sblr::SblrOpcodeStreamResult stream;
  if (opcode_stream) {
    stream = scratchbird::engine::sblr::DecodeSblrOpcodeStream(operation_bytes);
    if (stream.ok) {
      operation.ok = true;
      operation.envelope = stream.stream.operations.front();
    }
  } else {
    operation = scratchbird::engine::sblr::DecodeSblrEnvelope(operation_bytes);
  }
  if (!operation.ok ||
      (!opcode_stream &&
       (operation.envelope.operation_id != "query.execute" ||
        operation.envelope.opcode_code != 0x1207 ||
        operation.envelope.opcode != "SBLR_QUERY_EXECUTE"))) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4058,
        "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
        "sblr.operation.opcode_identity_mismatch",
        opcode_stream
            ? (stream.detail.empty() ? "canonical SBOS decoding failed"
                                     : stream.detail)
            : "private statement receipt operation dispatch admits query.execute only");
  }
  std::vector<const std::vector<std::uint8_t>*> literal_tables;
  std::vector<const std::vector<std::uint8_t>*> parameter_tables;
  const auto collect_literal_tables=[&](const auto& envelope){
    for(const auto& operand:envelope.operands){
      if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::expression_node_table)
        literal_tables.push_back(&operand.value_body);
      if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::parameter_node_table)
        parameter_tables.push_back(&operand.value_body);
    }
  };
  if(opcode_stream){for(const auto& member:stream.stream.operations)collect_literal_tables(member);}else collect_literal_tables(operation.envelope);
  if(!literal_tables.empty()&&request->literal_execution_binding.empty()){
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_execution_binding.required");
  }
  if(literal_tables.size()>1){
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_execution_binding.duplicate_table");
  }
  if(!literal_tables.empty()){
    const auto digest=scratchbird::core::hash::ComputeSha256Digest(*literal_tables.front());
    if(!digest.ok()||digest.digest!=receipt->literal_sbxn_sha256){
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_execution_binding.sbxn_hash_mismatch");
    }
  }
  if (!parameter_tables.empty() && !parameter_token_validated) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_execution_binding.required");
  }
  if (parameter_tables.size() > 1) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_execution_binding.duplicate_table");
  }
  if (!parameter_tables.empty()) {
    std::vector<std::uint8_t> material;
    constexpr std::string_view domain =
        "ScratchBird.SblrParameterNodeTable.V1";
    material.insert(material.end(), domain.begin(), domain.end());
    material.insert(material.end(), parameter_tables.front()->begin(),
                    parameter_tables.front()->end());
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
    if (!digest.ok() || digest.digest != receipt->parameter_sbpn_sha256) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4058,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_execution_binding.sbpn_hash_mismatch");
    }
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

  if (opcode_stream && stream.stream.operations.size() != 3) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4060,
                       "SBLR.OPERAND_INVALID",
                       "sblr.opcode_stream.external_root_count_invalid");
  }
  if (opcode_stream &&
      stream.stream.package_descriptor_uuid != view.bound_ast_uuid) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4060,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.opcode_stream.package_descriptor_invalid");
  }
  const bool exact_gateway_evidence = !opcode_stream ||
      (request->gateway_evidence.source ==
           scratchbird::server_engine_bridge::
               StatementGatewayEvidenceSource::kLocalObserved &&
       request->gateway_evidence.disposition ==
           scratchbird::server_engine_bridge::
               StatementGatewayDisposition::kPassThrough &&
       request->gateway_evidence.provider_observation_generation == 1 &&
       request->gateway_evidence.canonical_payload_sha256 ==
           request->operation_sha256 &&
       request->gateway_evidence.route_snapshot_uuid ==
           view.optimizer_route_snapshot_uuid &&
       request->gateway_evidence.route_epoch == view.optimizer_route_epoch &&
       request->gateway_evidence.route_generation ==
           view.optimizer_route_generation &&
       request->gateway_evidence.security_snapshot_uuid ==
           view.security_context_uuid &&
       request->gateway_evidence.security_epoch == view.security_epoch &&
       request->gateway_evidence.security_observation_generation ==
           view.security_epoch &&
       request->gateway_evidence.cluster_context_active ==
           view.cluster_context_active &&
       request->gateway_evidence.cluster_transaction_active ==
           view.cluster_transaction_active &&
       request->gateway_evidence.route_fence_present ==
           view.route_fence_present &&
       !view.cluster_context_active && !view.cluster_transaction_active &&
       !view.route_fence_present);
  if (!exact_gateway_evidence) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4060,
                       "PROCESS.CLUSTER_PATH_ABSENT",
                       "sblr.opcode_stream.gateway_evidence_invalid");
  }
  const bool exact_executor_evidence = !opcode_stream ||
      (request->package_executor_evidence.begin_executor_id ==
           "engine.op.package_begin" &&
       request->package_executor_evidence.end_executor_id ==
           "engine.op.package_end" &&
       request->package_executor_evidence.registry_snapshot_uuid ==
           stream.stream.registry_snapshot_uuid &&
       request->package_executor_evidence.executor_evidence_generation == 1 &&
       request->package_executor_evidence.canonical_payload_sha256 ==
           request->operation_sha256);
  if (!exact_executor_evidence) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4060,
                       "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                       "sblr.opcode_stream.executor_evidence_invalid");
  }

  // SBOP v1 freezes operation identity and typed operands but does not encode
  // duplicate authority booleans.  The engine opcode registry owns this
  // requirement; project it only after the exact query.execute identity and
  // receipt binding above have both been revalidated.
  auto dispatch_operation = operation.envelope;
  if (!opcode_stream) dispatch_operation.requires_transaction_context = true;

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

  constexpr std::size_t kStatementDescriptorProfileCountV10 = 646;
  constexpr std::size_t kMultilegDescriptorProfileCountV10 = 320;
  if (view.descriptor_profiles.size() !=
      kStatementDescriptorProfileCountV10) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4061,
        "ENGINE.STATEMENT_CONTEXT.MULTILEG_DESCRIPTOR_COHORT_INVALID",
        "engine.statement_context.multileg_descriptor_cohort_invalid",
        "live statement receipt is not the exact 646-profile V10 cohort");
  }
  std::vector<scratchbird::engine::optimizer::MultilegDescriptorProfileV1>
      multileg_profiles;
  multileg_profiles.reserve(kMultilegDescriptorProfileCountV10);
  const auto suffix_begin = view.descriptor_profiles.end() -
                            kMultilegDescriptorProfileCountV10;
  for (auto profile = suffix_begin;
       profile != view.descriptor_profiles.end(); ++profile) {
    const auto kind = static_cast<std::uint8_t>(profile->profile_kind);
    if (kind < 14 || kind > 23 || !profile->collation_uuid.empty() ||
        profile->width != 0 || profile->precision != 0 ||
        profile->scale != 0) {
      return fail_result(
          SB_ENGINE_STATUS_CONFLICT,
          out_result,
          4061,
          "ENGINE.STATEMENT_CONTEXT.MULTILEG_DESCRIPTOR_COHORT_INVALID",
          "engine.statement_context.multileg_descriptor_cohort_invalid",
          "live statement receipt V10 suffix metadata changed");
    }
    multileg_profiles.push_back(
        {kind, profile->slot, profile->descriptor_uuid,
         profile->type_uuid, profile->nullable});
  }
  scratchbird::engine::optimizer::MultilegDescriptorDispatchScopeV1
      descriptor_dispatch_scope(view.statement_uuid, multileg_profiles);
  if (!descriptor_dispatch_scope.installed()) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4061,
        descriptor_dispatch_scope.diagnostic_id(),
        "engine.statement_context.multileg_descriptor_scope_refused",
        descriptor_dispatch_scope.detail());
  }

  // Consume only after every structural, receipt, profile, security, gateway,
  // executor-evidence and resource admission check above has passed, but
  // immediately before executor entry. The receipt mutex makes this atomic.
  if(literal_token_validated){
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current;
    const auto availability=scratchbird::engine::internal_api::
        RevalidateSblrExecutorAvailability(
            receipt->engine_context,
            receipt->literal_executor_availability_snapshot,&current);
    if(availability.error)
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4061,
                         availability.code,availability.message_key,
                         availability.detail);
    if(receipt->literal_admission_consumed)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4061,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.literal_execution_binding.token_replayed");
    receipt->literal_admission_consumed=true;
  }
  if (parameter_token_validated) {
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
        parameter_identity;
    parameter_identity.executor_id =
        scratchbird::engine::internal_api::kSblrParameterExecutorId;
    parameter_identity.opcode_code =
        scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
    parameter_identity.opcode_version =
        scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
    parameter_identity.operand_descriptor_id =
        scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
    parameter_identity.result_descriptor_id =
        scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
    parameter_identity.result_descriptor_version =
        scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
        current_parameter_executor;
    const auto parameter_executor = scratchbird::engine::internal_api::
        RevalidateSblrExecutorAvailability(
            receipt->engine_context, parameter_identity,
            receipt->parameter_executor_availability_snapshot,
            &current_parameter_executor);
    if (parameter_executor.error ||
        current_parameter_executor.generation !=
            receipt->view.parameter_executor_availability_generation) {
      return fail_result(
          SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4061,
          parameter_executor.error
              ? parameter_executor.code
              : "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
          "sblr.parameter_execution.executor_revalidation_refused",
          parameter_executor.detail);
    }
    scratchbird::engine::internal_api::SblrParameterSetSnapshot current;
    const auto& admitted = receipt->parameter_set_snapshot;
    const auto revalidated = scratchbird::engine::internal_api::
        RevalidateSblrParameterSet(
            receipt->engine_context, admitted, receipt->view.receipt_uuid,
            admitted.execution_uuid, admitted.prepared_statement_uuid,
            admitted.prepared_generation, admitted.batch_uuid,
            admitted.batch_generation, admitted.dynamic_package_uuid,
            admitted.dynamic_generation, &current);
    if (revalidated.error) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4061,
                         revalidated.code, revalidated.message_key,
                         revalidated.detail);
    }
    if (receipt->parameter_admission_consumed) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4061,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.parameter_execution_binding.token_replayed");
    }
    receipt->parameter_admission_consumed = true;
  }

  scratchbird::engine::sblr::SblrDispatchResult dispatched;
  if (opcode_stream) {
    scratchbird::engine::sblr::SblrOpcodeStreamAdmission stream_admission;
    stream_admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
    stream_admission.authenticated = context.security_context_present;
    stream_admission.descriptor_class_accepted =
        stream.stream.package_descriptor_uuid == view.bound_ast_uuid;
    stream_admission.gateway_pass_through =
        request->gateway_evidence.source ==
            scratchbird::server_engine_bridge::
                StatementGatewayEvidenceSource::kLocalObserved &&
        request->gateway_evidence.disposition ==
            scratchbird::server_engine_bridge::
                StatementGatewayDisposition::kPassThrough &&
        request->gateway_evidence.provider_observation_generation == 1 &&
        request->gateway_evidence.canonical_payload_sha256 ==
            request->operation_sha256;
    stream_admission.executor_evidence_accepted =
        request->package_executor_evidence.begin_executor_id ==
            "engine.op.package_begin" &&
        request->package_executor_evidence.end_executor_id ==
            "engine.op.package_end" &&
        request->package_executor_evidence.registry_snapshot_uuid ==
            view.catalog_epoch_uuid &&
        request->package_executor_evidence.executor_evidence_generation == 1 &&
        request->package_executor_evidence.canonical_payload_sha256 ==
            request->operation_sha256;
    stream_admission.cancelled = cancellation_observed();
    stream_admission.resource_budget_available =
        resource_guard.ledger != nullptr && !resource_guard.token_id.empty();
    const auto admitted_stream =
        scratchbird::engine::sblr::AdmitSblrOpcodeStream(
            operation_bytes, stream_admission);
    if (!admitted_stream.ok) {
      if (admitted_stream.diagnostic_id == "PROCESS.CANCELLED") {
        resource_guard.reason = ResourceReleaseReason::kCancel;
      } else if (admitted_stream.diagnostic_id == "PROCESS.TIMEOUT" ||
                 admitted_stream.diagnostic_id == "TIMEOUT") {
        resource_guard.reason = ResourceReleaseReason::kTimeout;
      }
      const auto refusal_status = [&]() {
        if (admitted_stream.diagnostic_id == "SECURITY.ACCESS_DENIED")
          return SB_ENGINE_STATUS_SECURITY_DENIED;
        if (admitted_stream.diagnostic_id == "RESOURCE.BUDGET_EXCEEDED")
          return SB_ENGINE_STATUS_RESOURCE_EXHAUSTED;
        if (admitted_stream.diagnostic_id == "PROCESS.CANCELLED")
          return SB_ENGINE_STATUS_TIMEOUT;
        if (admitted_stream.diagnostic_id == "PROCESS.CLUSTER_PATH_ABSENT")
          return SB_ENGINE_STATUS_CONFLICT;
        if (admitted_stream.diagnostic_id ==
            "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING")
          return SB_ENGINE_STATUS_UNSUPPORTED;
        return SB_ENGINE_STATUS_INVALID_ARGUMENT;
      }();
      return fail_result(
          refusal_status,
          out_result,
          4062,
          admitted_stream.diagnostic_id,
          "sblr.opcode_stream.rejected",
          admitted_stream.detail);
    }
    // Receipt dispatch v1 admits exactly one contained root. Validate its
    // complete envelope, registry identity, and context requirements before
    // entering any executor so a late package member can never follow an
    // earlier side effect.
    auto member = admitted_stream.stream.operations[1];
    {
      const auto* member_entry =
          scratchbird::engine::sblr::LookupSblrOpcodeCode(member.opcode_code);
      if (member_entry != nullptr) {
        member.requires_security_context =
            member_entry->requires_security_context;
        member.requires_transaction_context =
            member_entry->requires_transaction_context;
        member.requires_cluster_authority =
            member_entry->requires_cluster_authority;
      }
      scratchbird::engine::internal_api::EngineApiRequest preflight_api_request;
      const auto member_preflight =
          scratchbird::engine::sblr::PreflightSblrQueryOperation(
              {context, member, std::move(preflight_api_request)});
      if (member_entry == nullptr || !member_preflight.ok) {
        return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4062,
                           member_preflight.diagnostic_id.empty()
                               ? "SBLR.OPERATION_UNSUPPORTED"
                               : member_preflight.diagnostic_id,
                           "sblr.opcode_stream.member_preflight_refused",
                           member_preflight.detail);
      }
      // Preflight materializes a private semantic view (typed operand values
      // and UUID property names) for validation.  The dispatcher must still
      // receive the original canonical envelope: DispatchSblrOperation owns
      // the canonical-envelope validation barrier and performs its own
      // materialization only after that barrier.  Replacing `member` with the
      // preflight view would both double-materialize it and make the strict
      // canonical validator reject an otherwise admitted package root.
      if ((member.requires_security_context &&
           !context.security_context_present) ||
          (member.requires_transaction_context &&
           context.local_transaction_id == 0 &&
           context.transaction_uuid.canonical.empty()) ||
          (member.requires_cluster_authority &&
           !context.cluster_authority_available)) {
        return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4062,
                           "SECURITY.ACCESS_DENIED",
                           "sblr.opcode_stream.member_context_refused");
      }
    }
    scratchbird::engine::internal_api::EngineApiRequest member_request;
    WriteEngineAbiPhaseTrace(
        "sblr_opcode_stream_admitted.PASS_THROUGH.statement_context_receipt",
        "engine.op.package_begin", request->canonical_operation_bytes.size(),
        {{"receipt_bound_dispatch", 0}});
    dispatched = scratchbird::engine::sblr::DispatchSblrOperation(
        {context, std::move(member), std::move(member_request),
         admitted_parameter_values});
    if (dispatched.accepted && dispatched.api_result.ok) {
      WriteEngineAbiPhaseTrace(
          "sblr_opcode_stream_admitted.PASS_THROUGH.statement_context_receipt",
          "engine.op.package_end", request->canonical_operation_bytes.size(),
          {{"receipt_bound_dispatch", 0}});
    }
  } else {
    scratchbird::engine::internal_api::EngineApiRequest api_request;
    dispatched = scratchbird::engine::sblr::DispatchSblrOperation(
        {context, std::move(dispatch_operation), std::move(api_request),
         admitted_parameter_values});
  }
  if (!dispatched.accepted || !dispatched.api_result.ok) {
    const auto failure_code = operation_envelope_failure_code(dispatched);
    if (failure_code == "PROCESS.CANCELLED") {
      resource_guard.reason = ResourceReleaseReason::kCancel;
    } else if (failure_code == "PROCESS.TIMEOUT" ||
               failure_code == "TIMEOUT") {
      resource_guard.reason = ResourceReleaseReason::kTimeout;
    }
    return fail_result(
        operation_envelope_failure_status(dispatched),
        out_result,
        4063,
        failure_code,
        "sblr.operation_envelope.rejected",
        first_dispatch_diagnostic_detail(dispatched),
        first_dispatch_diagnostic_fields(dispatched));
  }

  scratchbird::engine::sblr::QueryExecuteResultHandleValidationV1
      query_handle_validation;
  if (opcode_stream) {
    const auto& shape = dispatched.api_result.result_shape;
    if (std::any_of(shape.rows.begin(), shape.rows.end(),
                    [&](const auto& row) {
                      return row.fields.size() != shape.columns.size();
                    })) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4065,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "query_execute_result.row_stream_descriptor_invalid");
    }
    std::unordered_set<std::string> result_identities{
        view.statement_snapshot_uuid};
    std::string execution_uuid;
    std::string result_set_uuid;
    std::string row_descriptor_uuid;
    if (!generate_distinct_statement_context_uuid(&result_identities,
                                                   &execution_uuid) ||
        !generate_distinct_statement_context_uuid(&result_identities,
                                                   &result_set_uuid) ||
        !generate_distinct_statement_context_uuid(&result_identities,
                                                   &row_descriptor_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                         "query_execute_result.identity_unavailable");
    }
    std::vector<scratchbird::engine::sblr::QueryExecuteResultHandleFieldV1>
        handle_fields{
            {"execution_uuid", "desc.uuid", std::move(execution_uuid)},
            {"result_set_uuid", "desc.uuid", std::move(result_set_uuid)},
            {"row_descriptor_uuid", "desc.uuid",
             std::move(row_descriptor_uuid)},
            {"snapshot_uuid", "desc.uuid", view.statement_snapshot_uuid}};
    query_handle_validation =
        scratchbird::engine::sblr::ValidateQueryExecuteResultHandleV1(
            "query_execute_result", 1, handle_fields);
    if (!query_handle_validation.ok) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4065,
                         query_handle_validation.diagnostic_id,
                         "query_execute_result.handle_invalid",
                         query_handle_validation.detail);
    }
    WriteEngineAbiPhaseTrace(
        "query_execute_result.handle_validated."
        "admitted_query_row_stream_renderer",
        "query.execute", request->canonical_operation_bytes.size(),
        {{"registry_handle_validation", 0},
         {"row_descriptor_binding", 0}});
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
  if (opcode_stream) {
    result->query_execute_result_handle = query_handle_validation.handle;
    result->query_execute_result_handle_validated = true;
    result->admitted_query_row_stream_renderer = true;
  }
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
  if ((result->query_execute_result_handle_validated ||
       result->admitted_query_row_stream_renderer) &&
      !(result->query_execute_result_handle_validated &&
        result->admitted_query_row_stream_renderer)) {
    return SB_ENGINE_STATUS_CONFLICT;
  }
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
