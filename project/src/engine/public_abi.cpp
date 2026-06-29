// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "database_format.hpp"
#include "local_transaction_store.hpp"
#include "sblr_dispatch.hpp"
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
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

constexpr std::uint64_t kEngineMagic = 0x5342454e47494e45ull;
constexpr std::uint64_t kSessionMagic = 0x534245534553534eull;
constexpr std::uint64_t kTransactionMagic = 0x53425452414e5343ull;
constexpr std::uint64_t kResultMagic = 0x534245524553554cull;

constexpr const char* kBuildId = "scratchbird-engine-abi-v1";

struct DiagnosticStorage {
  sb_engine_diagnostic_view_t view{};
  std::string code;
  std::string message;
  std::string detail;
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
  std::uint32_t active_transactions = 0;
  std::uint32_t open_streams = 0;
};

struct sb_engine_transaction_s {
  std::uint64_t magic = kTransactionMagic;
  mutable std::mutex mutex;
  sb_engine_session_t session = nullptr;
  bool closed = false;
};

namespace {

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

std::string current_monotonic_ns_text() {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
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
  internal.snapshot_visible_through_local_transaction_id = context.transaction_ref;
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

sb_engine_status_t operation_envelope_failure_status(const scratchbird::engine::sblr::SblrDispatchResult& result) {
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
                               std::string detail);

sb_engine_status_t dispatch_operation_envelope(sb_engine_session_t session,
                                               const sb_engine_request_context_v1_t& context,
                                               const scratchbird::engine::SblrExecutionEnvelope& envelope,
                                               const sb_engine_sblr_dispatch_params_v1_t& params,
                                               sb_engine_result_t* out_result) {
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
  const auto api_context = make_internal_context(session->engine, context);
  mark_phase("make_internal_context");
  scratchbird::engine::internal_api::EngineApiRequest api_request;
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
  const auto dispatch_result =
      scratchbird::engine::sblr::DecodeAndDispatchSblrOperation(encoded,
                                                                api_context,
                                                                std::move(api_request));
  mark_phase("decode_and_dispatch_operation");
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
                       first_dispatch_diagnostic_detail(dispatch_result));
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
    result->affected_rows = dispatch_result.api_result.dml_summary.rows_changed;
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
                    std::string detail = {}) {
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
    result->diagnostic_views.push_back(diagnostic.view);
  }
}

sb_engine_status_t fail_result(sb_engine_status_t status,
                               sb_engine_result_t* out_result,
                               std::uint32_t numeric_code,
                               std::string code,
                               std::string message,
                               std::string detail = {}) {
  if (out_result != nullptr) {
    auto* result = make_result(SB_ENGINE_RESULT_DIAGNOSTIC_ONLY);
    add_diagnostic(result, numeric_code, SB_ENGINE_DIAGNOSTIC_ERROR, std::move(code), std::move(message), std::move(detail));
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
    const auto status = dispatch_operation_envelope(session, *context, decoded.envelope, *params, out_result);
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
