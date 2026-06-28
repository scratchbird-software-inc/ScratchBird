// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_SBLR_DISPATCH_RESULTS

#include "sblr_dispatch_server.hpp"

#include "sblr_admission.hpp"

#include "backup_archive/backup_archive_api.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/sbsql_language_elements_catalog.hpp"
#include "catalog/sys_information_projection.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/show_api.hpp"
#include "security/security_crypto_policy.hpp"
#include "transaction/transaction_api.hpp"

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"
#include "../engine/sblr/sblr_opcode_registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace scratchbird::server {

namespace {

namespace agents = scratchbird::core::agents;
namespace engine_api = scratchbird::engine::internal_api;

constexpr std::uint32_t kSchemaPrepareSblrTestV1 = 4001;
constexpr std::uint32_t kSchemaPrepareResultTestV1 = 4002;
constexpr std::uint32_t kSchemaExecuteSblrTestV1 = 4003;
constexpr std::uint32_t kSchemaExecuteResultTestV1 = 4004;
constexpr std::uint32_t kSchemaFetchTestV1 = 4005;
constexpr std::uint32_t kSchemaFetchResultTestV1 = 4006;
constexpr std::uint32_t kSchemaCloseCursorTestV1 = 4007;
constexpr std::uint32_t kSchemaCloseCursorResultTestV1 = 4008;
constexpr std::uint32_t kCursorCloseFlagCancel = 1u << 0;
constexpr std::uint16_t kLongStringSentinel = 0xffff;

std::string CurrentUtcTimestampText() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string CurrentMonotonicNsText() {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}

using ServerSteadyClock = std::chrono::steady_clock;

std::uint64_t ServerElapsedMicros(ServerSteadyClock::time_point start,
                                  ServerSteadyClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WritePublicAbiPhaseTrace(
    std::string_view operation_id,
    std::string_view encoded,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros,
    std::size_t envelope_size) {
  const char* trace_path = std::getenv("SCRATCHBIRD_PUBLIC_ABI_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "operation=" << operation_id
      << "\tencoded_bytes=" << encoded.size()
      << "\tenvelope_bytes=" << envelope_size;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
}

void WriteServerPhaseTrace(
    const char* env_name,
    std::string_view layer,
    std::string_view operation_id,
    std::size_t encoded_size,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv(env_name);
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "layer=" << layer
      << "\toperation=" << operation_id
      << "\tencoded_bytes=" << encoded_size;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
}

std::string ServerTraceField(std::string_view value) {
  std::string out(value);
  for (char& ch : out) {
    if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
  }
  return out;
}

void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) { out->push_back(value); }
void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}
void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}
void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}
std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}
std::uint32_t GetU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int byte = 3; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}
std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}
std::optional<std::uint64_t> JsonU64Field(std::string_view encoded, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = encoded.find(needle);
  if (key_pos == std::string_view::npos) return std::nullopt;
  const std::size_t colon = encoded.find(':', key_pos + needle.size());
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  std::uint64_t value = 0;
  bool saw_digit = false;
  while (cursor < encoded.size() && std::isdigit(static_cast<unsigned char>(encoded[cursor]))) {
    saw_digit = true;
    value = value * 10 + static_cast<std::uint64_t>(encoded[cursor] - '0');
    ++cursor;
  }
  return saw_digit ? std::optional<std::uint64_t>(value) : std::nullopt;
}

std::optional<std::string> JsonTextField(std::string_view encoded, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = encoded.find(needle);
  if (key_pos == std::string_view::npos) return std::nullopt;
  const std::size_t colon = encoded.find(':', key_pos + needle.size());
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  if (cursor >= encoded.size() || encoded[cursor] != '"') return std::nullopt;
  ++cursor;
  std::string out;
  bool escaped = false;
  while (cursor < encoded.size()) {
    const char ch = encoded[cursor++];
    if (!escaped && ch == '"') return out;
    if (!escaped && ch == '\\') {
      escaped = true;
      continue;
    }
    out.push_back(ch);
    escaped = false;
  }
  return std::nullopt;
}

std::optional<std::string> JsonPrimitiveField(std::string_view encoded, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = encoded.find(needle);
  if (key_pos == std::string_view::npos) return std::nullopt;
  const std::size_t colon = encoded.find(':', key_pos + needle.size());
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  if (cursor >= encoded.size() || encoded[cursor] == '"') return std::nullopt;
  const std::size_t begin = cursor;
  while (cursor < encoded.size() &&
         encoded[cursor] != ',' &&
         encoded[cursor] != '}' &&
         !std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  if (cursor == begin) return std::nullopt;
  return std::string(encoded.substr(begin, cursor - begin));
}

bool JsonBoolField(std::string_view encoded, std::string_view key, bool default_value = false) {
  const auto primitive = JsonPrimitiveField(encoded, key);
  if (!primitive) return default_value;
  return *primitive == "true" || *primitive == "1";
}

std::optional<std::uint64_t> ParseUnsignedDecimal(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return std::nullopt;
    }
    value = value * 10u + digit;
  }
  return value;
}

agents::AgentRuntimeStatus ScheduleFromEnvelope(std::string_view encoded,
                                                std::uint64_t now_microseconds,
                                                agents::BackgroundJobSchedule* schedule) {
  if (schedule == nullptr) {
    return agents::AgentError("BACKGROUND_JOBS.SCHEDULE_DEFINITION_INVALID",
                              "schedule_target_required");
  }
  schedule->schedule_name = JsonTextField(encoded, "schedule_name").value_or("");
  schedule->schedule_uuid = JsonTextField(encoded, "schedule_uuid").value_or("");
  schedule->schedule_kind = JsonTextField(encoded, "schedule_kind").value_or("");
  schedule->expression = JsonTextField(encoded, "schedule_expression").value_or("");
  schedule->starts_expression =
      JsonTextField(encoded, "schedule_starts_expression").value_or("");
  schedule->ends_expression = JsonTextField(encoded, "schedule_ends_expression").value_or("");
  schedule->time_zone = JsonTextField(encoded, "schedule_time_zone").value_or("UTC");
  schedule->enabled = true;
  if (schedule->schedule_uuid.empty() || schedule->schedule_name.empty() ||
      schedule->schedule_kind.empty() || schedule->expression.empty()) {
    return agents::AgentError("BACKGROUND_JOBS.SCHEDULE_DEFINITION_INVALID",
                              "schedule_uuid_name_kind_and_expression_required");
  }
  if (schedule->schedule_kind == "every") {
    const auto seconds = ParseUnsignedDecimal(schedule->expression);
    if (!seconds || *seconds == 0 ||
        *seconds > std::numeric_limits<std::uint64_t>::max() / 1000000u) {
      return agents::AgentError("BACKGROUND_JOBS.SCHEDULE_INTERVAL_INVALID",
                                schedule->expression);
    }
    schedule->interval_microseconds = *seconds * 1000000u;
    if (now_microseconds >
        std::numeric_limits<std::uint64_t>::max() - schedule->interval_microseconds) {
      return agents::AgentError("BACKGROUND_JOBS.SCHEDULE_INTERVAL_INVALID",
                                "next_run_overflow");
    }
    schedule->next_run_after_microseconds =
        now_microseconds + schedule->interval_microseconds;
  } else if (schedule->schedule_kind == "at" || schedule->schedule_kind == "cron") {
    schedule->interval_microseconds = 0;
    schedule->next_run_after_microseconds = now_microseconds;
  } else {
    return agents::AgentError("BACKGROUND_JOBS.SCHEDULE_KIND_UNSUPPORTED",
                              schedule->schedule_kind);
  }
  return agents::AgentOk();
}

std::string ReplaceJsonTextField(std::string encoded,
                                 std::string_view key,
                                 std::string_view replacement) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = encoded.find(needle);
  if (key_pos == std::string::npos) return encoded;
  const std::size_t colon = encoded.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return encoded;
  std::size_t cursor = colon + 1;
  while (cursor < encoded.size() &&
         std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  if (cursor >= encoded.size() || encoded[cursor] != '"') return encoded;
  const std::size_t value_begin = cursor + 1;
  cursor = value_begin;
  bool escaped = false;
  while (cursor < encoded.size()) {
    const char ch = encoded[cursor];
    if (!escaped && ch == '"') {
      encoded.replace(value_begin, cursor - value_begin, replacement);
      return encoded;
    }
    escaped = !escaped && ch == '\\';
    if (ch != '\\') escaped = false;
    ++cursor;
  }
  return encoded;
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}
std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  return uuid;
}
void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  if (value.size() >= kLongStringSentinel) {
    PutU16(out, kLongStringSentinel);
    PutU64(out, static_cast<std::uint64_t>(value.size()));
  } else {
    PutU16(out, static_cast<std::uint16_t>(value.size()));
  }
  out->insert(out->end(), value.begin(), value.end());
}
bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  auto length = static_cast<std::uint64_t>(GetU16(data, *offset));
  *offset += 2;
  if (length == kLongStringSentinel) {
    if (*offset + 8 > data.size()) return false;
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

void PutBytes(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& value) {
  PutU64(out, static_cast<std::uint64_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadBytes(const std::vector<std::uint8_t>& data,
               std::size_t* offset,
               std::vector<std::uint8_t>* out) {
  if (*offset + 8 > data.size()) return false;
  const std::uint64_t length = GetU64(data, *offset);
  *offset += 8;
  if (length > static_cast<std::uint64_t>(data.size() - *offset)) return false;
  out->assign(data.begin() + static_cast<std::ptrdiff_t>(*offset),
              data.begin() + static_cast<std::ptrdiff_t>(*offset + length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4u) & 0x0fu] << kHex[ch & 0x0fu];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

std::string EscapeOperationOperandField(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch; break;
    }
  }
  return out;
}

std::string UnescapeOperationOperandField(std::string_view value) {
  std::string out;
  bool escaped = false;
  for (const char ch : value) {
    if (!escaped) {
      if (ch == '\\') {
        escaped = true;
      } else {
        out.push_back(ch);
      }
      continue;
    }
    switch (ch) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case '\\': out.push_back('\\'); break;
      default:
        out.push_back('\\');
        out.push_back(ch);
        break;
    }
    escaped = false;
  }
  if (escaped) out.push_back('\\');
  return out;
}

std::optional<std::string> ExistingTextOperandValue(std::string_view encoded,
                                                    std::string_view key) {
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.starts_with("operand=")) {
      const std::string_view payload = line.substr(std::string_view("operand=").size());
      const std::size_t first = payload.find('\t');
      const std::size_t second =
          first == std::string_view::npos ? std::string_view::npos : payload.find('\t', first + 1);
      if (first != std::string_view::npos && second != std::string_view::npos &&
          UnescapeOperationOperandField(payload.substr(0, first)) == "text" &&
          UnescapeOperationOperandField(payload.substr(first + 1, second - first - 1)) == key) {
        return UnescapeOperationOperandField(payload.substr(second + 1));
      }
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

bool IsPublicRegistryPlaceholder(std::string_view value) {
  return value == "engine_resolves_from_public_registry" ||
         value == "engine_resolves_from_catalog" ||
         value == "public_registry";
}

void AppendExistingOperationOperands(std::string_view encoded, std::string* operation_envelope) {
  if (operation_envelope == nullptr) return;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.starts_with("operand=")) {
      operation_envelope->append(line);
      operation_envelope->push_back('\n');
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
}

ServerDiagnostic SblrServerDiagnostic(std::string code,
                                      std::string message,
                                      std::string detail = {}) {
  std::vector<ServerDiagnosticField> fields;
  if (!detail.empty()) fields.push_back({"detail", std::move(detail)});
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

std::string CanonicalResultColumnsJson() {
  return "["
         "{\"ordinal\":0,\"name\":\"value\",\"alias\":\"value\","
         "\"object_uuid\":\"019e05df-f012-7000-8000-0000000000f0\","
         "\"type\":\"int64\",\"canonical_type\":\"int64\",\"domain\":\"sb.int64\","
         "\"precision\":19,\"scale\":0,\"length\":8,\"charset\":\"\","
         "\"collation\":\"\",\"nullable\":false,\"generated\":false,"
         "\"computed\":false,\"identity\":false,\"has_default\":false,"
         "\"hidden\":false,\"system\":false},"
         "{\"ordinal\":1,\"name\":\"_sb_row_version\",\"alias\":\"_sb_row_version\","
         "\"object_uuid\":\"019e05df-f012-7000-8000-0000000000f1\","
         "\"type\":\"uint64\",\"canonical_type\":\"uint64\",\"domain\":\"sb.system.row_version\","
         "\"precision\":20,\"scale\":0,\"length\":8,\"charset\":\"\","
         "\"collation\":\"\",\"nullable\":false,\"generated\":true,"
         "\"computed\":true,\"identity\":false,\"has_default\":false,"
         "\"hidden\":true,\"system\":true}"
         "]";
}

std::string CanonicalResultMetadataJson(std::string_view command_tag,
                                        std::uint64_t rows_affected,
                                        std::uint64_t returned_rows) {
  std::ostringstream out;
  out << "{\"result_shape\":\"canonical.rowset.v1\","
      << "\"columns\":" << CanonicalResultColumnsJson() << ","
      << "\"completion\":{\"command_tag\":\"" << JsonEscape(command_tag)
      << "\",\"rows_affected\":" << rows_affected
      << ",\"returned_rows\":" << returned_rows << "},"
      << "\"warnings\":[],\"notices\":[],"
      << "\"cursor_metadata\":{\"forward_only\":true,\"scrollable\":false,"
      << "\"updatable\":false,\"holdable\":false}}";
  return out.str();
}

std::string ResultPacket(const std::string& operation_id,
                         bool ok,
                         std::uint64_t row_count,
                         const std::string& detail = {},
                         std::uint64_t first_row_index = 0,
                         std::uint64_t total_rows = 0,
                         bool end_of_stream = true) {
  if (total_rows == 0) total_rows = row_count;
  std::ostringstream out;
  out << "{\"engine_result\":{\"operation_id\":\"" << JsonEscape(operation_id) << "\","
      << "\"ok\":" << (ok ? "true" : "false") << ","
      << "\"row_count\":" << row_count << ",\"stream\":{\"chunk_start\":"
      << first_row_index << ",\"chunk_rows\":" << row_count
      << ",\"total_rows\":" << total_rows
      << ",\"end_of_stream\":" << (end_of_stream ? "true" : "false")
      << "},\"metadata\":"
      << CanonicalResultMetadataJson("SELECT " + std::to_string(total_rows),
                                     total_rows,
                                     row_count)
      << ",\"rows\":[";
  for (std::uint64_t i = 0; i < row_count; ++i) {
    if (i != 0) out << ',';
    out << "{\"operation_id\":\"" << JsonEscape(operation_id) << "\",\"row_index\":"
        << (first_row_index + i) << ",\"status\":\"" << (ok ? "accepted" : "rejected")
        << "\"}";
  }
  out << "],\"diagnostics\":[";
  if (!ok) {
    out << "{\"code\":\"SBLR.CAPABILITY.FORBIDDEN\",\"detail\":\"" << JsonEscape(detail) << "\"}";
  }
  out << "]}}\n";
  return out.str();
}

std::uint64_t BulkStreamEventCount(std::uint64_t rejected_rows) {
  return 3 + rejected_rows;
}

std::optional<std::string> SemicolonFieldValue(std::string_view encoded, std::string_view key) {
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find(';', start);
    const std::string_view field =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = field.find('=');
    if (equals != std::string_view::npos && field.substr(0, equals) == key) {
      return std::string(field.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> SemicolonFieldU64(std::string_view encoded, std::string_view key) {
  const auto value = SemicolonFieldValue(encoded, key);
  if (!value || value->empty()) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : *value) {
    if (!std::isdigit(ch)) return std::nullopt;
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed;
}

std::vector<std::string> ImportRejectRecordsFromPayload(std::string_view payload) {
  std::vector<std::string> records;
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find('\n', start);
    const std::string_view line =
        payload.substr(start, end == std::string_view::npos ? payload.size() - start : end - start);
    if (line.starts_with("row[")) {
      const std::size_t equals = line.find('=');
      if (equals != std::string_view::npos) {
        const std::string_view row = line.substr(equals + 1);
        if (SemicolonFieldValue(row, "diagnostic_code").has_value()) {
          const auto sequence = static_cast<std::uint64_t>(records.size() + 1);
          std::ostringstream out;
          out << "{\"event\":\"reject_record\",\"sequence\":" << sequence
              << ",\"source_row_number\":"
              << SemicolonFieldU64(row, "source_row_number").value_or(sequence)
              << ",\"diagnostic_code\":\""
              << JsonEscape(SemicolonFieldValue(row, "diagnostic_code").value_or("COPY.ROW.REJECTED"))
              << "\",\"message_key\":\""
              << JsonEscape(SemicolonFieldValue(row, "message_key").value_or("copy.row.rejected"))
              << "\",\"diagnostic_detail\":\""
              << JsonEscape(SemicolonFieldValue(row, "diagnostic_detail").value_or(""))
              << "\",\"rejected_value_digest\":\""
              << JsonEscape(SemicolonFieldValue(row, "rejected_value_digest").value_or(""))
              << "\",\"value_redacted\":"
              << (SemicolonFieldValue(row, "value_redacted").value_or("true") == "false" ? "false" : "true")
              << "}";
          records.push_back(out.str());
        }
      }
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return records;
}

std::string BulkStreamEventJson(const ServerCursorRecord& cursor, std::uint64_t event_index) {
  std::ostringstream out;
  const auto accepted_rows = cursor.bulk_total_rows > cursor.bulk_rejected_rows
                                 ? cursor.bulk_total_rows - cursor.bulk_rejected_rows
                                 : 0;
  if (event_index == 0) {
    out << "{\"event\":\"progress\",\"sequence\":0,\"kind\":\""
        << JsonEscape(cursor.bulk_stream_kind) << "\",\"rows_processed\":"
        << cursor.bulk_total_rows << ",\"rows_rejected\":" << cursor.bulk_rejected_rows
        << ",\"percent_complete\":100}";
    return out.str();
  }
  if (event_index <= cursor.bulk_rejected_rows) {
    if (event_index - 1 < cursor.bulk_reject_records.size()) {
      return cursor.bulk_reject_records[static_cast<std::size_t>(event_index - 1)];
    }
    out << "{\"event\":\"reject_record\",\"sequence\":" << event_index
        << ",\"source_row_number\":" << event_index
        << ",\"diagnostic_code\":\"COPY.ROW.REJECTED\","
        << "\"message_key\":\"copy.row.rejected\","
        << "\"rejected_value_digest\":\"sha256:redacted-" << event_index << "\","
        << "\"value_redacted\":true}";
    return out.str();
  }
  if (event_index == cursor.bulk_rejected_rows + 1) {
    out << "{\"event\":\"bulk_summary\",\"sequence\":" << event_index
        << ",\"accepted_rows\":" << accepted_rows
        << ",\"rejected_rows\":" << cursor.bulk_rejected_rows
        << ",\"total_rows\":" << cursor.bulk_total_rows
        << ",\"reject_policy\":\"reject_row\"}";
    return out.str();
  }
  out << "{\"event\":\"final_status\",\"sequence\":" << event_index
      << ",\"status\":\""
      << (cursor.bulk_rejected_rows == 0 ? "completed" : "completed_with_rejects")
      << "\",\"final\":true}";
  return out.str();
}

std::string BulkStreamPacket(const ServerCursorRecord& cursor,
                             std::uint64_t first_event,
                             std::uint64_t event_count,
                             bool end_of_stream) {
  std::ostringstream out;
  out << "{\"copy_stream\":{\"kind\":\"" << JsonEscape(cursor.bulk_stream_kind) << "\","
      << "\"stream\":{\"chunk_start\":" << first_event
      << ",\"chunk_rows\":" << event_count
      << ",\"total_rows\":" << cursor.total_row_count
      << ",\"end_of_stream\":" << (end_of_stream ? "true" : "false") << "},"
      << "\"events\":[";
  for (std::uint64_t i = 0; i < event_count; ++i) {
    if (i != 0) out << ',';
    out << BulkStreamEventJson(cursor, first_event + i);
  }
  out << "]}}\n";
  return out.str();
}

std::uint64_t MultiResultEventCount(std::uint64_t result_sets) {
  return result_sets * 2 + 1;
}

std::string MultiResultEventJson(const ServerCursorRecord& cursor, std::uint64_t event_index) {
  std::ostringstream out;
  if (event_index < cursor.multi_result_count * 2) {
    const auto result_set = event_index / 2;
    if ((event_index % 2) == 0) {
      out << "{\"event\":\"result_set_metadata\",\"ordinal\":" << result_set
          << ",\"result_set_id\":\"rs-" << result_set
          << "\",\"columns\":" << CanonicalResultColumnsJson() << ","
          << "\"command_tag\":\"SELECT " << (result_set + 1) << "\"}";
      return out.str();
    }
    out << "{\"event\":\"command_tag\",\"ordinal\":" << result_set
        << ",\"tag\":\"SELECT " << (result_set + 1)
        << "\",\"rows_affected\":" << (result_set + 1) << "}";
    return out.str();
  }
  out << "{\"event\":\"multi_result_finality\",\"result_sets\":"
      << cursor.multi_result_count << ",\"status\":\"complete\",\"final\":true}";
  return out.str();
}

std::string MultiResultPacket(const ServerCursorRecord& cursor,
                              std::uint64_t first_event,
                              std::uint64_t event_count,
                              bool end_of_stream) {
  std::ostringstream out;
  out << "{\"multi_result\":{\"kind\":\"" << JsonEscape(cursor.multi_result_kind) << "\","
      << "\"stream\":{\"chunk_start\":" << first_event
      << ",\"chunk_rows\":" << event_count
      << ",\"total_rows\":" << cursor.total_row_count
      << ",\"end_of_stream\":" << (end_of_stream ? "true" : "false") << "},"
      << "\"events\":[";
  for (std::uint64_t i = 0; i < event_count; ++i) {
    if (i != 0) out << ',';
    out << MultiResultEventJson(cursor, first_event + i);
  }
  out << "]}}\n";
  return out.str();
}

std::uint64_t WarningStreamEventCount(std::uint64_t partial_rows, std::uint64_t warnings) {
  return partial_rows + warnings + 1;
}

std::string WarningStreamEventJson(const ServerCursorRecord& cursor, std::uint64_t event_index) {
  std::ostringstream out;
  if (event_index < cursor.partial_result_rows) {
    out << "{\"event\":\"partial_result_row\",\"row_index\":" << event_index
        << ",\"status\":\"visible\",\"partial_result\":true}";
    return out.str();
  }
  const auto warning_index = event_index - cursor.partial_result_rows;
  if (warning_index < cursor.warning_count) {
    out << "{\"event\":\"warning\",\"ordinal\":" << warning_index
        << ",\"diagnostic_code\":\"STREAM.WARNING." << warning_index
        << "\",\"severity\":\"WARNING\","
        << "\"message_key\":\"stream.warning.partial_result\","
        << "\"does_not_abort\":true}";
    return out.str();
  }
  out << "{\"event\":\"partial_result_finality\",\"status\":\"completed_with_warnings\","
      << "\"partial_result\":true,\"warnings\":" << cursor.warning_count
      << ",\"final\":true}";
  return out.str();
}

std::string WarningStreamPacket(const ServerCursorRecord& cursor,
                                std::uint64_t first_event,
                                std::uint64_t event_count,
                                bool end_of_stream) {
  std::ostringstream out;
  out << "{\"warning_stream\":{\"kind\":\"" << JsonEscape(cursor.warning_stream_kind) << "\","
      << "\"stream\":{\"chunk_start\":" << first_event
      << ",\"chunk_rows\":" << event_count
      << ",\"total_rows\":" << cursor.total_row_count
      << ",\"end_of_stream\":" << (end_of_stream ? "true" : "false") << "},"
      << "\"events\":[";
  for (std::uint64_t i = 0; i < event_count; ++i) {
    if (i != 0) out << ',';
    out << WarningStreamEventJson(cursor, first_event + i);
  }
  out << "]}}\n";
  return out.str();
}

struct PreparePayload {
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> client_statement_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_generation = 0;
  std::string encoded_sblr_envelope;
};

struct ExecutePayload {
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> prepared_statement_uuid{};
  bool cursor_requested = false;
  std::string encoded_sblr_envelope;
  std::vector<std::uint8_t> data_packet;
};

struct CursorPayload {
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::uint64_t max_rows = 1;
  std::uint64_t max_bytes = 0;
  std::uint32_t fetch_flags = 0;
};

std::optional<PreparePayload> DecodePreparePayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 16 + 16 + 8 + 8 + 8) return std::nullopt;
  std::size_t offset = 0;
  PreparePayload out;
  out.session_uuid = GetUuid(payload, offset); offset += 16;
  out.client_statement_uuid = GetUuid(payload, offset); offset += 16;
  out.catalog_generation = GetU64(payload, offset); offset += 8;
  out.security_epoch = GetU64(payload, offset); offset += 8;
  out.policy_generation = GetU64(payload, offset); offset += 8;
  if (!ReadString(payload, &offset, &out.encoded_sblr_envelope)) return std::nullopt;
  return out;
}

std::optional<ExecutePayload> DecodeExecutePayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 16 + 16 + 1) return std::nullopt;
  std::size_t offset = 0;
  ExecutePayload out;
  out.session_uuid = GetUuid(payload, offset); offset += 16;
  out.prepared_statement_uuid = GetUuid(payload, offset); offset += 16;
  out.cursor_requested = payload[offset++] != 0;
  if (!ReadString(payload, &offset, &out.encoded_sblr_envelope)) return std::nullopt;
  if (offset < payload.size() && !ReadBytes(payload, &offset, &out.data_packet)) {
    return std::nullopt;
  }
  if (offset != payload.size()) return std::nullopt;
  return out;
}

std::optional<CursorPayload> DecodeCursorPayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 32) return std::nullopt;
  CursorPayload out;
  out.session_uuid = GetUuid(payload, 0);
  out.cursor_uuid = GetUuid(payload, 16);
  if (payload.size() >= 40) {
    out.max_rows = GetU64(payload, 32);
  }
  if (payload.size() >= 48) {
    out.max_bytes = GetU64(payload, 40);
  }
  if (payload.size() >= 52) {
    out.fetch_flags = GetU32(payload, 48);
  }
  if (out.max_rows == 0) out.max_rows = 1;
  return out;
}

std::vector<std::uint8_t> EncodePrepareResult(const std::string& outcome,
                                              const std::array<std::uint8_t, 16>& prepared_uuid,
                                              const std::string& operation_id,
                                              const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, prepared_uuid);
  PutString(&out, operation_id);
  PutString(&out, detail);
  return out;
}

std::vector<std::uint8_t> EncodeExecuteResult(const std::string& outcome,
                                              const std::array<std::uint8_t, 16>& server_request_uuid,
                                              const std::array<std::uint8_t, 16>& cursor_uuid,
                                              std::uint64_t row_count,
                                              const std::string& operation_id,
                                              const std::string& row_packet,
                                              const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, server_request_uuid);
  PutUuid(&out, cursor_uuid);
  PutU64(&out, row_count);
  PutString(&out, operation_id);
  PutString(&out, row_packet);
  PutString(&out, detail);
  return out;
}

std::vector<std::uint8_t> EncodeFetchResult(const std::array<std::uint8_t, 16>& cursor_uuid,
                                            std::uint64_t row_count,
                                            const std::string& row_packet,
                                            bool end_of_cursor,
                                            const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, cursor_uuid);
  PutU64(&out, row_count);
  PutString(&out, row_packet);
  PutU8(&out, end_of_cursor ? 1 : 0);
  PutString(&out, detail);
  return out;
}

std::vector<std::uint8_t> EncodeCloseCursorResult(const std::string& outcome,
                                                  const std::array<std::uint8_t, 16>& cursor_uuid,
                                                  const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, cursor_uuid);
  PutString(&out, detail);
  return out;
}

std::optional<ServerSessionRecord> FindSession(const ServerSessionRegistry& registry,
                                               const std::array<std::uint8_t, 16>& session_uuid) {
  const auto found = registry.sessions_by_uuid.find(UuidBytesToText(session_uuid));
  if (found == registry.sessions_by_uuid.end()) return std::nullopt;
  return found->second;
}

ServerSessionRecord* FindMutableSession(ServerSessionRegistry* registry,
                                        const std::array<std::uint8_t, 16>& session_uuid) {
  if (registry == nullptr) return nullptr;
  const auto found = registry->sessions_by_uuid.find(UuidBytesToText(session_uuid));
  return found == registry->sessions_by_uuid.end() ? nullptr : &found->second;
}

ServerPreparedStatementRecord* FindPreparedByName(ServerSessionRegistry* registry,
                                                  const std::array<std::uint8_t, 16>& session_uuid,
                                                  std::string_view statement_name) {
  if (registry == nullptr || statement_name.empty()) return nullptr;
  for (auto& [_, prepared] : registry->prepared_by_uuid) {
    if (!prepared.closed && prepared.session_uuid == session_uuid &&
        prepared.statement_name == statement_name) {
      return &prepared;
    }
  }
  return nullptr;
}

ServerCursorRecord* FindCursorByName(ServerSessionRegistry* registry,
                                     const std::array<std::uint8_t, 16>& session_uuid,
                                     std::string_view cursor_name) {
  if (registry == nullptr || cursor_name.empty()) return nullptr;
  for (auto& [_, cursor] : registry->cursors_by_uuid) {
    if (!cursor.closed && cursor.session_uuid == session_uuid &&
        cursor.cursor_name == cursor_name) {
      return &cursor;
    }
  }
  return nullptr;
}

agents::WorkloadResourceVector JobResourceVector() {
  agents::WorkloadResourceVector resources;
  resources.memory_bytes = 1;
  resources.worker_slots = 1;
  resources.active_requests = 1;
  return resources;
}

agents::WorkloadQuotaLimits JobQuotaLimits() {
  agents::WorkloadQuotaLimits limits;
  limits.hard.memory_bytes = 1024;
  limits.hard.worker_slots = 8;
  limits.hard.active_requests = 8;
  limits.soft = limits.hard;
  limits.queue_on_soft_limit = false;
  limits.max_queued_requests = 0;
  return limits;
}

agents::WorkloadResourcePoolConfig BackgroundJobPoolConfig() {
  agents::WorkloadResourcePoolConfig config;
  config.pool_id = "background";
  config.workload_class = agents::WorkloadClass::background;
  config.limits = JobQuotaLimits();
  return config;
}

struct JobSchedulerContext {
  agents::DatabaseLocalBackgroundJobScheduler* scheduler = nullptr;
  agents::WorkloadResourceQuotaController* quota = nullptr;
  agents::AgentRuntimeStatus status = agents::AgentError("BACKGROUND_JOBS.NOT_INITIALIZED");
};

JobSchedulerContext EnsureJobScheduler(ServerSessionRegistry* registry,
                                       const ServerSessionRecord& session) {
  JobSchedulerContext context;
  if (registry == nullptr) {
    context.status = agents::AgentError("BACKGROUND_JOBS.REGISTRY_REQUIRED",
                                        "server_session_registry_required");
    return context;
  }
  if (session.database_uuid.empty()) {
    context.status = agents::AgentError("BACKGROUND_JOBS.DATABASE_SCOPE_REQUIRED",
                                        "database_uuid_required");
    return context;
  }

  auto& quota = registry->job_quotas_by_database_uuid[session.database_uuid];
  const auto pool_status = quota.RegisterPool(BackgroundJobPoolConfig());
  if (!pool_status.ok &&
      pool_status.diagnostic_code != "WORKLOAD_RESOURCE.POOL_ALREADY_REGISTERED") {
    context.status = pool_status;
    return context;
  }

  auto& scheduler = registry->job_schedulers_by_database_uuid[session.database_uuid];
  if (scheduler.state() == agents::BackgroundJobSchedulerState::not_started) {
    agents::BackgroundJobSchedulerStartup startup;
    startup.database_uuid = session.database_uuid;
    startup.policy_generation = std::max<std::uint64_t>(1, session.policy_generation);
    startup.tx2_activation_committed = true;
    startup.startup_admitted = true;
    startup.scheduler_catalog_visible = true;
    startup.cluster_authority_available = false;
    startup.monotonic_now_microseconds = 100;
    const auto start_status = scheduler.Start(startup);
    if (!start_status.ok) {
      context.status = start_status;
      return context;
    }
  }

  context.scheduler = &scheduler;
  context.quota = &quota;
  context.status = agents::AgentOk();
  return context;
}

std::uint64_t JobSchedulerNowMicroseconds(
    const agents::DatabaseLocalBackgroundJobScheduler& scheduler,
    const agents::WorkloadResourceQuotaController& quota) {
  return 100 + static_cast<std::uint64_t>(scheduler.evidence_log().size() +
                                         quota.evidence_log().size());
}

std::string JobSchedulerRouteDetail(
    const agents::DatabaseLocalBackgroundJobScheduler& scheduler,
    const agents::WorkloadResourceQuotaController& quota) {
  std::ostringstream out;
  out << "jobs_scheduler_route=database_local;active_reservations="
      << quota.ActiveReservationCount();
  if (!scheduler.evidence_log().empty()) {
    out << '\n' << agents::SerializeBackgroundJobEvidence(scheduler.evidence_log().back());
  }
  if (!quota.evidence_log().empty()) {
    out << agents::SerializeWorkloadQuotaEvidence(quota.evidence_log().back());
  }
  return out.str();
}

std::string SanitizeArchiveRouteSegment(std::string value) {
  for (char& ch : value) {
    const bool ok = std::isalnum(static_cast<unsigned char>(ch)) ||
                    ch == '_' || ch == '-' || ch == '.';
    if (!ok) ch = '_';
  }
  if (value.empty()) value = "archive_replication";
  return value;
}

std::string DerivedLifecycleDatabasePath(const ServerSessionRecord& session,
                                         std::string_view encoded) {
  const std::string explicit_path =
      JsonTextField(encoded, "database_path")
          .value_or(JsonTextField(encoded, "target_database_path").value_or(""));
  if (!explicit_path.empty()) {
    return explicit_path;
  }

  const std::string database_name =
      JsonTextField(encoded, "database_name")
          .value_or(JsonTextField(encoded, "target_database_name").value_or(""));
  if (database_name.empty()) {
    return {};
  }

  std::filesystem::path base =
      session.database_path.empty()
          ? std::filesystem::current_path()
          : std::filesystem::path(session.database_path).parent_path();
  std::string filename = SanitizeArchiveRouteSegment(database_name);
  if (!filename.ends_with(".sbdb")) {
    filename += ".sbdb";
  }
  return (base / filename).lexically_normal().string();
}

std::string DefaultArchiveManifestUri(const ServerSessionRecord& session,
                                      std::string_view archive_operation,
                                      std::string_view target_name) {
  const std::string base =
      session.database_path.empty() ? std::string("scratchbird_database")
                                    : session.database_path;
  std::string suffix = SanitizeArchiveRouteSegment(std::string(target_name));
  if (suffix == "database" || suffix.empty()) {
    suffix = SanitizeArchiveRouteSegment(std::string(archive_operation));
  }
  const bool backup = archive_operation == "logical_backup";
  return base + "." + suffix + (backup ? ".sblbak" : ".sbdelta");
}

engine_api::EngineRequestContext ArchiveReplicationEngineContext(
    const ServerSessionRecord& session,
    const std::array<std::uint8_t, 16>& request_uuid) {
  engine_api::EngineRequestContext context;
  context.trust_mode = session.embedded_in_process
                           ? engine_api::EngineTrustMode::embedded_in_process
                           : engine_api::EngineTrustMode::server_isolated;
  context.request_id = UuidBytesToText(request_uuid);
  context.database_path = session.database_path;
  context.database_uuid.canonical =
      session.database_uuid.empty() ? std::string("database:session")
                                    : session.database_uuid;
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  if (!sbps::IsZeroUuid(session.active_role_uuid)) {
    context.current_role_uuid.canonical = UuidBytesToText(session.active_role_uuid);
  }
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.statement_uuid.canonical = UuidBytesToText(request_uuid);
  context.local_transaction_id = session.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = session.default_transaction_isolation_level;
  context.transaction_timestamp = session.transaction_timestamp;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  PopulateEngineLanguageContextFromSession(session, &context.language_context);
  context.trace_tags = session.engine_authorization_trace_tags;
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

engine_api::EngineRequestContext PublicAbiDispatchEngineContext(
    const ServerSessionRecord& session) {
  engine_api::EngineRequestContext context =
      ArchiveReplicationEngineContext(session, session.session_uuid);
  context.trace_tags.push_back("sblr.public_abi.dispatch");
  return context;
}

void AddArchiveReplicationCommonOptions(engine_api::EngineApiRequest* request,
                                        std::string_view manifest_uri) {
  if (request == nullptr) return;
  request->option_envelopes.push_back("filespace_uuid:filespace:session");
  request->option_envelopes.push_back("authoritative_wal:false");
  request->option_envelopes.push_back("allow_inspect_only_archive:true");
  request->option_envelopes.push_back("engine_owned_path:true");
  if (!manifest_uri.empty()) {
    request->option_envelopes.push_back("target_uri:" + std::string(manifest_uri));
  }
}

void AddArchiveReplicationRestoreOptions(engine_api::EngineApiRequest* request,
                                         std::string_view manifest_uri,
                                         bool verify_only = false) {
  if (request == nullptr) return;
  request->option_envelopes.push_back("filespace_uuid:filespace:session");
  request->option_envelopes.push_back("authoritative_wal:false");
  request->option_envelopes.push_back("restore_inspection_open:true");
  request->option_envelopes.push_back("recovery_classification:restore_inspection");
  request->option_envelopes.push_back("target_database_open:false");
  request->option_envelopes.push_back("engine_owned_path:true");
  if (verify_only) {
    request->option_envelopes.push_back("restore_verify_only:true");
  }
  if (!manifest_uri.empty()) {
    request->option_envelopes.push_back("source_manifest_uri:" + std::string(manifest_uri));
  }
}

void AddArchiveReplicationDeltaOptions(engine_api::EngineApiRequest* request,
                                       std::string_view manifest_uri,
                                       std::string_view archive_operation,
                                       std::string_view target_name) {
  AddArchiveReplicationCommonOptions(request, manifest_uri);
  if (request == nullptr) return;
  request->option_envelopes.push_back("source_backup_uuid:backup:session");
  request->option_envelopes.push_back("start_transaction_id:0");
  request->option_envelopes.push_back("end_transaction_id:0");
  request->option_envelopes.push_back("restore_point_name:" + std::string(archive_operation));
  if (!target_name.empty()) {
    request->option_envelopes.push_back("archive_target_name:" + std::string(target_name));
  }
}

std::string ArchiveReplicationResultDetail(const engine_api::EngineApiResult& result,
                                           std::string_view manifest_uri,
                                           std::string_view archive_operation) {
  std::ostringstream out;
  out << "archive_replication_route=backup_archive_api;engine_operation="
      << result.operation_id << ";archive_operation=" << archive_operation
      << ";manifest_uri=" << manifest_uri
      << ";evidence_count=" << result.evidence.size();
  for (const auto& evidence : result.evidence) {
    out << '\n' << evidence.evidence_kind << '=' << evidence.evidence_id;
  }
  return out.str();
}

std::string ArchiveReplicationDiagnosticDetail(const engine_api::EngineApiResult& result) {
  if (result.diagnostics.empty()) return "backup_archive_api_rejected";
  const auto& diagnostic = result.diagnostics.front();
  return diagnostic.detail.empty() ? diagnostic.code : diagnostic.detail;
}

void CloseOpenCursorsByName(ServerSessionRegistry* registry,
                            const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view cursor_name) {
  if (registry == nullptr || cursor_name.empty()) return;
  for (auto& [_, cursor] : registry->cursors_by_uuid) {
    if (!cursor.closed && cursor.session_uuid == session_uuid &&
        cursor.cursor_name == cursor_name) {
      cursor.finality_state = "closed";
      cursor.finality_reason = "name_redeclared";
      cursor.closed = true;
    }
  }
}

std::string PreparedInnerEnvelopeFromControl(std::string encoded) {
  encoded = ReplaceJsonTextField(std::move(encoded), "operation_family", "sblr.query.relational.v3");
  encoded = ReplaceJsonTextField(std::move(encoded), "operation_id", "query.evaluate_projection");
  encoded = ReplaceJsonTextField(std::move(encoded), "engine_api_operation_id", "query.evaluate_projection");
  encoded = ReplaceJsonTextField(std::move(encoded), "sblr_operation", "SBLR_QUERY_EVALUATE_PROJECTION");
  return encoded;
}

std::string PreparedAuthorityExpectedDependencyUuid(
    const ServerPreparedStatementRecord& prepared) {
  if (!prepared.target_object_uuid.empty()) return prepared.target_object_uuid;
  return prepared.database_uuid.empty() ? UuidBytesToText(prepared.session_uuid)
                                        : prepared.database_uuid;
}

std::string PreparedAuthorityExpectedDependencyKind(
    const ServerPreparedStatementRecord& prepared) {
  if (!prepared.target_object_uuid.empty()) {
    return prepared.target_object_kind.empty() ? std::string("object")
                                               : prepared.target_object_kind;
  }
  return prepared.database_uuid.empty() ? "session" : "database";
}

std::string PreparedAuthorityExpectedDependencyOperation(
    const ServerPreparedStatementRecord& prepared) {
  return prepared.target_operation_id.empty() ? prepared.operation_id
                                             : prepared.target_operation_id;
}

std::string PreparedAuthorityExpectedDependencyColumnHash(
    const ServerPreparedStatementRecord& prepared) {
  return prepared.target_column_set_hash.empty() ? std::string("columns/all")
                                                 : prepared.target_column_set_hash;
}

void AppendPreparedAuthorityProofField(std::ostringstream* out,
                                       std::string_view key,
                                       std::string_view value) {
  if (out == nullptr) return;
  *out << key << '=' << value << '\n';
}

void AppendPreparedAuthorityProofField(std::ostringstream* out,
                                       std::string_view key,
                                       std::uint64_t value) {
  if (out == nullptr) return;
  *out << key << '=' << value << '\n';
}

std::string PreparedAuthorityProofPayload(const ServerPreparedStatementRecord& prepared,
                                          const ServerSessionRecord& session) {
  std::ostringstream out;
  const auto language = ServerLanguageContextForSession(session);
  AppendPreparedAuthorityProofField(&out, "version", "server.prepared.authority.v1");
  AppendPreparedAuthorityProofField(&out,
                                    "prepared_statement_uuid",
                                    UuidBytesToText(prepared.prepared_statement_uuid));
  AppendPreparedAuthorityProofField(&out,
                                    "client_statement_uuid",
                                    UuidBytesToText(prepared.client_statement_uuid));
  AppendPreparedAuthorityProofField(&out,
                                    "session_uuid",
                                    UuidBytesToText(session.session_uuid));
  AppendPreparedAuthorityProofField(&out,
                                    "auth_context_uuid",
                                    UuidBytesToText(session.auth_context_uuid));
  AppendPreparedAuthorityProofField(&out,
                                    "principal_uuid",
                                    UuidBytesToText(session.principal_uuid));
  AppendPreparedAuthorityProofField(&out,
                                    "effective_user_uuid",
                                    UuidBytesToText(session.effective_user_uuid));
  AppendPreparedAuthorityProofField(&out, "database_uuid", session.database_uuid);
  AppendPreparedAuthorityProofField(&out, "operation_family", prepared.operation_family);
  AppendPreparedAuthorityProofField(&out, "operation_id", prepared.operation_id);
  AppendPreparedAuthorityProofField(&out,
                                    "dependency_uuid",
                                    prepared.authority_dependency_uuid);
  AppendPreparedAuthorityProofField(&out,
                                    "dependency_kind",
                                    prepared.authority_dependency_kind);
  AppendPreparedAuthorityProofField(&out,
                                    "dependency_operation_id",
                                    prepared.authority_dependency_operation_id);
  AppendPreparedAuthorityProofField(&out,
                                    "dependency_column_set_hash",
                                    prepared.authority_dependency_column_set_hash);
  AppendPreparedAuthorityProofField(&out,
                                    "session_object_handle_id",
                                    prepared.session_object_handle_id);
  AppendPreparedAuthorityProofField(&out,
                                    "session_object_handle_generation",
                                    prepared.session_object_handle_generation);
  AppendPreparedAuthorityProofField(&out, "catalog_generation", session.catalog_generation);
  AppendPreparedAuthorityProofField(&out, "security_epoch", session.security_epoch);
  AppendPreparedAuthorityProofField(&out, "descriptor_epoch", session.descriptor_epoch);
  AppendPreparedAuthorityProofField(&out, "grant_epoch", session.grant_epoch);
  AppendPreparedAuthorityProofField(&out, "policy_generation", session.policy_generation);
  AppendPreparedAuthorityProofField(&out, "role_set_hash", session.role_set_hash);
  AppendPreparedAuthorityProofField(&out, "group_set_hash", session.group_set_hash);
  AppendPreparedAuthorityProofField(&out, "search_path_hash", session.search_path_hash);
  AppendPreparedAuthorityProofField(&out, "language_profile", language.language_profile_id);
  AppendPreparedAuthorityProofField(&out, "language_tag", language.language_tag);
  AppendPreparedAuthorityProofField(&out,
                                    "default_language_tag",
                                    language.default_language_tag);
  AppendPreparedAuthorityProofField(&out,
                                    "input_syntax_profile",
                                    language.input_syntax_profile);
  AppendPreparedAuthorityProofField(&out,
                                    "input_language_fallback_tag",
                                    language.input_language_fallback_tag);
  AppendPreparedAuthorityProofField(&out,
                                    "common_resource_hash",
                                    language.common_resource_hash);
  AppendPreparedAuthorityProofField(&out,
                                    "language_resource_epoch",
                                    language.language_resource_epoch);
  AppendPreparedAuthorityProofField(&out,
                                    "localized_name_epoch",
                                    language.localized_name_epoch);
  AppendPreparedAuthorityProofField(&out,
                                    "message_resource_epoch",
                                    language.message_resource_epoch);
  AppendPreparedAuthorityProofField(&out,
                                    "resource_compatibility_identity",
                                    language.resource_compatibility_identity);
  AppendPreparedAuthorityProofField(&out,
                                    "resource_version_identity",
                                    language.resource_version_identity);
  return out.str();
}

std::string PreparedAuthorityProofHash(const ServerPreparedStatementRecord& prepared,
                                       const ServerSessionRecord& session) {
  const std::string digest =
      engine_api::SecuritySha256Hex(PreparedAuthorityProofPayload(prepared, session));
  return digest.empty() ? std::string{} : "sha256:" + digest;
}

std::string PreparedExecutionStatementShapeHash(std::string_view encoded) {
  const std::string digest = engine_api::SecuritySha256Hex(std::string(encoded));
  return digest.empty() ? std::string{} : "sha256:" + digest;
}

void SealPreparedAuthorityProof(ServerPreparedStatementRecord* prepared,
                                const ServerSessionRecord& session) {
  if (prepared == nullptr) return;
  prepared->authority_dependency_uuid =
      PreparedAuthorityExpectedDependencyUuid(*prepared);
  prepared->authority_dependency_kind =
      PreparedAuthorityExpectedDependencyKind(*prepared);
  prepared->authority_dependency_operation_id =
      PreparedAuthorityExpectedDependencyOperation(*prepared);
  prepared->authority_dependency_column_set_hash =
      PreparedAuthorityExpectedDependencyColumnHash(*prepared);
  prepared->authority_proof_hash_algorithm = "sha256";
  prepared->authority_proof_hash = PreparedAuthorityProofHash(*prepared, session);
}

std::string PreparedStatementAuthorityMismatchReason(
    const ServerSessionRegistry& registry,
    const ServerPreparedStatementRecord& prepared,
    const ServerSessionRecord& session) {
  const auto language = ServerLanguageContextForSession(session);
  if (prepared.closed) return "prepared_statement_closed";
  if (prepared.session_uuid != session.session_uuid) {
    return "prepared_statement_cross_session";
  }
  if (prepared.database_uuid != session.database_uuid) {
    return "prepared_statement_cross_database";
  }
  if (prepared.auth_context_uuid != session.auth_context_uuid) {
    return "prepared_statement_security_context_stale";
  }
  if (prepared.principal_uuid != session.principal_uuid ||
      prepared.effective_user_uuid != session.effective_user_uuid) {
    return "prepared_statement_cross_user";
  }
  if (prepared.catalog_generation != session.catalog_generation) {
    return "prepared_statement_catalog_epoch_stale";
  }
  if (prepared.security_epoch != session.security_epoch) {
    return "prepared_statement_security_epoch_stale";
  }
  if (prepared.descriptor_epoch != session.descriptor_epoch) {
    return "prepared_statement_descriptor_epoch_stale";
  }
  if (prepared.grant_epoch != session.grant_epoch) {
    return "prepared_statement_grant_epoch_stale";
  }
  if (prepared.policy_generation != session.policy_generation) {
    return "prepared_statement_policy_epoch_stale";
  }
  if (prepared.role_set_hash != session.role_set_hash ||
      prepared.group_set_hash != session.group_set_hash ||
      prepared.search_path_hash != session.search_path_hash) {
    return "prepared_statement_authorization_context_stale";
  }
  if (prepared.authority_dependency_uuid.empty() ||
      prepared.authority_dependency_kind.empty() ||
      prepared.authority_dependency_operation_id.empty() ||
      prepared.authority_dependency_column_set_hash.empty() ||
      prepared.authority_proof_hash.empty()) {
    return "prepared_statement_authority_proof_missing";
  }
  if (prepared.authority_dependency_uuid !=
          PreparedAuthorityExpectedDependencyUuid(prepared) ||
      prepared.authority_dependency_kind !=
          PreparedAuthorityExpectedDependencyKind(prepared) ||
      prepared.authority_dependency_operation_id !=
          PreparedAuthorityExpectedDependencyOperation(prepared) ||
      prepared.authority_dependency_column_set_hash !=
          PreparedAuthorityExpectedDependencyColumnHash(prepared)) {
    return "prepared_statement_dependency_stale";
  }
  if (prepared.authority_proof_hash_algorithm != "sha256") {
    return "prepared_statement_authority_proof_stale";
  }
  const std::string expected_proof = PreparedAuthorityProofHash(prepared, session);
  if (expected_proof.empty() ||
      !engine_api::SecurityConstantTimeEqual(prepared.authority_proof_hash,
                                             expected_proof)) {
    return "prepared_statement_authority_hash_stale";
  }
  const auto context_it = registry.prepared_execution_contexts_by_uuid.find(
      UuidBytesToText(prepared.prepared_statement_uuid));
  if (context_it == registry.prepared_execution_contexts_by_uuid.end()) {
    return "prepared_statement_execution_context_missing";
  }
  const auto& execution_context = context_it->second;
  if (execution_context.grants_authority) {
    return "prepared_statement_execution_context_stale";
  }
  if (execution_context.session_uuid != session.session_uuid) {
    return "prepared_statement_cross_session";
  }
  if (execution_context.auth_context_uuid != session.auth_context_uuid) {
    return "prepared_statement_security_context_stale";
  }
  if (execution_context.principal_uuid != session.principal_uuid ||
      execution_context.effective_user_uuid != session.effective_user_uuid) {
    return "prepared_statement_cross_user";
  }
  if (execution_context.database_uuid != session.database_uuid) {
    return "prepared_statement_cross_database";
  }
  if (execution_context.operation_id != prepared.operation_id ||
      execution_context.target_object_uuid != prepared.target_object_uuid ||
      execution_context.authority_proof_hash != prepared.authority_proof_hash ||
      execution_context.statement_shape_hash !=
          PreparedExecutionStatementShapeHash(prepared.encoded_sblr_envelope)) {
    return "prepared_statement_execution_context_stale";
  }
  if (execution_context.epoch_vector.catalog_generation != session.catalog_generation ||
      execution_context.epoch_vector.security_epoch != session.security_epoch ||
      execution_context.epoch_vector.descriptor_epoch != session.descriptor_epoch ||
      execution_context.epoch_vector.grant_epoch != session.grant_epoch ||
      execution_context.epoch_vector.policy_generation != session.policy_generation ||
      execution_context.epoch_vector.capability_policy_generation !=
          session.capability_policy_generation ||
      execution_context.epoch_vector.cache_invalidation_epoch !=
          session.cache_invalidation_epoch ||
      execution_context.epoch_vector.name_resolution_epoch !=
          session.name_resolution_epoch ||
      execution_context.epoch_vector.resource_epoch != session.resource_epoch) {
    return "prepared_statement_execution_context_stale";
  }
  if (execution_context.epoch_vector.role_set_hash != session.role_set_hash ||
      execution_context.epoch_vector.group_set_hash != session.group_set_hash ||
      execution_context.epoch_vector.search_path_hash != session.search_path_hash) {
    return "prepared_statement_authorization_context_stale";
  }
  if (prepared.session_object_handle_id != 0) {
    const auto validation =
        ValidateSessionObjectHandle(registry,
                                    session,
                                    prepared.session_object_handle_id,
                                    prepared.session_object_handle_generation,
                                    prepared.target_object_uuid,
                                    prepared.target_operation_id,
                                    prepared.target_column_set_hash);
    if (!validation.accepted) {
      return validation.detail.empty()
                 ? "prepared_statement_object_handle_stale"
                 : "prepared_statement_object_handle_" + validation.detail;
    }
  }
  if (prepared.language_profile != language.language_profile_id ||
      prepared.language_tag != language.language_tag ||
      prepared.default_language_tag != language.default_language_tag ||
      prepared.input_syntax_profile != language.input_syntax_profile ||
      prepared.input_language_fallback_tag != language.input_language_fallback_tag ||
      prepared.common_resource_hash != language.common_resource_hash ||
      prepared.language_resource_epoch != language.language_resource_epoch ||
      prepared.localized_name_epoch != language.localized_name_epoch ||
      prepared.message_resource_epoch != language.message_resource_epoch ||
      prepared.resource_compatibility_identity != language.resource_compatibility_identity ||
      prepared.resource_version_identity != language.resource_version_identity) {
    return "prepared_statement_language_context_stale";
  }
  return {};
}

std::string PreparedStatementRefusalDetail(std::string_view mismatch) {
  if (mismatch == "prepared_statement_security_context_stale" ||
      mismatch == "prepared_statement_catalog_epoch_stale" ||
      mismatch == "prepared_statement_security_epoch_stale" ||
      mismatch == "prepared_statement_descriptor_epoch_stale" ||
      mismatch == "prepared_statement_grant_epoch_stale" ||
      mismatch == "prepared_statement_policy_epoch_stale" ||
      mismatch == "prepared_statement_authorization_context_stale" ||
      mismatch == "prepared_statement_authority_proof_missing" ||
      mismatch == "prepared_statement_authority_proof_stale" ||
      mismatch == "prepared_statement_authority_hash_stale" ||
      mismatch == "prepared_statement_dependency_stale" ||
      mismatch == "prepared_statement_execution_context_missing" ||
      mismatch == "prepared_statement_execution_context_stale" ||
      mismatch == "prepared_statement_language_context_stale" ||
      mismatch.starts_with("prepared_statement_object_handle_")) {
    return "prepared_statement_epoch_stale";
  }
  return std::string(mismatch);
}

void CapturePreparedAuthorityContext(ServerPreparedStatementRecord* prepared,
                                     const ServerSessionRecord& session) {
  if (prepared == nullptr) return;
  prepared->session_uuid = session.session_uuid;
  prepared->auth_context_uuid = session.auth_context_uuid;
  prepared->principal_uuid = session.principal_uuid;
  prepared->effective_user_uuid = session.effective_user_uuid;
  prepared->database_uuid = session.database_uuid;
}

void CapturePreparedLanguageContext(ServerPreparedStatementRecord* prepared,
                                    const ServerSessionRecord& session) {
  if (prepared == nullptr) return;
  const auto language = ServerLanguageContextForSession(session);
  prepared->language_profile = language.language_profile_id;
  prepared->language_tag = language.language_tag;
  prepared->default_language_tag = language.default_language_tag;
  prepared->input_syntax_profile = language.input_syntax_profile;
  prepared->input_language_fallback_tag = language.input_language_fallback_tag;
  prepared->common_resource_hash = language.common_resource_hash;
  prepared->language_resource_epoch = language.language_resource_epoch;
  prepared->localized_name_epoch = language.localized_name_epoch;
  prepared->message_resource_epoch = language.message_resource_epoch;
  prepared->resource_compatibility_identity =
      language.resource_compatibility_identity;
  prepared->resource_version_identity = language.resource_version_identity;
}

std::optional<std::string> TextLineValue(std::string_view encoded, std::string_view key);
std::string EncodedTextField(std::string_view encoded, const std::string& field);
bool TextBoolField(std::string_view encoded,
                   std::string_view key,
                   bool default_value);

void BindPreparedSessionObjectHandle(ServerSessionRegistry* registry,
                                     ServerPreparedStatementRecord* prepared,
                                     const ServerSessionRecord& session,
                                     std::string_view encoded,
                                     std::string_view operation_id) {
  if (registry == nullptr || prepared == nullptr || operation_id.empty()) return;
  std::string object_uuid =
      TextLineValue(encoded, "target_object_uuid")
          .value_or(JsonTextField(encoded, "target_object_uuid").value_or(""));
  if (object_uuid.empty()) {
    object_uuid =
        TextLineValue(encoded, "object_uuid")
            .value_or(JsonTextField(encoded, "object_uuid").value_or(""));
  }
  if (object_uuid.empty()) return;
  std::string object_kind =
      TextLineValue(encoded, "target_object_kind")
          .value_or(JsonTextField(encoded, "target_object_kind").value_or("object"));
  std::string column_set_hash =
      TextLineValue(encoded, "column_set_hash")
          .value_or(JsonTextField(encoded, "column_set_hash").value_or("columns/all"));
  auto handle = AllocateSessionObjectHandle(registry,
                                            session,
                                            object_uuid,
                                            object_kind,
                                            std::string(operation_id),
                                            column_set_hash);
  prepared->session_object_handle_id = handle.handle_id;
  prepared->session_object_handle_generation = handle.generation;
  prepared->target_object_uuid = std::move(handle.object_uuid);
  prepared->target_object_kind = std::move(handle.object_kind);
  prepared->target_operation_id = std::move(handle.operation_id);
  prepared->target_column_set_hash = std::move(handle.column_set_hash);
}

ServerPreparedExecutionContextRecord BuildPreparedExecutionContext(
    const ServerPreparedStatementRecord& prepared,
    const ServerSessionRecord& session) {
  ServerPreparedExecutionContextRecord context;
  context.prepared_statement_uuid = prepared.prepared_statement_uuid;
  context.session_uuid = prepared.session_uuid;
  context.auth_context_uuid = prepared.auth_context_uuid;
  context.principal_uuid = prepared.principal_uuid;
  context.effective_user_uuid = prepared.effective_user_uuid;
  context.database_uuid = prepared.database_uuid;
  context.operation_id = prepared.operation_id;
  context.target_object_uuid = prepared.target_object_uuid;
  context.statement_shape_hash =
      PreparedExecutionStatementShapeHash(prepared.encoded_sblr_envelope);
  context.authority_proof_hash = prepared.authority_proof_hash;
  context.epoch_vector.catalog_generation = prepared.catalog_generation;
  context.epoch_vector.security_epoch = prepared.security_epoch;
  context.epoch_vector.descriptor_epoch = prepared.descriptor_epoch;
  context.epoch_vector.grant_epoch = prepared.grant_epoch;
  context.epoch_vector.policy_generation = prepared.policy_generation;
  context.epoch_vector.capability_policy_generation =
      session.capability_policy_generation;
  context.epoch_vector.cache_invalidation_epoch = session.cache_invalidation_epoch;
  context.epoch_vector.name_resolution_epoch = session.name_resolution_epoch;
  context.epoch_vector.resource_epoch = session.resource_epoch;
  context.epoch_vector.role_set_hash = prepared.role_set_hash;
  context.epoch_vector.group_set_hash = prepared.group_set_hash;
  context.epoch_vector.search_path_hash = prepared.search_path_hash;
  context.grants_authority = false;
  return context;
}

void StorePreparedExecutionContext(ServerSessionRegistry* registry,
                                   const ServerPreparedStatementRecord& prepared,
                                   const ServerSessionRecord& session) {
  if (registry == nullptr) return;
  registry->prepared_execution_contexts_by_uuid
      [UuidBytesToText(prepared.prepared_statement_uuid)] =
          BuildPreparedExecutionContext(prepared, session);
}

void BumpSessionLanguageResourceEpochs(ServerSessionRecord* session) {
  if (session == nullptr) return;
  session->language_resource_epoch =
      session->language_resource_epoch == 0 ? 1 : session->language_resource_epoch + 1;
  session->localized_name_epoch =
      session->localized_name_epoch == 0 ? 1 : session->localized_name_epoch + 1;
  session->message_resource_epoch =
      session->message_resource_epoch == 0 ? 1 : session->message_resource_epoch + 1;
  session->resource_epoch =
      session->resource_epoch == 0 ? session->language_resource_epoch : session->resource_epoch + 1;
  session->name_resolution_epoch =
      session->name_resolution_epoch == 0
          ? session->localized_name_epoch
          : session->name_resolution_epoch + 1;
}

bool PreservesStablePublicRelationNameCache(std::string_view operation_id) {
  return operation_id == "ddl.create_index" ||
         operation_id == "ddl.create_index_template" ||
         operation_id == "ddl.create_statistics";
}

bool PreservesQualifiedStablePublicRelationNameCache(std::string_view operation_id) {
  return operation_id == "ddl.create_schema" ||
         operation_id == "ddl.create_table" ||
         operation_id == "ddl.create_view";
}

bool MutatesPublicNameResolutionAuthority(std::string_view operation_id) {
  return operation_id.starts_with("ddl.") ||
         operation_id.starts_with("catalog.") ||
         operation_id.starts_with("security.") ||
         operation_id.starts_with("policy.") ||
         operation_id.starts_with("grant.") ||
         operation_id.starts_with("role.") ||
         operation_id.starts_with("group.") ||
         operation_id.starts_with("auth.") ||
         operation_id.starts_with("language.") ||
         operation_id.starts_with("session.search_path.") ||
         operation_id.starts_with("session.set_role") ||
         operation_id.starts_with("session.set_group");
}

void ClearDispatchSchemaParentPathCache();

void ClearStablePublicRelationNameCacheForMutation(ServerSessionRegistry* registry,
                                                   std::string_view operation_id) {
  if (registry == nullptr) return;
  if (!MutatesPublicNameResolutionAuthority(operation_id)) return;
  if (operation_id != "ddl.create_table" &&
      operation_id != "ddl.create_view" &&
      operation_id != "ddl.create_index" &&
      operation_id != "ddl.create_index_template" &&
      operation_id != "ddl.create_statistics") {
    ClearDispatchSchemaParentPathCache();
  }
  if (PreservesStablePublicRelationNameCache(operation_id)) return;
  if (PreservesQualifiedStablePublicRelationNameCache(operation_id)) {
    for (auto it = registry->stable_public_name_resolution_cache_by_key.begin();
         it != registry->stable_public_name_resolution_cache_by_key.end();) {
      if (it->second.search_path_hash == "<qualified>") {
        ++it;
      } else {
        registry->stable_public_name_resolution_cache_lru.erase(
            std::remove(registry->stable_public_name_resolution_cache_lru.begin(),
                        registry->stable_public_name_resolution_cache_lru.end(),
                        it->first),
            registry->stable_public_name_resolution_cache_lru.end());
        it = registry->stable_public_name_resolution_cache_by_key.erase(it);
      }
    }
    return;
  }
  registry->stable_public_name_resolution_cache_by_key.clear();
  registry->stable_public_name_resolution_cache_lru.clear();
}

std::optional<std::string> PipeDelimitedField(std::string_view row_packet,
                                              std::size_t ordinal) {
  const std::size_t line_end = row_packet.find('\n');
  std::string_view line =
      row_packet.substr(0, line_end == std::string_view::npos ? row_packet.size() : line_end);
  std::size_t start = 0;
  for (std::size_t index = 0; index <= ordinal; ++index) {
    const std::size_t end = line.find('|', start);
    if (index == ordinal) {
      return std::string(line.substr(start,
                                     end == std::string_view::npos ? line.size() - start
                                                                   : end - start));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<std::string> ServerApiPayloadRowField(std::string_view payload,
                                                    std::size_t row_index,
                                                    std::string_view field_name) {
  const std::string prefix = "row[" + std::to_string(row_index) + "]=";
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find('\n', start);
    const std::string_view line =
        payload.substr(start, end == std::string_view::npos ? payload.size() - start : end - start);
    if (line.starts_with(prefix)) {
      return SemicolonFieldValue(line.substr(prefix.size()), field_name);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

bool LooksLikeUuidText(std::string_view text) {
  std::size_t hex_count = 0;
  for (const char ch : text) {
    if (ch == '-') continue;
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    ++hex_count;
  }
  return hex_count == 32;
}

std::string NormalizeUuidText(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const unsigned char ch : text) {
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  return normalized;
}

std::string StablePublicRelationNameSeedKey(const ServerSessionRecord& session,
                                            std::string_view presented_name,
                                            std::string_view object_class) {
  const bool qualified = presented_name.find('.') != std::string_view::npos;
  const std::string_view stable_search_path_hash =
      qualified ? std::string_view("<qualified>") : std::string_view(session.search_path_hash);
  std::ostringstream key;
  key << "stable_relation_v1"
      << "|db=" << session.database_uuid
      << "|user=" << UuidBytesToText(session.effective_user_uuid)
      << "|presented=" << presented_name
      << "|quoted=0"
      << "|class=" << object_class
      << "|dialect=sbsql_v3"
      << "|identifier_profile=sbsql_v3"
      << "|request_language="
      << (session.default_language_tag.empty() ? std::string_view("en")
                                               : std::string_view(session.default_language_tag))
      << "|request_search_path=" << (qualified ? std::string_view("<qualified>")
                                               : std::string_view("sys,public"))
      << "|security=" << session.security_epoch
      << "|grant=" << session.grant_epoch
      << "|policy=" << session.policy_generation
      << "|role_hash=" << session.role_set_hash
      << "|group_hash=" << session.group_set_hash
      << "|search_path_hash=" << stable_search_path_hash
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|language_resource=" << session.language_resource_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

void StoreStablePublicRelationNameSeed(ServerSessionRegistry* registry,
                                       const ServerSessionRecord& session,
                                       std::string_view presented_name,
                                       std::string_view object_uuid,
                                       std::string_view object_class) {
  if (registry == nullptr || presented_name.empty() || object_uuid.empty() ||
      object_class.empty()) {
    return;
  }
  constexpr std::size_t kMaxServerStablePublicNameResolutionCacheEntries = 8192;
  ServerPublicNameResolutionCacheRecord record;
  record.cache_key =
      StablePublicRelationNameSeedKey(session, presented_name, object_class);
  record.effective_user_uuid = session.effective_user_uuid;
  record.database_uuid = session.database_uuid;
  record.object_uuid = std::string(object_uuid);
  record.canonical_name = std::string(presented_name);
  record.object_class = std::string(object_class);
  record.catalog_generation = session.catalog_generation;
  record.security_epoch = session.security_epoch;
  record.descriptor_epoch = session.descriptor_epoch;
  record.grant_epoch = session.grant_epoch;
  record.policy_generation = session.policy_generation;
  record.name_resolution_epoch = session.name_resolution_epoch;
  record.language_resource_epoch = session.language_resource_epoch;
  record.localized_name_epoch = session.localized_name_epoch;
  record.message_resource_epoch = session.message_resource_epoch;
  record.role_set_hash = session.role_set_hash;
  record.group_set_hash = session.group_set_hash;
  record.search_path_hash =
      presented_name.find('.') != std::string_view::npos ? "<qualified>"
                                                         : session.search_path_hash;
  record.language_profile = session.language_profile;
  record.language_tag = session.language_tag;
  record.input_syntax_profile = session.input_syntax_profile;
  record.input_language_fallback_tag = session.input_language_fallback_tag;
  record.common_resource_hash = session.common_resource_hash;
  record.resource_compatibility_identity = session.resource_compatibility_identity;
  record.resource_version_identity = session.resource_version_identity;
  record.generation = registry->next_public_name_resolution_cache_generation++;
  const std::string cache_key = record.cache_key;
  registry->stable_public_name_resolution_cache_by_key[cache_key] = std::move(record);
  if (const char* trace_path = std::getenv("SCRATCHBIRD_PUBLIC_NAME_RESOLUTION_TRACE_FILE");
      trace_path != nullptr && *trace_path != '\0') {
    std::ofstream out(trace_path, std::ios::app | std::ios::binary);
    if (out) {
      out << "layer=server_public_name_resolution_seed"
          << "\tpresented=" << ServerTraceField(presented_name)
          << "\tclass=" << ServerTraceField(object_class)
          << "\tobject_uuid=" << ServerTraceField(object_uuid)
          << "\tstable_cache_key=" << ServerTraceField(cache_key)
          << "\tstable_cache_entries="
          << registry->stable_public_name_resolution_cache_by_key.size()
          << "\tdatabase_uuid=" << ServerTraceField(session.database_uuid)
          << "\tuser_uuid=" << UuidBytesToText(session.effective_user_uuid)
          << "\tsearch_path_hash=" << ServerTraceField(session.search_path_hash)
          << "\tlanguage_tag=" << ServerTraceField(session.language_tag)
          << "\tdefault_language_tag=" << ServerTraceField(session.default_language_tag)
          << '\n';
    }
  }
  registry->stable_public_name_resolution_cache_lru.erase(
      std::remove(registry->stable_public_name_resolution_cache_lru.begin(),
                  registry->stable_public_name_resolution_cache_lru.end(),
                  cache_key),
      registry->stable_public_name_resolution_cache_lru.end());
  registry->stable_public_name_resolution_cache_lru.push_back(cache_key);
  while (registry->stable_public_name_resolution_cache_by_key.size() >
             kMaxServerStablePublicNameResolutionCacheEntries &&
         !registry->stable_public_name_resolution_cache_lru.empty()) {
    registry->stable_public_name_resolution_cache_by_key.erase(
        registry->stable_public_name_resolution_cache_lru.front());
    registry->stable_public_name_resolution_cache_lru.pop_front();
  }
}

void SeedStablePublicRelationNameCacheAfterDdl(ServerSessionRegistry* registry,
                                               const ServerSessionRecord& session,
                                               std::string_view operation_id,
                                               std::string_view encoded,
                                               std::string_view row_packet) {
  auto trace_skip = [&](std::string_view reason,
                        std::string_view object_uuid,
                        std::string_view object_name,
                        std::string_view schema_parent_path) {
    if (const char* trace_path = std::getenv("SCRATCHBIRD_PUBLIC_NAME_RESOLUTION_TRACE_FILE");
        trace_path != nullptr && *trace_path != '\0') {
      std::ofstream out(trace_path, std::ios::app | std::ios::binary);
      if (out) {
        out << "layer=server_public_name_resolution_seed_skip"
            << "\toperation=" << ServerTraceField(operation_id)
            << "\treason=" << ServerTraceField(reason)
            << "\tobject_uuid=" << ServerTraceField(object_uuid)
            << "\tobject_name=" << ServerTraceField(object_name)
            << "\tschema_parent_path=" << ServerTraceField(schema_parent_path)
            << "\tencoded_bytes=" << encoded.size()
            << "\trow_packet_bytes=" << row_packet.size()
            << '\n';
      }
    }
  };
  if (registry == nullptr) return;
  auto seed_field_value = [&](std::string_view field) -> std::string {
    return ExistingTextOperandValue(encoded, field).value_or(
        TextLineValue(encoded, field).value_or(
            JsonTextField(encoded, field).value_or("")));
  };
  auto seed_security_name = [&](std::string_view uuid_field,
                                std::string_view name_field,
                                const std::vector<std::string_view>& classes) {
    std::string object_uuid = seed_field_value(uuid_field);
    if (object_uuid.empty()) object_uuid = seed_field_value("principal_uuid");
    if (object_uuid.empty()) object_uuid = seed_field_value("target_object_uuid");
    if (object_uuid.empty()) {
      object_uuid = ServerApiPayloadRowField(row_packet, 0, uuid_field).value_or("");
    }
    if (object_uuid.empty()) {
      object_uuid = ServerApiPayloadRowField(row_packet, 0, "object_uuid").value_or("");
    }
    if (IsPublicRegistryPlaceholder(object_uuid)) object_uuid.clear();
    if (!object_uuid.empty() && !LooksLikeUuidText(object_uuid)) object_uuid.clear();
    std::string object_name = seed_field_value(name_field);
    if (object_name.empty()) {
      object_name = ServerApiPayloadRowField(row_packet, 0, name_field).value_or("");
    }
    if (object_name.empty()) {
      object_name = ServerApiPayloadRowField(row_packet, 0, "name").value_or("");
    }
    if (object_uuid.empty() || object_name.empty()) {
      trace_skip("missing_security_seed_field", object_uuid, object_name, "");
      return;
    }
    for (const auto object_class : classes) {
      StoreStablePublicRelationNameSeed(registry,
                                        session,
                                        object_name,
                                        object_uuid,
                                        object_class);
    }
  };
  if (operation_id == "security.role.create") {
    seed_security_name("role_uuid", "role_name", {"role", "security_role", "principal"});
    return;
  }
  if (operation_id == "security.group.create") {
    seed_security_name("group_uuid", "group_name", {"group", "security_group", "principal"});
    return;
  }
  if (operation_id == "security.policy.create") {
    std::vector<std::string_view> classes{"policy", "security_policy"};
    const std::string effect = seed_field_value("policy_effect");
    std::string effect_upper = effect;
    std::transform(effect_upper.begin(),
                   effect_upper.end(),
                   effect_upper.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::toupper(ch));
                   });
    if (effect_upper.find("MASK") != std::string::npos) classes.push_back("mask");
    if (effect_upper.find("RLS") != std::string::npos ||
        effect_upper.find("ROW") != std::string::npos) {
      classes.push_back("rls");
    }
    seed_security_name("policy_uuid", "policy_name", classes);
    return;
  }
  if (operation_id != "ddl.create_table" && operation_id != "ddl.create_view") return;
  const bool temporary =
      JsonBoolField(encoded, "temporary_table", false) ||
      JsonBoolField(encoded, "temporary", false) ||
      TextBoolField(encoded, "temporary_table", false) ||
      TextBoolField(encoded, "temporary", false);
  if (temporary) {
    trace_skip("temporary_relation", "", "", "");
    return;
  }
  const std::string object_kind =
      operation_id == "ddl.create_view" ? "view" : "table";
  std::string object_uuid = seed_field_value(object_kind + std::string("_object_uuid"));
  if (object_uuid.empty()) object_uuid = seed_field_value("target_object_uuid");
  if (IsPublicRegistryPlaceholder(object_uuid)) object_uuid.clear();
  if (!object_uuid.empty() && !LooksLikeUuidText(object_uuid)) object_uuid.clear();
  if (object_uuid.empty()) {
    object_uuid = ServerApiPayloadRowField(row_packet, 0, "object_uuid").value_or("");
  }
  if (object_uuid.empty()) object_uuid = PipeDelimitedField(row_packet, 0).value_or("");
  if (!object_uuid.empty() && !LooksLikeUuidText(object_uuid)) object_uuid.clear();
  std::string object_name = seed_field_value(object_kind + std::string("_name"));
  if (object_name.empty()) object_name = seed_field_value("name");
  if (object_name.empty()) {
    object_name = ServerApiPayloadRowField(row_packet, 0, "object_name").value_or("");
  }
  if (object_name.empty()) {
    object_name = ServerApiPayloadRowField(row_packet, 0, "name").value_or("");
  }
  if (object_name.empty()) object_name = PipeDelimitedField(row_packet, 3).value_or("");
  std::string schema_parent_path = seed_field_value("schema_parent_path");
  if (object_uuid.empty() || object_name.empty() || schema_parent_path.empty()) {
    trace_skip("missing_seed_field", object_uuid, object_name, schema_parent_path);
    return;
  }
  const std::string presented_name = schema_parent_path + "." + object_name;
  StoreStablePublicRelationNameSeed(registry,
                                    session,
                                    presented_name,
                                    object_uuid,
                                    object_kind);
  StoreStablePublicRelationNameSeed(registry,
                                    session,
                                    presented_name,
                                    object_uuid,
                                    "relation");
}

std::string LanguageSessionContextPacket(std::string_view operation_id,
                                         const ServerSessionRecord& session,
                                         std::string_view outcome,
                                         bool mutated) {
  const auto language = ServerLanguageContextForSession(session);
  std::ostringstream out;
  out << "operation_id=" << operation_id << "\n"
      << "result_kind=language.session_context.v1\n"
      << "outcome=" << outcome << "\n"
      << "mutated_session_language=" << (mutated ? "true" : "false") << "\n"
      << "server_session_language_context_authority=true\n"
      << "parser_updates_session_language=false\n"
      << "prepared_statement_reinterpretation=false\n"
      << "language_profile_id=" << language.language_profile_id << "\n"
      << "language_tag=" << language.language_tag << "\n"
      << "default_language_tag=" << language.default_language_tag << "\n"
      << "input_syntax_profile=" << language.input_syntax_profile << "\n"
      << "input_language_fallback_tag=" << language.input_language_fallback_tag << "\n"
      << "common_resource_hash=" << language.common_resource_hash << "\n"
      << "language_resource_epoch=" << language.language_resource_epoch << "\n"
      << "localized_name_epoch=" << language.localized_name_epoch << "\n"
      << "message_resource_epoch=" << language.message_resource_epoch << "\n"
      << "resource_compatibility_identity="
      << language.resource_compatibility_identity << "\n"
      << "resource_version_identity=" << language.resource_version_identity << "\n";
  return out.str();
}

SessionOperationResult LanguageSessionControlResult(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::string& operation_id,
    const std::string& row_packet,
    std::uint64_t row_count,
    std::string detail) {
  UpdateServerRequestLifecycleOperation(registry, request_uuid, operation_id);
  CompleteServerRequestLifecycle(registry,
                                 request_uuid,
                                 ServerRequestLifecycleState::kCompleted,
                                 detail.empty() ? "language_session_control_completed"
                                                : detail);
  SessionOperationResult result;
  result.accepted = true;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
  result.response_schema_id = kSchemaExecuteResultTestV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = session_uuid;
  result.payload = EncodeExecuteResult("accepted",
                                       request_uuid,
                                       {},
                                       row_count,
                                       operation_id,
                                       row_packet,
                                       std::move(detail));
  return result;
}

bool IsLanguageSessionOperation(std::string_view operation_id) {
  return operation_id == "language.session.set" ||
         operation_id == "language.session.reset" ||
         operation_id == "language.session.show";
}

bool IsLanguageBundleOperation(std::string_view operation_id) {
  return operation_id == "language.bundle.load" ||
         operation_id == "language.bundle.unload" ||
         operation_id == "language.bundle.validate";
}

bool IsLanguageResourceDirectoryOperation(std::string_view operation_id) {
  return operation_id == "language.resource_directory.scan" ||
         operation_id == "language.resource_directory.reload" ||
         operation_id == "language.resource_directory.show";
}

bool LanguageBundleManifestAdmitted(std::string_view encoded) {
  return JsonBoolField(encoded, "admitted_bundle_manifest_attached", false) &&
         JsonBoolField(encoded, "bundle_signature_verified", false) &&
         JsonBoolField(encoded, "bundle_security_admitted", false) &&
         JsonBoolField(encoded, "bundle_compatible_with_server", false) &&
         JsonBoolField(encoded, "bundle_provenance_verified", false);
}

ServerLanguageBundleRecord LanguageBundleRecordFromEnvelope(
    std::string_view encoded,
    const ServerSessionRecord& session) {
  ServerLanguageBundleRecord record;
  record.bundle_uuid = JsonTextField(encoded, "bundle_uuid").value_or("");
  record.language_profile_id =
      JsonTextField(encoded, "language_profile_id")
          .value_or(JsonTextField(encoded, "target_language_profile")
                        .value_or(JsonTextField(encoded, "profile_uuid").value_or("")));
  record.language_tag =
      JsonTextField(encoded, "language_tag")
          .value_or(JsonTextField(encoded, "exact_tag").value_or(""));
  record.dialect_profile_uuid =
      JsonTextField(encoded, "dialect_profile_uuid").value_or("");
  record.topology_profile_uuid =
      JsonTextField(encoded, "topology_profile_uuid").value_or("");
  record.common_resource_hash =
      JsonTextField(encoded, "common_resource_hash")
          .value_or(session.common_resource_hash);
  record.resource_hash =
      JsonTextField(encoded, "resource_hash").value_or(record.common_resource_hash);
  record.required_profile = JsonBoolField(encoded, "required_profile", false);
  record.loaded = true;
  return record;
}

bool LanguageBundleRecordIsComplete(const ServerLanguageBundleRecord& record) {
  return !record.bundle_uuid.empty() &&
         !record.language_profile_id.empty() &&
         !record.language_tag.empty() &&
         !record.common_resource_hash.empty() &&
         !record.resource_hash.empty();
}

std::string LanguageBundleRegistryPacket(std::string_view operation_id,
                                         const ServerLanguageBundleRecord& record,
                                         std::string_view outcome,
                                         bool mutated) {
  std::ostringstream out;
  out << "operation_id=" << operation_id << "\n"
      << "result_kind=language.bundle_registry.v1\n"
      << "outcome=" << outcome << "\n"
      << "mutated_language_bundle_registry=" << (mutated ? "true" : "false") << "\n"
      << "server_language_resource_registry_authority=true\n"
      << "parser_language_library_admission=false\n"
      << "load_or_unload_effects_executed_by_parser=false\n"
      << "row_storage_touched=false\n"
      << "mga_finality_claimed=false\n"
      << "bundle_uuid=" << record.bundle_uuid << "\n"
      << "language_profile_id=" << record.language_profile_id << "\n"
      << "language_tag=" << record.language_tag << "\n"
      << "dialect_profile_uuid=" << record.dialect_profile_uuid << "\n"
      << "topology_profile_uuid=" << record.topology_profile_uuid << "\n"
      << "common_resource_hash=" << record.common_resource_hash << "\n"
      << "resource_hash=" << record.resource_hash << "\n"
      << "loaded=" << (record.loaded ? "true" : "false") << "\n"
      << "required_profile=" << (record.required_profile ? "true" : "false") << "\n"
      << "language_resource_epoch=" << record.language_resource_epoch << "\n";
  return out.str();
}

bool LanguageResourceDirectoryManifestAdmitted(std::string_view encoded) {
  return JsonBoolField(encoded, "language_resource_directory_manifest_attached", false) &&
         JsonBoolField(encoded, "language_resource_directory_signature_verified", false) &&
         JsonBoolField(encoded, "language_resource_directory_security_admitted", false) &&
         JsonBoolField(encoded, "language_resource_directory_compatible", false);
}

ServerLanguageResourceDirectoryRecord LanguageResourceDirectoryRecordFromEnvelope(
    std::string_view encoded) {
  ServerLanguageResourceDirectoryRecord record;
  record.directory_id =
      JsonTextField(encoded, "directory_id")
          .value_or(JsonTextField(encoded, "language_resource_directory_id").value_or(""));
  record.directory_path =
      JsonTextField(encoded, "directory_path")
          .value_or(JsonTextField(encoded, "resource_directory_path").value_or(""));
  record.manifest_hash =
      JsonTextField(encoded, "manifest_hash")
          .value_or(JsonTextField(encoded, "language_resource_manifest_hash").value_or(""));
  record.signing_key_id =
      JsonTextField(encoded, "signing_key_id")
          .value_or(JsonTextField(encoded, "language_resource_signing_key_id").value_or(""));
  record.scan_evidence_id =
      JsonTextField(encoded, "scan_evidence_id")
          .value_or(JsonTextField(encoded, "language_resource_scan_evidence_id").value_or(""));
  record.audit_reason =
      JsonTextField(encoded, "audit_reason")
          .value_or(JsonTextField(encoded, "language_resource_audit_reason").value_or(""));
  record.signed_manifest_verified =
      JsonBoolField(encoded, "language_resource_directory_signature_verified", false);
  record.admitted_by_security_policy =
      JsonBoolField(encoded, "language_resource_directory_security_admitted", false);
  record.compatible_with_server =
      JsonBoolField(encoded, "language_resource_directory_compatible", false);
  record.active = true;
  return record;
}

bool LanguageResourceDirectoryRecordIsComplete(
    const ServerLanguageResourceDirectoryRecord& record) {
  return !record.directory_id.empty() &&
         !record.manifest_hash.empty() &&
         !record.signing_key_id.empty() &&
         !record.scan_evidence_id.empty() &&
         !record.audit_reason.empty() &&
         record.signed_manifest_verified &&
         record.admitted_by_security_policy &&
         record.compatible_with_server;
}

std::string LanguageResourceDirectoryPacket(
    std::string_view operation_id,
    const ServerLanguageResourceDirectoryRecord& record,
    std::string_view outcome,
    bool mutated) {
  std::ostringstream out;
  out << "operation_id=" << operation_id << "\n"
      << "result_kind=language.resource_directory_registry.v1\n"
      << "outcome=" << outcome << "\n"
      << "mutated_language_resource_directory=" << (mutated ? "true" : "false") << "\n"
      << "cache_invalidated=" << (mutated ? "true" : "false") << "\n"
      << "server_language_resource_directory_authority=true\n"
      << "server_language_resource_registry_authority=true\n"
      << "parser_language_library_admission=false\n"
      << "load_or_reload_effects_executed_by_parser=false\n"
      << "row_storage_touched=false\n"
      << "mga_finality_claimed=false\n"
      << "directory_id=" << record.directory_id << "\n"
      << "directory_path_state="
      << (record.directory_path.empty() ? "not_reported" : "redacted") << "\n"
      << "manifest_hash=" << record.manifest_hash << "\n"
      << "signing_key_id=" << record.signing_key_id << "\n"
      << "scan_evidence_id=" << record.scan_evidence_id << "\n"
      << "audit_reason=" << record.audit_reason << "\n"
      << "signed_manifest_verified="
      << (record.signed_manifest_verified ? "true" : "false") << "\n"
      << "admitted_by_security_policy="
      << (record.admitted_by_security_policy ? "true" : "false") << "\n"
      << "compatible_with_server="
      << (record.compatible_with_server ? "true" : "false") << "\n"
      << "active=" << (record.active ? "true" : "false") << "\n"
      << "language_resource_epoch=" << record.language_resource_epoch << "\n"
      << "localized_name_epoch=" << record.localized_name_epoch << "\n"
      << "message_resource_epoch=" << record.message_resource_epoch << "\n";
  return out.str();
}

SessionOperationResult Failure(std::uint16_t response_type,
                               std::uint32_t schema,
                               std::array<std::uint8_t, 16> session_uuid,
                               std::string code,
                               std::string message,
                               std::string detail = {}) {
  SessionOperationResult result;
  result.response_message_type = response_type;
  result.response_schema_id = schema;
  result.session_uuid = session_uuid;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
  result.diagnostics.push_back(SblrServerDiagnostic(std::move(code), std::move(message), detail));
  result.payload = EncodePrepareResult("rejected", {}, "sblr.dispatch", std::move(detail));
  return result;
}

SessionOperationResult FailureWithDiagnostics(
    std::uint16_t response_type,
    std::uint32_t schema,
    std::array<std::uint8_t, 16> session_uuid,
    std::vector<ServerDiagnostic> diagnostics,
    std::string detail = {}) {
  SessionOperationResult result;
  result.response_message_type = response_type;
  result.response_schema_id = schema;
  result.session_uuid = session_uuid;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
  result.diagnostics = std::move(diagnostics);
  result.payload = EncodePrepareResult("rejected", {}, "sblr.dispatch", std::move(detail));
  return result;
}

std::string StringViewToString(sb_engine_string_view_t view) {
  return view.data == nullptr ? std::string{} : std::string(view.data, view.data + view.size_bytes);
}

bool LooksLikeBinarySblrEnvelope(std::string_view encoded) {
  return encoded.size() >= 4 && encoded[0] == 'S' && encoded[1] == 'B' &&
         encoded[2] == 'L' && encoded[3] == 'R';
}

std::optional<std::string> TextLineValue(std::string_view encoded, std::string_view key) {
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && line.substr(0, equals) == key) {
      return std::string(line.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> TextLineU64(std::string_view encoded, std::string_view key) {
  const auto value = TextLineValue(encoded, key);
  if (!value || value->empty()) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : *value) {
    if (!std::isdigit(ch)) return std::nullopt;
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed;
}

bool TextBoolField(std::string_view encoded, std::string_view key, bool default_value = false) {
  const auto value = TextLineValue(encoded, key);
  if (!value.has_value()) return default_value;
  return *value == "true" || *value == "1" || *value == "yes" || *value == "on";
}

std::string EncodedTextField(std::string_view encoded, const std::string& field) {
  return JsonTextField(encoded, field).value_or(TextLineValue(encoded, field).value_or(""));
}

std::string DispatchStatementShapeHash(std::string_view encoded) {
  std::ostringstream canonical;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start,
                       end == std::string_view::npos ? encoded.size() - start
                                                     : end - start);
    const std::size_t equals = line.find('=');
    const std::string_view key =
        equals == std::string_view::npos ? std::string_view{} : line.substr(0, equals);
    if (key != "negative_authorization_cache_key" &&
        key != "capability_cache_key" &&
        key != "statement_preflight_cache_key") {
      canonical << line << '\n';
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  const std::string digest = engine_api::SecuritySha256Hex(canonical.str());
  return digest.empty() ? std::string{} : "sha256:" + digest;
}

std::string StripDispatchAuthorityCacheMetadata(std::string_view encoded) {
  std::string stripped;
  bool removed = false;
  bool wrote_line = false;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start,
                       end == std::string_view::npos ? encoded.size() - start
                                                     : end - start);
    const std::size_t equals = line.find('=');
    const std::string_view key =
        equals == std::string_view::npos ? std::string_view{} : line.substr(0, equals);
    if (key == "negative_authorization_cache_key" ||
        key == "capability_cache_key" ||
        key == "statement_preflight_cache_key") {
      removed = true;
    } else {
      if (wrote_line) stripped.push_back('\n');
      stripped.append(line);
      wrote_line = true;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return removed ? stripped : std::string(encoded);
}

std::string DiagnosticDetailField(const ServerDiagnostic& diagnostic) {
  for (const auto& field : diagnostic.fields) {
    if (field.key == "detail") return field.value;
  }
  return diagnostic.code;
}

struct DispatchIdentifierPart {
  std::string text;
  bool quoted = false;
};

std::vector<DispatchIdentifierPart> SplitDispatchIdentifierPath(std::string_view path) {
  std::vector<DispatchIdentifierPart> parts;
  DispatchIdentifierPart current;
  bool in_quote = false;
  bool saw_quote = false;
  for (std::size_t index = 0; index < path.size(); ++index) {
    const char ch = path[index];
    if (in_quote) {
      if (ch == '"') {
        if (index + 1 < path.size() && path[index + 1] == '"') {
          current.text.push_back('"');
          ++index;
          continue;
        }
        in_quote = false;
        current.quoted = true;
        saw_quote = true;
        continue;
      }
      current.text.push_back(ch);
      continue;
    }
    if (ch == '"') {
      in_quote = true;
      current.quoted = true;
      saw_quote = true;
      continue;
    }
    if (ch == '.') {
      if (!current.text.empty()) {
        current.quoted = current.quoted || saw_quote;
        parts.push_back(std::move(current));
      }
      current = {};
      saw_quote = false;
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(ch)) || !current.text.empty()) {
      current.text.push_back(ch);
    }
  }
  while (!current.text.empty() &&
         std::isspace(static_cast<unsigned char>(current.text.back()))) {
    current.text.pop_back();
  }
  if (!in_quote && !current.text.empty()) {
    current.quoted = current.quoted || saw_quote;
    parts.push_back(std::move(current));
  }
  return parts;
}

engine_api::EngineIdentifierAtom DispatchIdentifierAtom(
    const DispatchIdentifierPart& part) {
  engine_api::EngineIdentifierAtom atom;
  atom.raw_text = part.text;
  atom.was_quoted = part.quoted;
  atom.quote_style = part.quoted ? "double_quote" : "none";
  atom.requires_exact_match = part.quoted;
  atom.identifier_profile_uuid = "sbsql_v3";
  return atom;
}

std::unordered_map<std::string, std::string>& DispatchSchemaParentPathCache() {
  thread_local std::unordered_map<std::string, std::string> cache;
  return cache;
}

void ClearDispatchSchemaParentPathCache() {
  DispatchSchemaParentPathCache().clear();
}

std::string DispatchSchemaParentPathCacheKey(const ServerSessionRecord& session,
                                             std::string_view schema_parent_path) {
  std::ostringstream key;
  key << "schema_parent_path_v1"
      << "|db=" << session.database_uuid
      << "|user=" << UuidBytesToText(session.effective_user_uuid)
      << "|role=" << UuidBytesToText(session.active_role_uuid)
      << "|security=" << session.security_epoch
      << "|grant=" << session.grant_epoch
      << "|policy=" << session.policy_generation
      << "|role_hash=" << session.role_set_hash
      << "|group_hash=" << session.group_set_hash
      << "|search_path_hash=" << session.search_path_hash
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|path=" << schema_parent_path;
  return key.str();
}

std::string ResolveSchemaParentPathForDispatch(
    const ServerSessionRecord& session,
    std::string_view schema_parent_path) {
  const auto parts = SplitDispatchIdentifierPath(schema_parent_path);
  if (parts.empty()) return {};
  const std::string cache_key =
      DispatchSchemaParentPathCacheKey(session, schema_parent_path);
  auto& cache = DispatchSchemaParentPathCache();
  const auto cached = cache.find(cache_key);
  if (cached != cache.end()) {
    return cached->second;
  }
  engine_api::EngineResolveNameRequest request;
  request.context = PublicAbiDispatchEngineContext(session);
  request.sql_object_reference.expected_object_type = "schema";
  request.sql_object_reference.path_type = parts.size() > 1 ? "qualified" : "unqualified";
  request.sql_object_reference.no_search_path = parts.size() > 1;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    request.sql_object_reference.path_components.push_back(
        DispatchIdentifierAtom(parts[index]));
  }
  request.sql_object_reference.object_name = DispatchIdentifierAtom(parts.back());
  const auto resolved = engine_api::EngineResolveName(request);
  if (!resolved.ok || resolved.primary_object.uuid.canonical.empty()) {
    return {};
  }
  if (cache.size() > 4096) {
    cache.clear();
  }
  cache[cache_key] = resolved.primary_object.uuid.canonical;
  return resolved.primary_object.uuid.canonical;
}

std::string ResolveDefaultSchemaForDispatch(const ServerSessionRecord& session) {
  for (std::string_view candidate : {"users.public", "public", "app"}) {
    const std::string resolved = ResolveSchemaParentPathForDispatch(session, candidate);
    if (!resolved.empty()) return resolved;
  }
  return {};
}

std::string ResolveDomainTypePathForDispatch(const ServerSessionRecord& session,
                                             std::string_view domain_path) {
  const auto parts = SplitDispatchIdentifierPath(domain_path);
  if (parts.empty()) return {};
  if (parts.size() > 1) {
    std::string schema_path;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
      if (parts[index].text.empty()) continue;
      if (!schema_path.empty()) schema_path.push_back('.');
      schema_path += parts[index].text;
    }
    const std::string schema_uuid =
        ResolveSchemaParentPathForDispatch(session, schema_path);
    if (!schema_uuid.empty()) {
      const auto context = PublicAbiDispatchEngineContext(session);
      const std::uint64_t observer_tx =
          context.snapshot_visible_through_local_transaction_id != 0
              ? context.snapshot_visible_through_local_transaction_id
              : context.local_transaction_id;
      const auto domains = engine_api::LoadDomainState(context);
      if (domains.ok) {
        const std::string profile =
            engine_api::NameRegistryDefaultIdentifierProfile(context);
        const std::string wanted_key =
            engine_api::NameRegistryLookupKey(parts.back().text, profile, false);
        std::string match;
        for (const auto& domain : domains.domains) {
          const auto visible =
              engine_api::FindVisibleDomain(context, domain.domain_uuid, observer_tx);
          if (!visible || visible->schema_uuid != schema_uuid) continue;
          if (engine_api::NameRegistryLookupKey(visible->default_name, profile, false) !=
              wanted_key) {
            continue;
          }
          if (!match.empty() && match != visible->domain_uuid) return {};
          match = visible->domain_uuid;
        }
        if (!match.empty()) return match;
      }
    }
  }
  engine_api::EngineResolveNameRequest request;
  request.context = PublicAbiDispatchEngineContext(session);
  request.sql_object_reference.expected_object_type = "domain";
  request.sql_object_reference.path_type = parts.size() > 1 ? "qualified" : "unqualified";
  request.sql_object_reference.no_search_path = parts.size() > 1;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    request.sql_object_reference.path_components.push_back(
        DispatchIdentifierAtom(parts[index]));
  }
  request.sql_object_reference.object_name = DispatchIdentifierAtom(parts.back());
  const auto resolved = engine_api::EngineResolveName(request);
  if (!resolved.ok || resolved.primary_object.object_kind != "domain") return {};
  return resolved.primary_object.uuid.canonical;
}

std::string ResolveSequencePathForDispatch(const ServerSessionRecord& session,
                                           std::string_view sequence_path) {
  if (sequence_path.empty()) return {};
  if (LooksLikeUuidText(sequence_path)) return NormalizeUuidText(sequence_path);
  const auto parts = SplitDispatchIdentifierPath(sequence_path);
  if (parts.empty()) return {};
  engine_api::EngineResolveNameRequest request;
  request.context = PublicAbiDispatchEngineContext(session);
  request.sql_object_reference.expected_object_type = "sequence";
  request.sql_object_reference.path_type = parts.size() > 1 ? "qualified" : "unqualified";
  request.sql_object_reference.no_search_path = parts.size() > 1;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    request.sql_object_reference.path_components.push_back(
        DispatchIdentifierAtom(parts[index]));
  }
  request.sql_object_reference.object_name = DispatchIdentifierAtom(parts.back());
  const auto resolved = engine_api::EngineResolveName(request);
  if (!resolved.ok || resolved.primary_object.object_kind != "sequence") return {};
  return resolved.primary_object.uuid.canonical;
}

std::string ResolveRelationPathForDispatch(const ServerSessionRecord& session,
                                           std::string_view relation_path) {
  if (relation_path.empty()) return {};
  if (LooksLikeUuidText(relation_path)) return NormalizeUuidText(relation_path);
  const auto parts = SplitDispatchIdentifierPath(relation_path);
  if (parts.empty()) return {};
  for (const auto expected : {"table", "view", "materialized_view", "relation"}) {
    engine_api::EngineResolveNameRequest request;
    request.context = PublicAbiDispatchEngineContext(session);
    request.sql_object_reference.expected_object_type = expected;
    request.sql_object_reference.path_type =
        parts.size() > 1 ? "qualified" : "unqualified";
    request.sql_object_reference.no_search_path = parts.size() > 1;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
      request.sql_object_reference.path_components.push_back(
          DispatchIdentifierAtom(parts[index]));
    }
    request.sql_object_reference.object_name = DispatchIdentifierAtom(parts.back());
    const auto resolved = engine_api::EngineResolveName(request);
    if (resolved.ok && !resolved.primary_object.uuid.canonical.empty()) {
      return resolved.primary_object.uuid.canonical;
    }
  }
  return {};
}

std::string TrimAsciiWhitespace(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

bool StartsWithAsciiNoCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    const auto lhs = static_cast<unsigned char>(text[index]);
    const auto rhs = static_cast<unsigned char>(prefix[index]);
    if (std::tolower(lhs) != std::tolower(rhs)) return false;
  }
  return true;
}

bool DescriptorHasField(std::string_view descriptor, std::string_view field) {
  if (descriptor.empty() || field.empty()) return false;
  const std::string prefix = std::string(field) + "=";
  return descriptor.rfind(prefix, 0) == 0 ||
         descriptor.find(";" + prefix) != std::string_view::npos;
}

std::string DescriptorFieldValue(std::string_view descriptor, std::string_view field) {
  if (descriptor.empty() || field.empty()) return {};
  const std::string prefix = std::string(field) + "=";
  std::size_t position = descriptor.find(prefix);
  while (position != std::string_view::npos) {
    if (position == 0 || descriptor[position - 1] == ';') {
      const std::size_t value_begin = position + prefix.size();
      const std::size_t value_end = descriptor.find(';', value_begin);
      return std::string(descriptor.substr(
          value_begin,
          value_end == std::string_view::npos
              ? std::string_view::npos
              : value_end - value_begin));
    }
    position = descriptor.find(prefix, position + 1);
  }
  return {};
}

void SetDescriptorField(std::string* descriptor,
                        std::string_view field,
                        std::string_view value) {
  if (descriptor == nullptr || field.empty() || value.empty()) return;
  const std::string prefix = std::string(field) + "=";
  std::size_t position = descriptor->find(prefix);
  while (position != std::string::npos) {
    if (position == 0 || (*descriptor)[position - 1] == ';') {
      const std::size_t value_begin = position + prefix.size();
      const std::size_t value_end = descriptor->find(';', value_begin);
      descriptor->replace(
          value_begin,
          value_end == std::string::npos ? std::string::npos : value_end - value_begin,
          value);
      return;
    }
    position = descriptor->find(prefix, position + 1);
  }
  if (!descriptor->empty()) descriptor->push_back(';');
  descriptor->append(field);
  descriptor->push_back('=');
  descriptor->append(value);
}

bool LooksLikeDomainTypePath(std::string_view candidate) {
  return !candidate.empty() &&
         candidate.find('.') != std::string_view::npos &&
         candidate.find('(') == std::string_view::npos &&
         candidate.find(')') == std::string_view::npos;
}

std::string ResolveColumnDomainUuidForDispatch(const ServerSessionRecord& session,
                                               std::string_view column_type,
                                               std::string_view column_descriptor) {
  const std::string existing_domain_uuid =
      DescriptorFieldValue(column_descriptor, "domain_uuid");
  if (!existing_domain_uuid.empty()) return existing_domain_uuid;

  for (std::string candidate : {
           DescriptorFieldValue(column_descriptor, "domain"),
           DescriptorFieldValue(column_descriptor, "source_type"),
           DescriptorFieldValue(column_descriptor, "type"),
           DescriptorFieldValue(column_descriptor, "canonical"),
           std::string(column_type)}) {
    if (!LooksLikeDomainTypePath(candidate)) continue;
    const std::string domain_uuid =
        ResolveDomainTypePathForDispatch(session, candidate);
    if (!domain_uuid.empty()) return domain_uuid;
  }
  return {};
}

std::string ResolveColumnDefaultForDispatch(const ServerSessionRecord& session,
                                            std::string_view default_expression) {
  constexpr std::string_view kSequenceNextPrefix = "sequence_next:";
  constexpr std::string_view kNextValueForPrefix = "next value for ";
  const std::string trimmed = TrimAsciiWhitespace(default_expression);
  if (trimmed.empty()) return {};
  std::string sequence_path;
  if (trimmed.rfind(kSequenceNextPrefix, 0) == 0) {
    sequence_path = TrimAsciiWhitespace(
        std::string_view(trimmed).substr(kSequenceNextPrefix.size()));
  } else if (StartsWithAsciiNoCase(trimmed, kNextValueForPrefix)) {
    sequence_path = TrimAsciiWhitespace(
        std::string_view(trimmed).substr(kNextValueForPrefix.size()));
  } else {
    return trimmed;
  }
  const std::string sequence_uuid = ResolveSequencePathForDispatch(session, sequence_path);
  if (sequence_uuid.empty()) return trimmed;
  return std::string(kSequenceNextPrefix) + sequence_uuid;
}

struct DispatchViewDescriptor {
  bool found = false;
  bool materialized = false;
  std::string view_uuid;
  std::string source_uuid;
  std::string predicate_kind;
  std::string predicate_column;
  std::string predicate_value;
  std::string predicate_value_type;
  std::string group_key_field;
  std::string aggregate_value_field;
  std::string aggregate_function;
};

std::optional<std::string> BehaviorPayloadField(std::string_view payload,
                                                std::string_view key) {
  if (const auto value = SemicolonFieldValue(payload, key)) {
    return value;
  }
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find(';', start);
    const std::string_view field =
        payload.substr(start, end == std::string_view::npos ? payload.size() - start : end - start);
    const std::size_t colon = field.find(':');
    if (colon != std::string_view::npos && field.substr(0, colon) == key) {
      return std::string(field.substr(colon + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<DispatchViewDescriptor> LoadDispatchViewDescriptor(
    const ServerSessionRecord& session,
    std::string_view view_uuid) {
  if (view_uuid.empty()) return std::nullopt;
  const auto context = PublicAbiDispatchEngineContext(session);
  const auto record = engine_api::FindVisibleApiBehaviorRecord(
      context,
      std::string(view_uuid),
      context.local_transaction_id);
  if (!record.has_value()) return std::nullopt;
  if (record->object_kind != "view" && record->object_kind != "materialized_view") {
    return std::nullopt;
  }
  DispatchViewDescriptor descriptor;
  descriptor.found = true;
  descriptor.materialized = record->object_kind == "materialized_view";
  descriptor.view_uuid = record->object_uuid;
  descriptor.source_uuid = BehaviorPayloadField(record->payload, "view_source_uuid").value_or("");
  descriptor.predicate_kind =
      BehaviorPayloadField(record->payload, "view_predicate_kind").value_or("");
  descriptor.predicate_column =
      BehaviorPayloadField(record->payload, "view_predicate_column").value_or("");
  descriptor.predicate_value =
      BehaviorPayloadField(record->payload, "view_predicate_value").value_or("");
  descriptor.predicate_value_type =
      BehaviorPayloadField(record->payload, "view_predicate_value_type").value_or("");
  descriptor.group_key_field =
      BehaviorPayloadField(record->payload, "view_group_key_field").value_or("");
  descriptor.aggregate_value_field =
      BehaviorPayloadField(record->payload, "view_aggregate_value_field").value_or("");
  descriptor.aggregate_function =
      BehaviorPayloadField(record->payload, "view_aggregate_function").value_or("");
  if (descriptor.source_uuid.empty()) return std::nullopt;
  return descriptor;
}

std::string MergeViewPredicateField(std::string_view view_value,
                                    std::string_view query_value) {
  if (view_value.empty()) return std::string(query_value);
  if (query_value.empty()) return std::string(view_value);
  if (view_value == query_value) return std::string(view_value);
  std::string merged(view_value);
  merged.push_back(',');
  merged.append(query_value);
  return merged;
}

void AppendDescriptorField(std::string* descriptor,
                           std::string_view field,
                           std::string_view value) {
  if (descriptor == nullptr || field.empty() || value.empty()) return;
  if (DescriptorHasField(*descriptor, field)) return;
  if (!descriptor->empty()) descriptor->push_back(';');
  descriptor->append(field);
  descriptor->push_back('=');
  descriptor->append(value);
}

std::uint64_t ParseU64Text(const std::string& value) {
  std::uint64_t parsed = 0;
  if (value.empty()) return 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return 0;
    parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed;
}

void AppendOperationOperand(std::string* operation_envelope,
                            std::string_view field,
                            std::string_view value) {
  if (operation_envelope == nullptr) return;
  *operation_envelope += "operand=text\t";
  *operation_envelope += field;
  *operation_envelope += "\t";
  *operation_envelope += EscapeOperationOperandField(value);
  *operation_envelope += "\n";
}

void AppendViewPredicateOperands(const DispatchViewDescriptor& descriptor,
                                 std::string_view encoded,
                                 std::string* operation_envelope) {
  if (operation_envelope == nullptr) return;
  const std::string query_kind = EncodedTextField(encoded, "predicate_kind");
  const std::string query_column = EncodedTextField(encoded, "predicate_column");
  const std::string query_value = EncodedTextField(encoded, "predicate_value");
  const std::string query_type = EncodedTextField(encoded, "predicate_value_type");
  if (descriptor.predicate_kind.empty()) {
    if (!query_kind.empty()) AppendOperationOperand(operation_envelope, "predicate_kind", query_kind);
    if (!query_column.empty()) AppendOperationOperand(operation_envelope, "predicate_column", query_column);
    if (!query_value.empty()) AppendOperationOperand(operation_envelope, "predicate_value", query_value);
    if (!query_type.empty()) AppendOperationOperand(operation_envelope, "predicate_value_type", query_type);
    return;
  }
  if (query_kind.empty()) {
    AppendOperationOperand(operation_envelope, "predicate_kind", descriptor.predicate_kind);
    AppendOperationOperand(operation_envelope, "predicate_column", descriptor.predicate_column);
    AppendOperationOperand(operation_envelope, "predicate_value", descriptor.predicate_value);
    AppendOperationOperand(operation_envelope, "predicate_value_type", descriptor.predicate_value_type);
    return;
  }
  if (descriptor.predicate_kind == "column_equals" && query_kind == "column_equals") {
    AppendOperationOperand(operation_envelope,
                           "predicate_kind",
                           descriptor.predicate_column == query_column
                               ? "column_equals"
                               : "columns_all_equal");
    AppendOperationOperand(operation_envelope,
                           "predicate_column",
                           MergeViewPredicateField(descriptor.predicate_column, query_column));
    AppendOperationOperand(operation_envelope,
                           "predicate_value",
                           MergeViewPredicateField(descriptor.predicate_value, query_value));
    AppendOperationOperand(operation_envelope,
                           "predicate_value_type",
                           MergeViewPredicateField(descriptor.predicate_value_type, query_type));
    return;
  }
  AppendOperationOperand(operation_envelope, "predicate_kind", query_kind);
  AppendOperationOperand(operation_envelope, "predicate_column", query_column);
  AppendOperationOperand(operation_envelope, "predicate_value", query_value);
  AppendOperationOperand(operation_envelope, "predicate_value_type", query_type);
}

void AppendOperationRowTextField(std::string* operation_envelope,
                                 std::string_view row_uuid,
                                 std::string_view field,
                                 std::string_view value) {
  if (operation_envelope == nullptr) return;
  *operation_envelope += "operand=row_field:text\t";
  *operation_envelope += EscapeOperationOperandField(row_uuid);
  *operation_envelope += "|";
  *operation_envelope += EscapeOperationOperandField(field);
  *operation_envelope += "\t";
  *operation_envelope += EscapeOperationOperandField(value);
  *operation_envelope += "\n";
}

void AppendProjectionExpressionOperands(std::string_view encoded,
                                        const std::string& prefix,
                                        std::string* operation_envelope,
                                        std::uint32_t depth = 0) {
  if (depth > 4) return;
  constexpr std::string_view kFields[] = {
      "name", "expr_kind", "expr_opcode", "type", "value", "is_null",
      "function_id", "function_arg_count",
      "operator_id", "canonical_operator_id", "operator_arg_count",
      "special_form_id", "sblr_binding", "special_form_arg_count"};
  for (const auto field : kFields) {
    const std::string key = prefix + std::string(field);
    AppendOperationOperand(operation_envelope, key, EncodedTextField(encoded, key));
  }

  std::uint64_t arg_count = ParseU64Text(EncodedTextField(encoded, prefix + "function_arg_count"));
  const std::uint64_t operator_arg_count =
      ParseU64Text(EncodedTextField(encoded, prefix + "operator_arg_count"));
  if (operator_arg_count > arg_count) arg_count = operator_arg_count;
  for (std::uint64_t arg_index = 0; arg_index < arg_count; ++arg_index) {
    AppendProjectionExpressionOperands(encoded,
                                       prefix + "arg_" + std::to_string(arg_index) + "_",
                                       operation_envelope,
                                       depth + 1);
  }
}

bool LooksLikeOrdinalInsertFieldName(std::string_view name) {
  if (name.size() < 2 || name.front() != 'c') return false;
  for (std::size_t index = 1; index < name.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(name[index]))) return false;
  }
  return true;
}

std::vector<std::string> VisibleInsertColumnNamesForTarget(
    const ServerSessionRecord& session,
    const std::string& target_object_uuid) {
  if (target_object_uuid.empty()) return {};
  const auto context = PublicAbiDispatchEngineContext(session);
  const auto loaded = engine_api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) return {};
  const engine_api::CrudState state =
      engine_api::BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = engine_api::FindVisibleCrudTable(
      state,
      target_object_uuid,
      context.local_transaction_id);
  if (!table) return {};
  std::vector<std::string> columns;
  columns.reserve(table->columns.size());
  for (const auto& [name, descriptor] : table->columns) {
    (void)descriptor;
    columns.push_back(name);
  }
  return columns;
}

struct InsertValueOperandCell {
  std::string name;
  std::string type;
  std::string value;
  bool is_null = false;
};

std::optional<std::uint64_t> ParseInsertValueOrdinal(std::string_view text,
                                                     std::size_t* cursor) {
  if (cursor == nullptr || *cursor >= text.size() ||
      !std::isdigit(static_cast<unsigned char>(text[*cursor]))) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  while (*cursor < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*cursor]))) {
    const auto digit = static_cast<std::uint64_t>(text[*cursor] - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return std::nullopt;
    }
    value = value * 10u + digit;
    ++(*cursor);
  }
  return value;
}

struct InsertValueKey {
  std::uint64_t row = 0;
  std::uint64_t column = 0;
  std::string_view field;
};

std::optional<InsertValueKey> ParseInsertValueKey(std::string_view key) {
  constexpr std::string_view prefix = "insert_values_";
  if (!key.starts_with(prefix)) return std::nullopt;
  std::size_t cursor = prefix.size();
  auto row = ParseInsertValueOrdinal(key, &cursor);
  if (!row || cursor >= key.size() || key[cursor] != '_') return std::nullopt;
  ++cursor;
  auto column = ParseInsertValueOrdinal(key, &cursor);
  if (!column || cursor >= key.size() || key[cursor] != '_') return std::nullopt;
  ++cursor;
  const std::string_view field = key.substr(cursor);
  if (field != "name" && field != "type" && field != "value" &&
      field != "is_null") {
    return std::nullopt;
  }
  return InsertValueKey{*row, *column, field};
}

int InsertValueHexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

bool InsertValueHexDecode(std::string_view encoded, std::string* out) {
  if (out == nullptr || (encoded.size() % 2u) != 0) return false;
  std::string decoded;
  decoded.reserve(encoded.size() / 2u);
  for (std::size_t index = 0; index < encoded.size(); index += 2u) {
    const int high = InsertValueHexDigit(encoded[index]);
    const int low = InsertValueHexDigit(encoded[index + 1u]);
    if (high < 0 || low < 0) return false;
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  *out = std::move(decoded);
  return true;
}

std::vector<std::string_view> SplitInsertCompactCell(std::string_view cell) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= cell.size()) {
    const std::size_t end = cell.find('|', start);
    parts.push_back(cell.substr(
        start,
        end == std::string_view::npos ? cell.size() - start : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  return parts;
}

std::vector<InsertValueOperandCell> ExtractCompactInsertValueOperandCells(
    std::string_view payload,
    std::uint64_t row_count,
    std::uint64_t column_count) {
  const std::uint64_t cell_count = row_count * column_count;
  std::vector<InsertValueOperandCell> cells;
  if (payload.empty() || row_count == 0 || column_count == 0 ||
      cell_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return cells;
  }
  cells.resize(static_cast<std::size_t>(cell_count));
  std::uint64_t ordinal = 0;
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find(';', start);
    const std::string_view cell =
        payload.substr(start,
                       end == std::string_view::npos ? payload.size() - start
                                                     : end - start);
    if (ordinal >= cell_count) return {};
    const auto parts = SplitInsertCompactCell(cell);
    if (parts.size() != 4) return {};
    auto& target = cells[static_cast<std::size_t>(ordinal)];
    if (!InsertValueHexDecode(parts[0], &target.name) ||
        !InsertValueHexDecode(parts[1], &target.type) ||
        !InsertValueHexDecode(parts[2], &target.value)) {
      return {};
    }
    target.is_null = parts[3] == "1" || parts[3] == "true";
    ++ordinal;
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  if (ordinal != cell_count) return {};
  return cells;
}

std::optional<std::string> JsonValueAt(std::string_view encoded,
                                       std::size_t colon) {
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < encoded.size() &&
         std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  if (cursor >= encoded.size()) return std::nullopt;
  if (encoded[cursor] == '"') {
    ++cursor;
    std::string out;
    bool escaped = false;
    while (cursor < encoded.size()) {
      const char ch = encoded[cursor++];
      if (!escaped && ch == '"') return out;
      if (!escaped && ch == '\\') {
        escaped = true;
        continue;
      }
      out.push_back(ch);
      escaped = false;
    }
    return std::nullopt;
  }
  const std::size_t begin = cursor;
  while (cursor < encoded.size() &&
         encoded[cursor] != ',' &&
         encoded[cursor] != '}' &&
         !std::isspace(static_cast<unsigned char>(encoded[cursor]))) {
    ++cursor;
  }
  if (cursor == begin) return std::nullopt;
  return std::string(encoded.substr(begin, cursor - begin));
}

std::vector<InsertValueOperandCell> ExtractInsertValueOperandCells(
    std::string_view encoded,
    std::uint64_t row_count,
    std::uint64_t column_count) {
  const std::uint64_t cell_count = row_count * column_count;
  std::vector<InsertValueOperandCell> cells;
  if (row_count == 0 || column_count == 0 ||
      cell_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return cells;
  }
  cells.resize(static_cast<std::size_t>(cell_count));
  std::size_t cursor = 0;
  while (cursor < encoded.size()) {
    const std::size_t key_begin = encoded.find("\"insert_values_", cursor);
    if (key_begin == std::string_view::npos) break;
    const std::size_t key_text_begin = key_begin + 1;
    const std::size_t key_end = encoded.find('"', key_text_begin);
    if (key_end == std::string_view::npos) break;
    const std::string_view key = encoded.substr(key_text_begin,
                                                key_end - key_text_begin);
    cursor = key_end + 1;
    const auto parsed = ParseInsertValueKey(key);
    if (!parsed || parsed->row >= row_count || parsed->column >= column_count) {
      continue;
    }
    const std::size_t colon = encoded.find(':', key_end + 1);
    if (colon == std::string_view::npos) break;
    auto value = JsonValueAt(encoded, colon);
    if (!value) continue;
    auto& cell = cells[static_cast<std::size_t>(
        parsed->row * column_count + parsed->column)];
    if (parsed->field == "name") {
      cell.name = std::move(*value);
    } else if (parsed->field == "type") {
      cell.type = std::move(*value);
    } else if (parsed->field == "value") {
      cell.value = std::move(*value);
    } else if (parsed->field == "is_null") {
      cell.is_null = *value == "true" || *value == "1";
    }
  }
  return cells;
}

void AppendDmlInsertValueOperands(const ServerSessionRecord& session,
                                  std::string_view encoded,
                                  std::string* operation_envelope) {
  if (operation_envelope == nullptr ||
      encoded.find("operand=row_field") != std::string_view::npos ||
      encoded.find("operand=row_null_field") != std::string_view::npos) {
    return;
  }
  const std::uint64_t row_count =
      ParseU64Text(EncodedTextField(encoded, "insert_values_row_count"));
  const std::uint64_t column_count =
      ParseU64Text(EncodedTextField(encoded, "insert_values_column_count"));
  if (row_count == 0 || column_count == 0) return;
  const std::string compact_format =
      EncodedTextField(encoded, "insert_values_compact_format");
  const std::string compact_payload =
      EncodedTextField(encoded, "insert_values_compact_payload");
  const bool explicit_column_list =
      JsonBoolField(encoded, "insert_values_column_list_present", false) ||
      TextBoolField(encoded, "insert_values_column_list_present", false);
  if (compact_format == "sbsql.insert_values.cells.v1" && !compact_payload.empty()) {
    AppendOperationOperand(operation_envelope,
                           "insert_values_row_count",
                           std::to_string(row_count));
    AppendOperationOperand(operation_envelope,
                           "insert_values_column_count",
                           std::to_string(column_count));
    AppendOperationOperand(operation_envelope,
                           "insert_values_column_list_present",
                           explicit_column_list ? "true" : "false");
    AppendOperationOperand(operation_envelope,
                           "insert_values_compact_format",
                           compact_format);
    AppendOperationOperand(operation_envelope,
                           "insert_values_compact_payload",
                           compact_payload);
    AppendOperationOperand(operation_envelope,
                           "insert_values_parser_executes_sql",
                           "false");
    if (!explicit_column_list) {
      const std::string target_uuid = EncodedTextField(encoded, "target_object_uuid");
      const auto descriptor_columns =
          VisibleInsertColumnNamesForTarget(session, target_uuid);
      for (std::size_t index = 0; index < descriptor_columns.size(); ++index) {
        if (!descriptor_columns[index].empty()) {
          AppendOperationOperand(operation_envelope,
                                 "insert_values_descriptor_column_" +
                                     std::to_string(index),
                                 descriptor_columns[index]);
        }
      }
    }
    return;
  }
  auto insert_cells =
      compact_format == "sbsql.insert_values.cells.v1" && !compact_payload.empty()
          ? ExtractCompactInsertValueOperandCells(compact_payload,
                                                  row_count,
                                                  column_count)
          : std::vector<InsertValueOperandCell>{};
  if (insert_cells.empty()) {
    insert_cells = ExtractInsertValueOperandCells(encoded, row_count, column_count);
  }
  if (insert_cells.empty()) return;
  operation_envelope->reserve(
      operation_envelope->size() +
      static_cast<std::size_t>(
          std::min<std::uint64_t>(row_count * column_count * 96,
                                  static_cast<std::uint64_t>(
                                      std::numeric_limits<std::size_t>::max() -
                                      operation_envelope->size()))));

  std::vector<std::string> descriptor_columns;
  if (!explicit_column_list) {
    const std::string target_uuid = EncodedTextField(encoded, "target_object_uuid");
    descriptor_columns = VisibleInsertColumnNamesForTarget(session, target_uuid);
  }

  for (std::uint64_t row = 0; row < row_count; ++row) {
    const std::string row_uuid = engine_api::GenerateCrudEngineUuid("row");
    for (std::uint64_t column = 0; column < column_count; ++column) {
      const auto& cell = insert_cells[static_cast<std::size_t>(
          row * column_count + column)];
      std::string name = cell.name;
      if (!explicit_column_list &&
          (name.empty() || LooksLikeOrdinalInsertFieldName(name)) &&
          column < descriptor_columns.size() &&
          !descriptor_columns[static_cast<std::size_t>(column)].empty()) {
        name = descriptor_columns[static_cast<std::size_t>(column)];
      }
      if (name.empty()) name = "c" + std::to_string(column);
      std::string type = cell.type;
      if (type.empty()) type = "text";
      const std::string& value = cell.value;
      const bool is_null = cell.is_null;
      *operation_envelope += "operand=";
      *operation_envelope += is_null ? "row_null_field:" : "row_field:";
      *operation_envelope += EscapeOperationOperandField(type);
      *operation_envelope += "\t";
      *operation_envelope += EscapeOperationOperandField(row_uuid);
      *operation_envelope += "|";
      *operation_envelope += EscapeOperationOperandField(name);
      *operation_envelope += "\t";
      *operation_envelope += EscapeOperationOperandField(value);
      *operation_envelope += "\n";
    }
  }
}

std::optional<std::uint64_t> EvidenceU64(std::string_view encoded, std::string_view evidence_kind) {
  const std::string prefix = "evidence=" + std::string(evidence_kind) + ":";
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.starts_with(prefix)) {
      return TextLineU64(std::string("value=") + std::string(line.substr(prefix.size())), "value");
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<std::string> EvidenceText(std::string_view encoded, std::string_view evidence_kind) {
  const std::string prefix = "evidence=" + std::string(evidence_kind) + ":";
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.starts_with(prefix)) {
      return std::string(line.substr(prefix.size()));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

bool IsClusterDispatchOperation(std::string_view operation_id) {
  return operation_id.starts_with("cluster.") ||
         operation_id.starts_with("placement.cluster.");
}

const char* CatalogMutationPublicAbiOpcodeForOperation(std::string_view operation_id) {
  if (operation_id == "catalog.mutation.create_materialized_view") return "SBLR_CATALOG_MUTATION_CREATE_MATERIALIZED_VIEW";
  if (operation_id == "catalog.mutation.create_cast") return "SBLR_CATALOG_MUTATION_CREATE_CAST";
  if (operation_id == "catalog.mutation.create_server") return "SBLR_CATALOG_MUTATION_CREATE_SERVER";
  if (operation_id == "catalog.mutation.show_storage_buffer_io_index") return "SBLR_CATALOG_MUTATION_SHOW_STORAGE_BUFFER_IO_INDEX";
  if (operation_id == "catalog.mutation.alter_time_series") return "SBLR_CATALOG_MUTATION_ALTER_TIME_SERIES";
  if (operation_id == "catalog.mutation.create_key_value_store") return "SBLR_CATALOG_MUTATION_CREATE_KEY_VALUE_STORE";
  if (operation_id == "catalog.mutation.cypher_create") return "SBLR_CATALOG_MUTATION_CYPHER_CREATE";
  if (operation_id == "catalog.mutation.graph_create_node") return "SBLR_CATALOG_MUTATION_GRAPH_CREATE_NODE";
  if (operation_id == "catalog.mutation.create_bucket") return "SBLR_CATALOG_MUTATION_CREATE_BUCKET";
  if (operation_id == "catalog.mutation.alter_filespace") return "SBLR_CATALOG_MUTATION_ALTER_FILESPACE";
  if (operation_id == "catalog.mutation.create_operation") return "SBLR_CATALOG_MUTATION_CREATE_OPERATION";
  if (operation_id == "catalog.mutation.create_operator") return "SBLR_CATALOG_MUTATION_CREATE_OPERATOR";
  if (operation_id == "catalog.mutation.create_event_trigger") return "SBLR_CATALOG_MUTATION_CREATE_EVENT_TRIGGER";
  if (operation_id == "catalog.mutation.create_package_body") return "SBLR_CATALOG_MUTATION_CREATE_PACKAGE_BODY";
  if (operation_id == "catalog.mutation.create_aggregate") return "SBLR_CATALOG_MUTATION_CREATE_AGGREGATE";
  if (operation_id == "catalog.mutation.alter_routine") return "SBLR_CATALOG_MUTATION_ALTER_ROUTINE";
  if (operation_id == "catalog.mutation.create_graph") return "SBLR_CATALOG_MUTATION_CREATE_GRAPH";
  if (operation_id == "catalog.mutation.create_dictionary") return "SBLR_CATALOG_MUTATION_CREATE_DICTIONARY";
  if (operation_id == "catalog.mutation.create_package") return "SBLR_CATALOG_MUTATION_CREATE_PACKAGE";
  if (operation_id == "catalog.mutation.alter_udr") return "SBLR_CATALOG_MUTATION_ALTER_UDR";
  if (operation_id == "catalog.mutation.graph_create_edge") return "SBLR_CATALOG_MUTATION_GRAPH_CREATE_EDGE";
  if (operation_id == "catalog.mutation.create_filespace") return "SBLR_CATALOG_MUTATION_CREATE_FILESPACE";
  if (operation_id == "catalog.mutation.create_filespace_agent") return "SBLR_CATALOG_MUTATION_CREATE_FILESPACE_AGENT";
  if (operation_id == "catalog.mutation.create_quota") return "SBLR_CATALOG_MUTATION_CREATE_QUOTA";
  if (operation_id == "catalog.mutation.alter_key_value_store") return "SBLR_CATALOG_MUTATION_ALTER_KEY_VALUE_STORE";
  if (operation_id == "catalog.mutation.alter_subject") return "SBLR_CATALOG_MUTATION_ALTER_SUBJECT";
  if (operation_id == "catalog.mutation.graph_create_index") return "SBLR_CATALOG_MUTATION_GRAPH_CREATE_INDEX";
  if (operation_id == "catalog.mutation.create_binding") return "SBLR_CATALOG_MUTATION_CREATE_BINDING";
  if (operation_id == "catalog.mutation.create_monitor") return "SBLR_CATALOG_MUTATION_CREATE_MONITOR";
  if (operation_id == "catalog.mutation.refresh_materialized_view") return "SBLR_CATALOG_MUTATION_REFRESH_MATERIALIZED_VIEW";
  if (operation_id == "catalog.mutation.create_transform") return "SBLR_CATALOG_MUTATION_CREATE_TRANSFORM";
  if (operation_id == "catalog.mutation.create_secret") return "SBLR_CATALOG_MUTATION_CREATE_SECRET";
  if (operation_id == "catalog.mutation.alter_reference") return "SBLR_CATALOG_MUTATION_ALTER_REFERENCE";
  if (operation_id == "catalog.mutation.create_pipeline") return "SBLR_CATALOG_MUTATION_CREATE_PIPELINE";
  if (operation_id == "catalog.mutation.create_collation") return "SBLR_CATALOG_MUTATION_CREATE_COLLATION";
  if (operation_id == "catalog.mutation.create_type") return "SBLR_CATALOG_MUTATION_CREATE_TYPE";
  if (operation_id == "catalog.mutation.alter_view") return "SBLR_CATALOG_MUTATION_ALTER_VIEW";
  if (operation_id == "catalog.mutation.create_udr") return "SBLR_CATALOG_MUTATION_CREATE_UDR";
  if (operation_id == "catalog.mutation.create_tenant") return "SBLR_CATALOG_MUTATION_CREATE_TENANT";
  if (operation_id == "catalog.mutation.create_time_series") return "SBLR_CATALOG_MUTATION_CREATE_TIME_SERIES";
  if (operation_id == "catalog.mutation.create_document_collection") return "SBLR_CATALOG_MUTATION_CREATE_DOCUMENT_COLLECTION";
  return nullptr;
}

const char* BridgePublicAbiOpcodeForOperation(std::string_view operation_id) {
  if (operation_id == "bridge.describe_capabilities") return "SBLR_BRIDGE_DESCRIBE_CAPABILITIES";
  if (operation_id == "bridge.connect" || operation_id == "bridge.attach") {
    return "SBLR_BRIDGE_OPEN_CHANNEL";
  }
  if (operation_id == "bridge.authenticate") return "SBLR_BRIDGE_AUTHENTICATE";
  if (operation_id == "bridge.open_session") return "SBLR_BRIDGE_OPEN_SESSION";
  if (operation_id == "bridge.close_session" || operation_id == "bridge.detach") {
    return "SBLR_BRIDGE_CLOSE_SESSION";
  }
  if (operation_id == "bridge.ping" || operation_id == "bridge.health") {
    return "SBLR_BRIDGE_HEALTH";
  }
  if (operation_id == "bridge.cancel") return "SBLR_BRIDGE_CANCEL";
  if (operation_id == "bridge.drain" || operation_id == "bridge.shutdown") {
    return "SBLR_BRIDGE_DRAIN";
  }
  if (operation_id == "bridge.begin") return "SBLR_BRIDGE_TX_BEGIN";
  if (operation_id == "bridge.commit") return "SBLR_BRIDGE_TX_COMMIT";
  if (operation_id == "bridge.rollback") return "SBLR_BRIDGE_TX_ROLLBACK";
  if (operation_id == "bridge.prepare") return "SBLR_BRIDGE_TX_PREPARE";
  if (operation_id == "bridge.savepoint") return "SBLR_BRIDGE_TX_SAVEPOINT";
  if (operation_id == "bridge.execute") return "SBLR_BRIDGE_EXECUTE";
  if (operation_id == "bridge.cursor_open") return "SBLR_BRIDGE_CURSOR_OPEN";
  if (operation_id == "bridge.cursor_fetch") return "SBLR_BRIDGE_CURSOR_FETCH";
  if (operation_id == "bridge.cursor_close") return "SBLR_BRIDGE_CURSOR_CLOSE";
  if (operation_id == "bridge.stream_open") return "SBLR_BRIDGE_STREAM_OPEN";
  if (operation_id == "bridge.stream_read") return "SBLR_BRIDGE_STREAM_READ";
  if (operation_id == "bridge.stream_write") return "SBLR_BRIDGE_STREAM_WRITE";
  if (operation_id == "bridge.stream_close") return "SBLR_BRIDGE_STREAM_CLOSE";
  if (operation_id == "bridge.cdc_start") return "SBLR_BRIDGE_CDC_START";
  if (operation_id == "bridge.cdc_read") return "SBLR_BRIDGE_CDC_READ";
  if (operation_id == "bridge.cdc_apply") return "SBLR_BRIDGE_CDC_APPLY";
  if (operation_id == "bridge.proxy_route") return "SBLR_BRIDGE_PROXY_ROUTE";
  if (operation_id == "bridge.compare_result") return "SBLR_BRIDGE_COMPARE_RESULT";
  if (operation_id == "bridge.cutover") return "SBLR_BRIDGE_CUTOVER";
  if (operation_id == "bridge.validate" ||
      operation_id == "bridge.cluster_route" ||
      operation_id == "bridge.cluster.distributed_query" ||
      operation_id == "bridge.cluster.cross_node_query") {
    return "SBLR_BRIDGE_VALIDATE";
  }
  return nullptr;
}

const char* PublicAbiOpcodeForOperation(std::string_view operation_id) {
  if (operation_id.starts_with("bridge.")) return BridgePublicAbiOpcodeForOperation(operation_id);
  if (operation_id == "cluster.sys.agents") return "SBLR_CLUSTER_SYS_AGENTS";
  if (operation_id == "cluster.inspect_state") return "SBLR_CLUSTER_INSPECT_STATE";
  if (operation_id == "cluster.inspect_routing_plan") return "SBLR_CLUSTER_INSPECT_ROUTING_PLAN";
  if (operation_id == "cluster.control_cluster") return "SBLR_CLUSTER_CONTROL_CLUSTER";
  if (operation_id == "cluster.inspect_provider") return "SBLR_CLUSTER_INSPECT_PROVIDER";
  if (operation_id == "cluster.place_object") return "SBLR_CLUSTER_PLACE_OBJECT";
  if (operation_id == "cluster.inspect_replication") return "SBLR_CLUSTER_INSPECT_REPLICATION";
  if (operation_id == "cluster.prepare_remote_participant_insert") return "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT";
  if (operation_id == "cluster.validate_insert_route_fence") return "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE";
  if (operation_id == "cluster.profile_operation") return "SBLR_CLUSTER_PROFILE_OPERATION";
  if (IsClusterDispatchOperation(operation_id)) return "SBLR_CLUSTER_PRIVATE_OPERATION";
  if (operation_id == "dml.select_rows") return "SBLR_DML_SELECT_ROWS";
  if (operation_id == "dml.insert_rows") return "SBLR_DML_INSERT_ROWS";
  if (operation_id == "dml.update_rows") return "SBLR_DML_UPDATE_ROWS";
  if (operation_id == "dml.delete_rows") return "SBLR_DML_DELETE_ROWS";
  if (operation_id == "dml.merge_rows") return "SBLR_DML_MERGE_ROWS";
  if (operation_id == "dml.plan_import_rows") return "SBLR_DML_PLAN_IMPORT_ROWS";
  if (operation_id == "dml.execute_import_rows") return "SBLR_DML_EXECUTE_IMPORT_ROWS";
  if (operation_id == "dml.execute_native_bulk_ingest") return "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST";
  if (operation_id == "routine.procedure_invoke") return "SBLR_PROCEDURE_INVOKE";
  if (operation_id == "routine.function_invoke") return "SBLR_FUNCTION_INVOKE";
  if (operation_id == "extensibility.inspect_gpu_capability") return "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY";
  if (operation_id == "extensibility.compile_llvm_module") return "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE";
  if (operation_id == "filespace.create") return "SBLR_FILESPACE_CREATE";
  if (operation_id == "filespace.preallocate") return "SBLR_FILESPACE_PREALLOCATE";
  if (operation_id == "filespace.attach") return "SBLR_FILESPACE_ATTACH";
  if (operation_id == "filespace.detach") return "SBLR_FILESPACE_DETACH";
  if (operation_id == "filespace.disconnect") return "SBLR_FILESPACE_DISCONNECT";
  if (operation_id == "filespace.move") return "SBLR_FILESPACE_MOVE";
  if (operation_id == "filespace.merge") return "SBLR_FILESPACE_MERGE";
  if (operation_id == "filespace.promote") return "SBLR_FILESPACE_PROMOTE";
  if (operation_id == "filespace.verify") return "SBLR_FILESPACE_VERIFY";
  if (operation_id == "filespace.compact") return "SBLR_FILESPACE_COMPACT";
  if (operation_id == "filespace.fence") return "SBLR_FILESPACE_FENCE";
  if (operation_id == "filespace.release") return "SBLR_FILESPACE_RELEASE";
  if (operation_id == "filespace.archive") return "SBLR_FILESPACE_ARCHIVE";
  if (operation_id == "filespace.quarantine") return "SBLR_FILESPACE_QUARANTINE";
  if (operation_id == "filespace.snapshot.create") return "SBLR_FILESPACE_SNAPSHOT_CREATE";
  if (operation_id == "filespace.snapshot.refresh") return "SBLR_FILESPACE_SNAPSHOT_REFRESH";
  if (operation_id == "filespace.snapshot.validate") return "SBLR_FILESPACE_SNAPSHOT_VALIDATE";
  if (operation_id == "filespace.snapshot.retire") return "SBLR_FILESPACE_SNAPSHOT_RETIRE";
  if (operation_id == "filespace.shadow.create") return "SBLR_FILESPACE_SHADOW_CREATE";
  if (operation_id == "filespace.shadow.refresh") return "SBLR_FILESPACE_SHADOW_REFRESH";
  if (operation_id == "filespace.shadow.validate") return "SBLR_FILESPACE_SHADOW_VALIDATE";
  if (operation_id == "filespace.shadow.promote") return "SBLR_FILESPACE_SHADOW_PROMOTE";
  if (operation_id == "filespace.truncate") return "SBLR_FILESPACE_TRUNCATE";
  if (operation_id == "filespace.drop") return "SBLR_FILESPACE_DROP";
  if (operation_id == "filespace.delete_physical") return "SBLR_FILESPACE_DELETE_PHYSICAL";
  if (operation_id == "filespace.repair") return "SBLR_FILESPACE_REPAIR";
  if (operation_id == "filespace.rebuild") return "SBLR_FILESPACE_REBUILD";
  if (operation_id == "filespace.salvage") return "SBLR_FILESPACE_SALVAGE";
  if (operation_id == "storage.manage_operation") return "SBLR_STORAGE_MANAGEMENT_OPERATION";
  if (operation_id == "ddl.create_table") return "SBLR_DDL_CREATE_TABLE";
  if (operation_id == "query.cast_value") return "SBLR_QUERY_CAST_VALUE";
  if (operation_id == "query.evaluate_projection") return "SBLR_QUERY_EVALUATE_PROJECTION";
  if (operation_id == "query.plan_operation") return "SBLR_QUERY_PLAN_OPERATION";
  if (operation_id == "nosql.document_insert") return "SBLR_NOSQL_DOCUMENT_INSERT";
  if (operation_id == "nosql.document_find") return "SBLR_NOSQL_DOCUMENT_FIND";
  if (operation_id == "nosql.document_update") return "SBLR_NOSQL_DOCUMENT_UPDATE";
  if (operation_id == "nosql.document_delete") return "SBLR_NOSQL_DOCUMENT_DELETE";
  if (operation_id == "nosql.graph_query") return "SBLR_NOSQL_GRAPH_QUERY";
  if (operation_id == "nosql.key_value_get") return "SBLR_NOSQL_KEY_VALUE_GET";
  if (operation_id == "nosql.key_value_put") return "SBLR_NOSQL_KEY_VALUE_PUT";
  if (operation_id == "nosql.key_value_multiget") return "SBLR_NOSQL_KEY_VALUE_MULTIGET";
  if (operation_id == "nosql.key_value_pipeline") return "SBLR_NOSQL_KEY_VALUE_PIPELINE";
  if (operation_id == "nosql.key_value_atomic_program") return "SBLR_NOSQL_KEY_VALUE_ATOMIC_PROGRAM";
  if (operation_id == "nosql.backpressure_debt_plan") return "SBLR_NOSQL_BACKPRESSURE_DEBT_PLAN";
  if (operation_id == "nosql.family_maintenance_plan") return "SBLR_NOSQL_FAMILY_MAINTENANCE_PLAN";
  if (operation_id == "nosql.statistics_advisor_plan") return "SBLR_NOSQL_STATISTICS_ADVISOR_PLAN";
  if (operation_id == "nosql.time_series_append") return "SBLR_NOSQL_TIME_SERIES_APPEND";
  if (operation_id == "nosql.vector_search") return "SBLR_NOSQL_VECTOR_SEARCH";
  if (operation_id == "nosql.vector_collection_op") return "SBLR_NOSQL_VECTOR_COLLECTION_OP";
  if (operation_id == "nosql.search_query") return "SBLR_NOSQL_SEARCH_QUERY";
  if (operation_id == "observability.show_version") return "SBLR_OBSERVABILITY_SHOW_VERSION";
  if (operation_id == "observability.show_database") return "SBLR_OBSERVABILITY_SHOW_DATABASE";
  if (operation_id == "observability.show_system") return "SBLR_OBSERVABILITY_SHOW_SYSTEM";
  if (operation_id == "observability.show_catalog") return "SBLR_OBSERVABILITY_SHOW_CATALOG";
  if (operation_id == "observability.show_sessions") return "SBLR_OBSERVABILITY_SHOW_SESSIONS";
  if (operation_id == "observability.show_transactions") return "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS";
  if (operation_id == "observability.show_locks") return "SBLR_OBSERVABILITY_SHOW_LOCKS";
  if (operation_id == "observability.show_statements") return "SBLR_OBSERVABILITY_SHOW_STATEMENTS";
  if (operation_id == "observability.show_jobs") return "SBLR_OBSERVABILITY_SHOW_JOBS";
  if (operation_id == "observability.show_management") return "SBLR_OBSERVABILITY_SHOW_MANAGEMENT";
  if (operation_id == "observability.show_diagnostics") return "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS";
  if (operation_id == "observability.show_diagnostics_extended") return "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED";
  if (operation_id == "observability.show_archive_replication") return "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION";
  if (operation_id == "observability.show_agents_extended") return "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED";
  if (operation_id == "observability.show_filespace_extended") return "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED";
  if (operation_id == "observability.show_decision_service") return "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE";
  if (operation_id == "observability.show_acceleration") return "SBLR_OBSERVABILITY_SHOW_ACCELERATION";
  if (operation_id == "observability.show_acceleration_extended") return "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED";
  if (operation_id == "observability.show_metrics") return "SBLR_OBSERVABILITY_SHOW_METRICS";
  if (operation_id == "observability.explain_operation") return "SBLR_OBSERVABILITY_EXPLAIN_OPERATION";
  if (operation_id == "op.sbsql.surface_replay") return "SBLR_OP_SBSQL_SURFACE_REPLAY";
  if (operation_id == "catalog.get_descriptor") return "SBLR_CATALOG_GET_DESCRIPTOR";
  if (operation_id == "artifact.export_catalog") return "SBLR_ARTIFACT_EXPORT_CATALOG";
  if (operation_id == "artifact.import_catalog") return "SBLR_ARTIFACT_IMPORT_CATALOG";
  if (operation_id == "artifact.external_git.export_snapshot") {
    return "SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT";
  }
  if (operation_id == "artifact.external_git.diff_snapshot") {
    return "SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT";
  }
  if (operation_id == "artifact.external_git.rollback_plan") {
    return "SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN";
  }
  if (const char* catalog_mutation_opcode =
          CatalogMutationPublicAbiOpcodeForOperation(operation_id);
      catalog_mutation_opcode != nullptr) {
    return catalog_mutation_opcode;
  }
  if (operation_id == "agents.list") return "SBLR_AGENTS_LIST";
  if (operation_id == "agents.show") return "SBLR_AGENTS_SHOW";
  if (operation_id == "agents.start") return "SBLR_AGENTS_START";
  if (operation_id == "agents.stop") return "SBLR_AGENTS_STOP";
  if (operation_id == "agents.pause") return "SBLR_AGENTS_PAUSE";
  if (operation_id == "agents.resume") return "SBLR_AGENTS_RESUME";
  if (operation_id == "agents.configure") return "SBLR_AGENTS_CONFIGURE";
  if (operation_id == "agents.run") return "SBLR_AGENTS_RUN";
  if (operation_id == "agents.dry_run") return "SBLR_AGENTS_DRY_RUN";
  if (operation_id == "agents.override") return "SBLR_AGENTS_OVERRIDE";
  if (operation_id == "agents.metrics.get") return "SBLR_AGENT_METRICS_GET";
  if (operation_id == "agents.policy.get") return "SBLR_AGENT_POLICY_GET";
  if (operation_id == "agents.evidence.list") return "SBLR_AGENT_EVIDENCE_LIST";
  if (operation_id == "agents.audit.list") return "SBLR_AGENT_AUDIT_LIST";
  if (operation_id == "agents.actions.list") return "SBLR_AGENT_ACTION_LIST";
  if (operation_id == "agents.overrides.list") return "SBLR_AGENT_OVERRIDE_LIST";
  if (operation_id == "agents.drain") return "SBLR_AGENT_LIFECYCLE_DRAIN";
  if (operation_id == "agents.restart") return "SBLR_AGENT_LIFECYCLE_RESTART";
  if (operation_id == "agents.enable") return "SBLR_AGENT_LIFECYCLE_ENABLE";
  if (operation_id == "agents.disable") return "SBLR_AGENT_LIFECYCLE_DISABLE";
  if (operation_id == "agents.quarantine") return "SBLR_AGENT_QUARANTINE";
  if (operation_id == "agents.unquarantine") return "SBLR_AGENT_UNQUARANTINE";
  if (operation_id == "agents.policy.attach") return "SBLR_AGENT_POLICY_ATTACH";
  if (operation_id == "agents.policy.detach") return "SBLR_AGENT_POLICY_DETACH";
  if (operation_id == "agents.policy.validate") return "SBLR_AGENT_POLICY_VALIDATE";
  if (operation_id == "agents.policy.simulate") return "SBLR_AGENT_POLICY_SIMULATE";
  if (operation_id == "agents.policy.apply") return "SBLR_AGENT_POLICY_APPLY";
  if (operation_id == "agents.policy.rollback") return "SBLR_AGENT_POLICY_ROLLBACK";
  if (operation_id == "agents.action.approve") return "SBLR_AGENT_ACTION_APPROVE";
  if (operation_id == "agents.action.cancel") return "SBLR_AGENT_ACTION_CANCEL";
  if (operation_id == "agents.action.retry") return "SBLR_AGENT_ACTION_RETRY";
  if (operation_id == "agents.action.suppress") return "SBLR_AGENT_ACTION_SUPPRESS";
  if (operation_id == "agents.override.create") return "SBLR_AGENT_OVERRIDE_CREATE";
  if (operation_id == "agents.override.update") return "SBLR_AGENT_OVERRIDE_UPDATE";
  if (operation_id == "agents.override.drop") return "SBLR_AGENT_OVERRIDE_DROP";
  if (operation_id == "agents.set_mode") return "SBLR_AGENT_SET_MODE";
  if (operation_id == "filespaces.show") return "SBLR_SHOW_FILESPACES";
  if (operation_id == "filespaces.health.show") return "SBLR_SHOW_FILESPACE_HEALTH";
  if (operation_id == "filespaces.capacity.show") return "SBLR_SHOW_FILESPACE_CAPACITY";
  if (operation_id == "pages.allocation.show") return "SBLR_SHOW_PAGE_ALLOCATION";
  if (operation_id == "pages.allocation.family.show") return "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY";
  if (operation_id == "pages.relocation_backlog.show") return "SBLR_SHOW_PAGE_RELOCATION_BACKLOG";
  if (operation_id == "filespaces.shrink_readiness.show") return "SBLR_SHOW_FILESPACE_SHRINK_READINESS";
  if (operation_id == "cluster.agent.list") return "SBLR_CLUSTER_AGENT_LIST";
  if (operation_id == "cluster.agent.get") return "SBLR_CLUSTER_AGENT_GET";
  if (operation_id == "cluster.agent.control") return "SBLR_CLUSTER_AGENT_CONTROL";
  if (operation_id == "event.channel.create") return "SBLR_EVENT_CHANNEL_CREATE";
  if (operation_id == "event.channel.listen") return "SBLR_EVENT_CHANNEL_LISTEN";
  if (operation_id == "event.channel.unlisten") return "SBLR_EVENT_CHANNEL_UNLISTEN";
  if (operation_id == "event.channel.notify") return "SBLR_EVENT_CHANNEL_NOTIFY";
  if (operation_id == "event.subscription.list") return "SBLR_EVENT_SUBSCRIPTION_LIST";
  if (operation_id == "event.delivery.poll") return "SBLR_EVENT_DELIVERY_POLL";
  if (operation_id == "event.delivery.ack") return "SBLR_EVENT_DELIVERY_ACK";
  if (operation_id == "session.notification.unlisten") return "SBLR_EVENT_CHANNEL_UNLISTEN";
  if (operation_id == "session.notification.unlisten_all") return "SBLR_EVENT_CHANNEL_UNLISTEN_ALL";
  if (operation_id == "op.migration.begin_from_reference") return "SBLR_MIGRATION_BEGIN_FROM_REFERENCE";
  if (operation_id == "op.migration.alter") return "SBLR_MIGRATION_ALTER";
  if (operation_id == "op.show.migration") return "SBLR_SHOW_MIGRATION";
  if (operation_id == "op.show.migrations") return "SBLR_SHOW_MIGRATIONS";
  if (operation_id.starts_with("op.management.") ||
      operation_id.starts_with("op.show.management.")) {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id == "management.inspect_runtime") return "SBLR_MANAGEMENT_INSPECT_RUNTIME";
  if (operation_id == "management.control_runtime") return "SBLR_MANAGEMENT_CONTROL_RUNTIME";
  if (operation_id.starts_with("memory.")) {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("storage_tier.")) {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("filespace.discovery.")) {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("filespace.package.")) {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("shard_placement.")) {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("security.encryption_key.") ||
      operation_id.starts_with("security.protected_material") ||
      operation_id == "security.encrypted_filespace.open" ||
      operation_id == "security.request_protected_material") {
    if (const auto* entry = scratchbird::engine::sblr::LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id == "lifecycle.create_database") return "SBLR_LIFECYCLE_CREATE_DATABASE";
  if (operation_id == "lifecycle.open_database") return "SBLR_LIFECYCLE_OPEN_DATABASE";
  if (operation_id == "lifecycle.attach_database") return "SBLR_LIFECYCLE_ATTACH_DATABASE";
  if (operation_id == "lifecycle.detach_database") return "SBLR_LIFECYCLE_DETACH_DATABASE";
  if (operation_id == "lifecycle.enter_maintenance") return "SBLR_LIFECYCLE_ENTER_MAINTENANCE";
  if (operation_id == "lifecycle.exit_maintenance") return "SBLR_LIFECYCLE_EXIT_MAINTENANCE";
  if (operation_id == "lifecycle.enter_restricted_open") return "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN";
  if (operation_id == "lifecycle.exit_restricted_open") return "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN";
  if (operation_id == "lifecycle.inspect_database") return "SBLR_LIFECYCLE_INSPECT_DATABASE";
  if (operation_id == "lifecycle.verify_database") return "SBLR_LIFECYCLE_VERIFY_DATABASE";
  if (operation_id == "lifecycle.repair_database") return "SBLR_LIFECYCLE_REPAIR_DATABASE";
  if (operation_id == "lifecycle.shutdown_database") return "SBLR_LIFECYCLE_SHUTDOWN_DATABASE";
  if (operation_id == "lifecycle.shutdown_force") return "SBLR_LIFECYCLE_SHUTDOWN_FORCE";
  if (operation_id == "lifecycle.shutdown_acknowledge") return "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE";
  if (operation_id == "lifecycle.drop_database") return "SBLR_LIFECYCLE_DROP_DATABASE";
  if (operation_id == "extensibility.register_udr_package") return "SBLR_EXTENSIBILITY_REGISTER_UDR_PACKAGE";
  if (operation_id == "extensibility.load_udr_package") return "SBLR_EXTENSIBILITY_LOAD_UDR_PACKAGE";
  if (operation_id == "extensibility.unload_udr_package") return "SBLR_EXTENSIBILITY_UNLOAD_UDR_PACKAGE";
  if (operation_id == "extensibility.inspect_udr_packages") return "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES";
  if (operation_id == "extensibility.invoke_udr_package") return "SBLR_UDR_INVOKE";
  if (operation_id == "ddl.create_schema") return "SBLR_DDL_CREATE_SCHEMA";
  if (operation_id == "ddl.create_index") return "SBLR_DDL_CREATE_INDEX";
  if (operation_id == "ddl.create_index_template") return "SBLR_DDL_CREATE_INDEX_TEMPLATE";
  if (operation_id == "ddl.create_domain") return "SBLR_DDL_CREATE_DOMAIN";
  if (operation_id == "ddl.create_sequence") return "SBLR_DDL_CREATE_SEQUENCE";
  if (operation_id == "ddl.create_statistics") return "SBLR_DDL_CREATE_STATISTICS";
  if (operation_id == "ddl.create_view") return "SBLR_DDL_CREATE_VIEW";
  if (operation_id == "ddl.create_function") return "SBLR_DDL_CREATE_FUNCTION";
  if (operation_id == "ddl.create_procedure") return "SBLR_DDL_CREATE_PROCEDURE";
  if (operation_id == "ddl.create_trigger") return "SBLR_DDL_CREATE_TRIGGER";
  if (operation_id == "ddl.constraint.create") return "SBLR_DDL_CONSTRAINT_CREATE";
  if (operation_id == "ddl.constraint.alter") return "SBLR_DDL_CONSTRAINT_ALTER";
  if (operation_id == "ddl.constraint.drop") return "SBLR_DDL_CONSTRAINT_DROP";
  if (operation_id == "ddl.alter_object") return "SBLR_DDL_ALTER_OBJECT";
  if (operation_id == "ddl.drop_object") return "SBLR_DDL_DROP_OBJECT";
  if (operation_id == "ddl.comment_on_object") return "SBLR_DDL_COMMENT_ON_OBJECT";
  if (operation_id == "transaction.begin") return "SBLR_TRANSACTION_BEGIN";
  if (operation_id == "transaction.set_characteristics") return "SBLR_TRANSACTION_SET_CHARACTERISTICS";
  if (operation_id == "transaction.commit") return "SBLR_TRANSACTION_COMMIT";
  if (operation_id == "transaction.rollback") return "SBLR_TRANSACTION_ROLLBACK";
  if (operation_id == "transaction.create_savepoint") return "SBLR_TRANSACTION_CREATE_SAVEPOINT";
  if (operation_id == "transaction.release_savepoint") return "SBLR_TRANSACTION_RELEASE_SAVEPOINT";
  if (operation_id == "transaction.rollback_to_savepoint") return "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT";
  if (operation_id == "transaction.execute_block") return "SBLR_TRANSACTION_EXECUTE_BLOCK";
  if (operation_id == "transaction.lock_table") return "SBLR_TXN_LOCK_TABLE";
  if (operation_id == "transaction.unlock_table") return "SBLR_TXN_UNLOCK_TABLE";
  if (operation_id == "transaction.lock_named") return "SBLR_TXN_LOCK_NAMED";
  if (operation_id == "transaction.unlock_named") return "SBLR_TXN_UNLOCK_NAMED";
  if (operation_id == "security.role.create") return "SBLR_SEC_CREATE_ROLE";
  if (operation_id == "security.role.drop") return "SBLR_SEC_DROP_ROLE";
  if (operation_id == "security.group.create") return "SBLR_SEC_CREATE_GROUP";
  if (operation_id == "security.group.drop") return "SBLR_SEC_DROP_GROUP";
  if (operation_id == "security.principal.create") return "SBLR_SECURITY_PRINCIPAL_CREATE";
  if (operation_id == "security.principal.alter") return "SBLR_SECURITY_PRINCIPAL_ALTER";
  if (operation_id == "security.membership.grant") return "SBLR_SECURITY_MEMBERSHIP_GRANT";
  if (operation_id == "security.membership.revoke") return "SBLR_SECURITY_MEMBERSHIP_REVOKE";
  if (operation_id == "security.privilege.grant") return "SBLR_SECURITY_PRIVILEGE_GRANT";
  if (operation_id == "security.privilege.revoke") return "SBLR_SECURITY_PRIVILEGE_REVOKE";
  if (operation_id == "security.session.set_role") return "SBLR_SECURITY_SESSION_SET_ROLE";
  if (operation_id == "security.policy.create") return "SBLR_SECURITY_POLICY_CREATE";
  if (operation_id == "security.policy.alter") return "SBLR_SECURITY_POLICY_ALTER";
  if (operation_id == "security.policy.drop" ||
      operation_id == "security.policy.lifecycle_drop") return "SBLR_SECURITY_POLICY_DROP";
  if (operation_id == "security.mask.drop") return "SBLR_SECURITY_MASK_DROP";
  if (operation_id == "security.rls.drop") return "SBLR_SECURITY_RLS_DROP";
  if (operation_id == "security.policy.attach") return "SBLR_SECURITY_POLICY_ATTACH";
  if (operation_id == "security.policy.activate") return "SBLR_SECURITY_POLICY_ACTIVATE";
  if (operation_id == "security.policy.deactivate") return "SBLR_SECURITY_POLICY_DEACTIVATE";
  if (operation_id == "security.policy.validate") return "SBLR_SECURITY_POLICY_VALIDATE";
  if (operation_id == "security.policy.show") return "SBLR_SECURITY_POLICY_SHOW";
  return nullptr;
}

bool EqualsAsciiInsensitive(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
        std::tolower(static_cast<unsigned char>(rhs[index]))) {
      return false;
    }
  }
  return true;
}

std::array<std::uint8_t, 16> ServerVirtualSyntheticUuid(std::string_view normalized_name) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char ch : normalized_name) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  std::array<std::uint8_t, 16> uuid{};
  for (int i = 0; i < 8; ++i) {
    uuid[static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((hash >> (i * 8)) & 0xffu);
    uuid[static_cast<std::size_t>(8 + i)] =
        static_cast<std::uint8_t>(((hash ^ 0xa5a5a5a5a5a5a5a5ull) >> (i * 8)) & 0xffu);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

std::string ServerVirtualProjectionFromName(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  if (value == "sys.version") { return "sys.version"; }
  if (value == "sys.catalog" ||
      value == "sys.catalog_readable.object_tree") {
    return "sys.catalog_readable.object_tree";
  }
  const std::string canonical = engine_api::SysInformationCanonicalViewPath(value);
  if (engine_api::FindSysInformationProjectionDefinition(canonical) != nullptr) {
    return canonical;
  }
  return {};
}

bool ContainsAsciiInsensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || needle.size() > haystack.size()) return false;
  for (std::size_t offset = 0; offset + needle.size() <= haystack.size(); ++offset) {
    if (EqualsAsciiInsensitive(haystack.substr(offset, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

std::string ServerVirtualProjectionFromUuid(std::string_view target_uuid) {
  if (target_uuid.empty()) { return {}; }
  if (EqualsAsciiInsensitive(
          target_uuid,
          "b4a0fd27-e19b-7719-9105-5882443ee2bc")) {
    return "sys.version";
  }
  if (EqualsAsciiInsensitive(
          target_uuid,
          UuidBytesToText(ServerVirtualSyntheticUuid("sys.version")))) {
    return "sys.version";
  }
  if (EqualsAsciiInsensitive(
          target_uuid,
          UuidBytesToText(ServerVirtualSyntheticUuid("sys.catalog")))) {
    return "sys.catalog_readable.object_tree";
  }
  for (const auto& definition : engine_api::BuiltinSysInformationProjectionDefinitions()) {
    if (EqualsAsciiInsensitive(
            target_uuid,
            UuidBytesToText(ServerVirtualSyntheticUuid(definition.view_path)))) {
      return definition.view_path;
    }
    static constexpr std::string_view kCanonicalPrefix = "sys.information.";
    if (definition.view_path.rfind(kCanonicalPrefix, 0) == 0) {
      const std::string legacy_alias =
          "sys.information_schema." + definition.view_path.substr(kCanonicalPrefix.size());
      if (EqualsAsciiInsensitive(
              target_uuid,
              UuidBytesToText(ServerVirtualSyntheticUuid(legacy_alias)))) {
        return definition.view_path;
      }
    }
  }
  return {};
}

std::string ServerVirtualProjectionForTarget(std::string_view encoded) {
  constexpr std::string_view kUuidFields[] = {
      "target_object_uuid",
      "source_relation_uuid",
      "relation_uuid",
      "object_uuid"};
  for (const auto field : kUuidFields) {
    const std::string target_uuid = JsonTextField(encoded, field).value_or(
        TextLineValue(encoded, field).value_or(""));
    if (target_uuid.empty()) { continue; }
    if (const std::string projection = ServerVirtualProjectionFromUuid(target_uuid);
        !projection.empty()) {
      return projection;
    }
  }

  if (ContainsAsciiInsensitive(encoded, "b4a0fd27-e19b-7719-9105-5882443ee2bc")) {
    return "sys.version";
  }
  for (const std::string_view virtual_name : {"sys.version", "sys.catalog"}) {
    const std::string virtual_uuid =
        UuidBytesToText(ServerVirtualSyntheticUuid(virtual_name));
    if (!ContainsAsciiInsensitive(encoded, virtual_uuid)) { continue; }
    if (const std::string projection = ServerVirtualProjectionFromUuid(virtual_uuid);
        !projection.empty()) {
      return projection;
    }
  }
  for (const auto& definition : engine_api::BuiltinSysInformationProjectionDefinitions()) {
    const std::string virtual_uuid =
        UuidBytesToText(ServerVirtualSyntheticUuid(definition.view_path));
    if (ContainsAsciiInsensitive(encoded, virtual_uuid)) { return definition.view_path; }
    static constexpr std::string_view kCanonicalPrefix = "sys.information.";
    if (definition.view_path.rfind(kCanonicalPrefix, 0) == 0) {
      const std::string legacy_alias =
          "sys.information_schema." + definition.view_path.substr(kCanonicalPrefix.size());
      const std::string alias_uuid =
          UuidBytesToText(ServerVirtualSyntheticUuid(legacy_alias));
      if (ContainsAsciiInsensitive(encoded, alias_uuid)) { return definition.view_path; }
    }
  }

  constexpr std::string_view kNameFields[] = {
      "target_name",
      "target_object_name",
      "target_object_path",
      "source_relation",
      "source_relation_name",
      "source_relation_path",
      "object_path",
      "relation_path"};
  for (const auto field : kNameFields) {
    const std::string target_name = JsonTextField(encoded, field).value_or(
        TextLineValue(encoded, field).value_or(""));
    if (target_name.empty()) { continue; }
    if (const std::string projection = ServerVirtualProjectionFromName(target_name);
        !projection.empty()) {
      return projection;
    }
  }

  return {};
}

scratchbird::engine::SblrOperationFamily PublicAbiFamilyForServerFamily(std::string_view family) {
  using scratchbird::engine::SblrOperationFamily;
  if (family == "sblr.transaction.control.v3") return SblrOperationFamily::transaction_control;
  if (family == "sblr.query.relational.v3") return SblrOperationFamily::relational_query;
  if (family == "sblr.query.kv.v3") return SblrOperationFamily::structured_kv;
  if (family == "sblr.query.document.v3") return SblrOperationFamily::document;
  if (family == "sblr.query.graph.v3") return SblrOperationFamily::graph;
  if (family == "sblr.query.search.v3") return SblrOperationFamily::search;
  if (family == "sblr.query.vector.v3") return SblrOperationFamily::vector;
  if (family == "sblr.query.timeseries.v3") return SblrOperationFamily::timeseries;
  if (family == "sblr.dml.insert.v3") return SblrOperationFamily::dml_insert;
  if (family == "sblr.dml.update.v3") return SblrOperationFamily::dml_update;
  if (family == "sblr.dml.delete.v3") return SblrOperationFamily::dml_delete;
  if (family == "sblr.dml.merge.v3") return SblrOperationFamily::dml_merge;
  if (family == "sblr.bulk.import.v3") return SblrOperationFamily::bulk_import;
  if (family == "sblr.bulk.export.v3") return SblrOperationFamily::bulk_export;
  if (family == "sblr.catalog.mutation.v3") return SblrOperationFamily::catalog_mutation;
  if (family == "sblr.security.mutation.v3") return SblrOperationFamily::security_mutation;
  if (family == "sblr.policy.operation.v3") return SblrOperationFamily::security_mutation;
  if (family == "sblr.language.resource_control.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.management.control.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.management.report.v3") return SblrOperationFamily::management_inspect;
  if (family == "sblr.metrics.read.v3") return SblrOperationFamily::metrics_inspect;
  if (family == "sblr.mga.control.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.mga.report.v3") return SblrOperationFamily::management_inspect;
  if (family == "sblr.filespace.management.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.migration.operation.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.index.maintenance.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.database.management.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.backup.operation.v3") return SblrOperationFamily::replication_operation;
  if (family == "sblr.archive.operation.v3") return SblrOperationFamily::replication_operation;
  if (family == "sblr.replication.operation.v3") return SblrOperationFamily::replication_operation;
  if (family == "sblr.replication.consumer.v3") return SblrOperationFamily::replication_operation;
  if (family == "sblr.acceleration.gpu.v3") return SblrOperationFamily::acceleration_management;
  if (family == "sblr.acceleration.llvm.v3") return SblrOperationFamily::acceleration_management;
  if (family == "sblr.cluster.control.v3") return SblrOperationFamily::cluster_placement;
  if (family == "sblr.cluster.report.v3") return SblrOperationFamily::cluster_placement;
  return SblrOperationFamily::management_inspect;
}

bool OperationNeedsTransactionContext(std::string_view operation_id) {
  return operation_id.starts_with("dml.") ||
         operation_id == "bridge.begin" ||
         operation_id == "bridge.commit" ||
         operation_id == "bridge.rollback" ||
         operation_id == "bridge.prepare" ||
         operation_id == "bridge.savepoint" ||
         operation_id == "bridge.execute" ||
         operation_id.starts_with("bridge.cursor_") ||
         operation_id.starts_with("bridge.stream_") ||
         operation_id.starts_with("bridge.cdc_") ||
         operation_id == "bridge.proxy_route" ||
         operation_id == "bridge.compare_result" ||
         operation_id == "bridge.cutover" ||
         operation_id == "bridge.cluster_route" ||
         operation_id == "bridge.cluster.distributed_query" ||
         operation_id == "bridge.cluster.cross_node_query" ||
         operation_id == "ddl.create_schema" ||
         operation_id == "ddl.create_table" ||
         operation_id == "ddl.create_index" ||
         operation_id == "ddl.create_index_template" ||
         operation_id == "ddl.create_domain" ||
         operation_id == "ddl.create_sequence" ||
         operation_id == "ddl.create_statistics" ||
         operation_id == "ddl.create_view" ||
         operation_id == "ddl.create_function" ||
         operation_id == "ddl.create_procedure" ||
         operation_id == "ddl.create_trigger" ||
         operation_id == "ddl.alter_object" ||
         operation_id == "ddl.drop_object" ||
         operation_id == "ddl.comment_on_object" ||
         operation_id.starts_with("ddl.constraint.") ||
         operation_id == "query.evaluate_projection" ||
         operation_id == "query.plan_operation" ||
         operation_id == "op.migration.begin_from_reference" ||
         operation_id == "op.migration.alter" ||
         (operation_id != "transaction.begin" &&
          operation_id != "transaction.set_characteristics" &&
          operation_id.starts_with("transaction.")) ||
         operation_id.starts_with("extensibility.register_udr_package") ||
         operation_id.starts_with("extensibility.load_udr_package") ||
         operation_id.starts_with("extensibility.unload_udr_package") ||
         operation_id.starts_with("extensibility.inspect_udr_packages") ||
         operation_id.starts_with("extensibility.invoke_udr_package") ||
         operation_id == "security.membership.grant" ||
         operation_id == "security.membership.revoke" ||
         operation_id == "security.privilege.grant" ||
         operation_id == "security.privilege.revoke" ||
         operation_id == "security.role.create" ||
         operation_id == "security.role.drop" ||
         operation_id == "security.group.create" ||
         operation_id == "security.group.drop" ||
         operation_id == "security.principal.create" ||
         operation_id == "security.principal.alter" ||
         operation_id == "security.policy.create" ||
         operation_id == "security.policy.alter" ||
         operation_id == "security.policy.drop" ||
         operation_id == "security.policy.lifecycle_drop" ||
         operation_id == "security.mask.drop" ||
         operation_id == "security.rls.drop" ||
         operation_id == "security.policy.attach" ||
         operation_id == "security.policy.activate" ||
         operation_id == "security.policy.deactivate" ||
         operation_id == "event.channel.create" ||
         operation_id == "event.channel.listen" ||
         operation_id == "event.channel.unlisten" ||
         operation_id == "event.channel.notify" ||
         operation_id == "event.subscription.list" ||
         operation_id == "event.delivery.poll" ||
         operation_id == "event.delivery.ack";
}

bool IsTransactionOperation(std::string_view operation_id) {
  return operation_id.starts_with("transaction.");
}

bool TransactionBeginRequestsReadOnly(std::string_view encoded) {
  return encoded.find("read_only:true") != std::string_view::npos ||
         encoded.find("transaction_read_only=true") != std::string_view::npos ||
         encoded.find("\"read_only\":true") != std::string_view::npos ||
         encoded.find("\"read_only\": true") != std::string_view::npos;
}

std::optional<std::string> SavepointOperandForDispatch(std::string_view encoded,
                                                       std::string_view operation_id) {
  if (operation_id != "transaction.create_savepoint" &&
      operation_id != "transaction.release_savepoint" &&
      operation_id != "transaction.rollback_to_savepoint") {
    return std::nullopt;
  }
  if (const auto value = JsonTextField(encoded, "savepoint_name");
      value.has_value() && !value->empty()) {
    return value;
  }
  if (const auto value = TextLineValue(encoded, "savepoint_name");
      value.has_value() && !value->empty()) {
    return value;
  }
  return std::nullopt;
}

bool SessionHasTraceTag(const ServerSessionRecord& session, std::string_view tag) {
  for (const auto& candidate : session.engine_authorization_trace_tags) {
    if (candidate == tag) return true;
  }
  return false;
}

const HostedDatabaseSnapshot* HostedDatabaseForSession(const HostedEngineState& engine_state,
                                                       const ServerSessionRecord& session) {
  for (const auto& database : engine_state.databases) {
    if (!database.database_open) continue;
    const bool path_matches = !session.database_path.empty() &&
                              database.database_path == session.database_path;
    const bool uuid_matches = !session.database_uuid.empty() &&
                              database.database_uuid == session.database_uuid;
    if (path_matches || uuid_matches) return &database;
  }
  return nullptr;
}

std::string EngineEvidenceValue(const engine_api::EngineApiResult& result,
                                std::string_view evidence_kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == evidence_kind) return evidence.evidence_id;
  }
  return {};
}

std::string JoinUuidList(const std::vector<std::array<std::uint8_t, 16>>& values) {
  std::string out;
  for (const auto& value : values) {
    if (sbps::IsZeroUuid(value)) { continue; }
    if (!out.empty()) { out.push_back(';'); }
    out += UuidBytesToText(value);
  }
  return out;
}

void ApplyBeginTransactionResultToSession(
    const engine_api::EngineBeginTransactionResult& result,
    ServerSessionRecord* session) {
  if (session == nullptr) return;
  session->local_transaction_id = result.local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = result.transaction_uuid.canonical;
  session->transaction_timestamp = EngineEvidenceValue(result, "transaction_timestamp");
}

void ApplyAutocommitBoundaryResultToSession(
    const engine_api::EngineAutocommitBoundaryResult& result,
    ServerSessionRecord* session) {
  if (session == nullptr || result.replacement_local_transaction_id == 0) return;
  session->local_transaction_id = result.replacement_local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      result.replacement_snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = result.replacement_transaction_uuid.canonical;
  session->transaction_timestamp = result.replacement_transaction_timestamp;
}

engine_api::EngineRequestContext ReplacementTransactionContext(
    const ServerSessionRecord& session,
    const HostedDatabaseSnapshot& database,
    const std::array<std::uint8_t, 16>& request_uuid) {
  engine_api::EngineRequestContext context;
  context.trust_mode = session.embedded_in_process
                           ? engine_api::EngineTrustMode::embedded_in_process
                           : engine_api::EngineTrustMode::server_isolated;
  context.request_id = UuidBytesToText(request_uuid);
  context.database_path = session.database_path.empty() ? database.database_path : session.database_path;
  context.database_uuid.canonical =
      session.database_uuid.empty() ? database.database_uuid : session.database_uuid;
  context.statement_uuid.canonical = context.request_id;
  context.statement_timestamp = CurrentUtcTimestampText();
  context.current_timestamp = context.statement_timestamp;
  context.current_monotonic_ns = CurrentMonotonicNsText();
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  if (!sbps::IsZeroUuid(session.active_role_uuid)) {
    context.current_role_uuid.canonical = UuidBytesToText(session.active_role_uuid);
  }
  context.application_name = session.application_name;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.read_only_mode =
      session.attach_mode == "read_only" || session.default_transaction_read_only ||
      database.read_only || database.state == HostedDatabaseState::kReadOnly;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  PopulateEngineLanguageContextFromSession(session, &context.language_context);
  context.trace_tags = session.engine_authorization_trace_tags;
  context.trace_tags.push_back("sb_server.sblr_dispatch.always_active_replacement");
  return context;
}

engine_api::EngineRequestContext ActiveTransactionContext(
    const ServerSessionRecord& session,
    const HostedDatabaseSnapshot& database,
    const std::array<std::uint8_t, 16>& request_uuid) {
  auto context = ReplacementTransactionContext(session, database, request_uuid);
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.local_transaction_id = session.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  context.transaction_timestamp = session.transaction_timestamp;
  context.transaction_isolation_level = session.default_transaction_isolation_level;
  context.trace_tags.push_back("sb_server.sblr_dispatch.autocommit_finality");
  return context;
}

bool BeginReplacementTransactionForSession(ServerSessionRecord* session,
                                           const HostedEngineState& engine_state,
                                           const std::array<std::uint8_t, 16>& request_uuid,
                                           std::string* diagnostic_code,
                                           std::string* diagnostic_detail) {
  if (session == nullptr) {
    if (diagnostic_code != nullptr) *diagnostic_code = "PARSER_SERVER_IPC.SESSION_REQUIRED";
    if (diagnostic_detail != nullptr) *diagnostic_detail = "session_required";
    return false;
  }
  const auto* database = HostedDatabaseForSession(engine_state, *session);
  if (database == nullptr) {
    if (diagnostic_code != nullptr) *diagnostic_code = "PARSER_SERVER_IPC.TRANSACTION_DATABASE_UNAVAILABLE";
    if (diagnostic_detail != nullptr) *diagnostic_detail = "database_unavailable";
    return false;
  }
  engine_api::EngineBeginTransactionRequest begin;
  begin.context = ReplacementTransactionContext(*session, *database, request_uuid);
  begin.isolation_level = session->default_transaction_isolation_level;
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_only:") + (begin.context.read_only_mode ? "true" : "false"));
  begin.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_mode:") + (begin.context.read_only_mode ? "read_only" : "read_write"));
  const auto replacement = engine_api::EngineBeginTransaction(begin);
  if (!replacement.ok || replacement.local_transaction_id == 0) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = replacement.diagnostics.empty() || replacement.diagnostics.front().code.empty()
                             ? "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"
                             : replacement.diagnostics.front().code;
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = replacement.diagnostics.empty() || replacement.diagnostics.front().detail.empty()
                               ? "replacement_transaction_begin_failed"
                               : replacement.diagnostics.front().detail;
    }
    return false;
  }
  ApplyBeginTransactionResultToSession(replacement, session);
  return true;
}

struct AutocommitBoundaryResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string evidence;
};

AutocommitBoundaryResult FinalizeAutocommitBoundaryForSession(
    ServerSessionRecord* session,
    const HostedEngineState& engine_state,
    const std::array<std::uint8_t, 16>& request_uuid,
    bool statement_succeeded) {
  AutocommitBoundaryResult result;
  if (session == nullptr) {
    result.diagnostic_code = "PARSER_SERVER_IPC.SESSION_REQUIRED";
    result.diagnostic_detail = "session_required";
    return result;
  }
  const auto* database = HostedDatabaseForSession(engine_state, *session);
  if (database == nullptr) {
    result.diagnostic_code = "PARSER_SERVER_IPC.TRANSACTION_DATABASE_UNAVAILABLE";
    result.diagnostic_detail = "database_unavailable";
    return result;
  }
  if (session->local_transaction_id == 0) {
    result.diagnostic_code = "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED";
    result.diagnostic_detail = "active_transaction_required";
    return result;
  }

  const std::uint64_t finalized_local_transaction_id = session->local_transaction_id;
  engine_api::EngineAutocommitBoundaryRequest boundary;
  boundary.context = ActiveTransactionContext(*session, *database, request_uuid);
  boundary.statement_succeeded = statement_succeeded;
  boundary.replacement_isolation_level = session->default_transaction_isolation_level;
  boundary.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  boundary.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_only:") +
      ((session->attach_mode == "read_only" || session->default_transaction_read_only ||
        database->read_only || database->state == HostedDatabaseState::kReadOnly)
           ? "true"
           : "false"));
  boundary.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_mode:") +
      ((session->attach_mode == "read_only" || session->default_transaction_read_only ||
        database->read_only || database->state == HostedDatabaseState::kReadOnly)
           ? "read_only"
           : "read_write"));
  const auto finalized = engine_api::EngineAutocommitBoundary(boundary);
  if (!finalized.ok) {
    ApplyAutocommitBoundaryResultToSession(finalized, session);
    result.diagnostic_code =
        finalized.diagnostics.empty() || finalized.diagnostics.front().code.empty()
            ? "PARSER_SERVER_IPC.AUTOCOMMIT_FINALITY_FAILED"
            : finalized.diagnostics.front().code;
    result.diagnostic_detail =
        finalized.diagnostics.empty() || finalized.diagnostics.front().detail.empty()
            ? (statement_succeeded ? "autocommit_commit_failed"
                                   : "autocommit_rollback_failed")
            : finalized.diagnostics.front().detail;
    return result;
  }
  ApplyAutocommitBoundaryResultToSession(finalized, session);

  result.ok = true;
  result.evidence += statement_succeeded
                         ? "evidence=autocommit_statement_succeeded:committed\n"
                         : "evidence=autocommit_statement_failed:rolled_back\n";
  result.evidence += "autocommit_finalized_local_transaction_id=" +
                     std::to_string(finalized_local_transaction_id) + "\n";
  result.evidence += "replacement_local_transaction_id=" +
                     std::to_string(session->local_transaction_id) + "\n";
  result.evidence += "replacement_snapshot_visible_through_local_transaction_id=" +
                     std::to_string(session->snapshot_visible_through_local_transaction_id) + "\n";
  if (!session->transaction_uuid.empty()) {
    result.evidence += "replacement_transaction_uuid=" + session->transaction_uuid + "\n";
  }
  result.evidence += "evidence=always_active_transaction_replacement:" +
                     std::to_string(session->local_transaction_id) + "\n";
  result.evidence += "evidence=parser_finality:false\n";
  for (const auto& item : finalized.evidence) {
    result.evidence += "evidence=" + item.evidence_kind + ":" + item.evidence_id + "\n";
  }
  return result;
}

bool AutocommitEmulationRequested(std::string_view encoded) {
  return JsonBoolField(encoded, "autocommit_emulation", false) ||
         TextBoolField(encoded, "autocommit_emulation", false);
}

std::string SessionTransactionStatePayload(std::string_view operation_id,
                                           const ServerSessionRecord& session,
                                           std::string_view state) {
  std::ostringstream out;
  out << "operation_id=" << operation_id << "\n"
      << "result_kind=transaction.state.v1\n"
      << "row_count=0\n"
      << "evidence=transaction_state:active\n"
      << "evidence=transaction_adoption:" << state << "\n"
      << "evidence=always_active_session:true\n"
      << "local_transaction_id=" << session.local_transaction_id << "\n"
      << "snapshot_visible_through_local_transaction_id="
      << session.snapshot_visible_through_local_transaction_id << "\n";
  if (!session.transaction_uuid.empty()) {
    out << "transaction_uuid=" << session.transaction_uuid << "\n";
  }
  if (!session.transaction_timestamp.empty()) {
    out << "transaction_timestamp=" << session.transaction_timestamp << "\n";
  }
  return out.str();
}

void AppendReplacementTransactionState(std::string* payload,
                                       const ServerSessionRecord& session) {
  if (payload == nullptr) return;
  if (!payload->empty() && payload->back() != '\n') payload->push_back('\n');
  *payload += "replacement_local_transaction_id=" + std::to_string(session.local_transaction_id) + "\n";
  *payload += "replacement_snapshot_visible_through_local_transaction_id=" +
              std::to_string(session.snapshot_visible_through_local_transaction_id) + "\n";
  if (!session.transaction_uuid.empty()) {
    *payload += "replacement_transaction_uuid=" + session.transaction_uuid + "\n";
  }
  if (!session.transaction_timestamp.empty()) {
    *payload += "replacement_transaction_timestamp=" + session.transaction_timestamp + "\n";
  }
  *payload += "evidence=always_active_transaction_replacement:" +
              std::to_string(session.local_transaction_id) + "\n";
}

void AppendTransactionPressureRestartEvidence(std::string* payload,
                                              std::string_view encoded,
                                              const ServerSessionRecord& session) {
  const bool pressure_restart = JsonBoolField(encoded, "transaction_pressure_restart", false) ||
                                TextBoolField(encoded, "transaction_pressure_restart", false);
  if (payload == nullptr || !pressure_restart) return;
  if (!payload->empty() && payload->back() != '\n') payload->push_back('\n');
  *payload += "diagnostic_code=SERVER.TRANSACTION_PRESSURE.RESTART_FORCED\n";
  *payload += "diagnostic_detail=long_idle_transaction_forced_restart\n";
  *payload += "evidence=transaction_pressure_restart:forced\n";
  *payload += "evidence=transaction_pressure_policy:long_idle_restart\n";
  *payload += "evidence=parser_finality:false\n";
  *payload += "pressure_replacement_local_transaction_id=" +
              std::to_string(session.local_transaction_id) + "\n";
}

SessionOperationResult TransactionAdmissionFailure(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string code,
    std::string message,
    std::string detail) {
  std::vector<ServerDiagnostic> diagnostics;
  diagnostics.push_back(SblrServerDiagnostic(code, message, detail));
  if (code != "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED") {
    diagnostics.push_back(SblrServerDiagnostic(
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission failed after attachment admission.",
        detail));
  }
  return FailureWithDiagnostics(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                                kSchemaExecuteResultTestV1,
                                session_uuid,
                                std::move(diagnostics),
                                std::move(detail));
}

std::optional<SessionOperationResult> ValidateTransactionAdmission(
    const HostedEngineState& engine_state,
    const ServerSessionRecord& session,
    const ServerSblrAdmissionResult& admission,
    std::string_view encoded) {
  if (!IsTransactionOperation(admission.operation_id)) return std::nullopt;

  const auto* database = HostedDatabaseForSession(engine_state, session);
  if (database == nullptr) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "PARSER_SERVER_IPC.TRANSACTION_DATABASE_UNAVAILABLE",
        "Transaction admission requires an open hosted database bound to the session.",
        "database_unavailable");
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
        "Standalone transaction admission cannot use cluster lifecycle authority.",
        "cluster_authority_unavailable");
  }
  if (database->state == HostedDatabaseState::kMaintenance ||
      database->state == HostedDatabaseState::kRestrictedOpen ||
      database->state == HostedDatabaseState::kQuarantined ||
      database->state == HostedDatabaseState::kFailed ||
      database->state == HostedDatabaseState::kDetached ||
      database->state == HostedDatabaseState::kNotConfigured ||
      database->state == HostedDatabaseState::kOpening) {
    const std::string detail = database->state == HostedDatabaseState::kMaintenance
        ? "maintenance_transaction_admission_fenced"
        : database->state == HostedDatabaseState::kRestrictedOpen
            ? "restricted_open_transaction_admission_fenced"
            : "database_lifecycle_state_not_transaction_admissible";
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "The hosted database lifecycle state does not admit ordinary transactions.",
        detail);
  }
  if (sbps::IsZeroUuid(session.effective_user_uuid) ||
      sbps::IsZeroUuid(session.session_uuid) ||
      session.database_path.empty() ||
      session.database_uuid.empty()) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission requires a complete attached session identity.",
        "session_identity_required");
  }
  if (session.catalog_generation == 0 ||
      session.security_epoch == 0 ||
      session.policy_generation == 0 ||
      session.name_resolution_epoch == 0 ||
      session.resource_epoch == 0) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission requires current catalog security policy name and resource generations.",
        "authority_generation_required");
  }
  if (SessionHasTraceTag(session, "tx_admission:policy_stale") ||
      SessionHasTraceTag(session, "tx_admission:catalog_stale")) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission refused a stale policy or catalog epoch.",
        "authority_epoch_stale");
  }
  if (SessionHasTraceTag(session, "tx_admission:filespace_unavailable")) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission requires an available filespace.",
        "filespace_unavailable");
  }
  if (SessionHasTraceTag(session, "tx_admission:memory_denied")) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission requires memory policy admission.",
        "memory_admission_denied");
  }
  if (SessionHasTraceTag(session, "tx_admission:lock_denied")) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction admission requires lock admission.",
        "lock_admission_denied");
  }

  if (admission.operation_id == "transaction.begin") {
    const bool read_only_request =
        session.default_transaction_read_only ||
        TransactionBeginRequestsReadOnly(encoded);
    if (session.local_transaction_id != 0) {
      if ((database->write_admission_fenced ||
           database->read_only ||
           database->state == HostedDatabaseState::kReadOnly ||
           session.attach_mode == "read_only") &&
          !read_only_request) {
        return TransactionAdmissionFailure(
            session.session_uuid,
            "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
            "Write-capable transaction admission is fenced for this attachment.",
            "write_admission_fenced");
      }
      return std::nullopt;
    }
    if ((database->write_admission_fenced ||
         database->read_only ||
         database->state == HostedDatabaseState::kReadOnly ||
         session.attach_mode == "read_only") &&
        !read_only_request) {
      return TransactionAdmissionFailure(
          session.session_uuid,
          "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
          "Write-capable transaction admission is fenced for this attachment.",
          "write_admission_fenced");
    }
    return std::nullopt;
  }

  if (admission.operation_id == "transaction.set_characteristics") {
    return std::nullopt;
  }

  if (session.local_transaction_id == 0) {
    return TransactionAdmissionFailure(
        session.session_uuid,
        "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
        "Transaction operation requires an active admitted transaction.",
        "active_transaction_required");
  }
  return std::nullopt;
}

std::string PublicAbiEnvelopeForDispatch(const ServerSessionRecord& session,
                                         std::string_view encoded,
                                         std::string_view operation_id,
                                         std::string_view operation_family) {
  std::string_view dispatch_operation_id = operation_id;
  std::string_view dispatch_operation_family = operation_family;
  std::string virtual_projection;
  if (operation_id == "dml.select_rows") {
    virtual_projection = ServerVirtualProjectionForTarget(encoded);
  }
  if (LooksLikeBinarySblrEnvelope(encoded) && virtual_projection.empty()) {
    return std::string(encoded);
  }
  if (operation_id == "dml.select_rows" && virtual_projection == "sys.version") {
    dispatch_operation_id = "observability.show_version";
    dispatch_operation_family = "sblr.management.report.v3";
  } else if (operation_id == "dml.select_rows" && !virtual_projection.empty()) {
    dispatch_operation_id = "observability.show_catalog";
    dispatch_operation_family = "sblr.catalog.introspect.v3";
  }
  const char* opcode = PublicAbiOpcodeForOperation(dispatch_operation_id);
  if (opcode == nullptr) return std::string(encoded);

  const auto phase_start = ServerSteadyClock::now();
  auto phase_last = phase_start;
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(8);
  const auto mark_phase = [&](std::string phase) {
    const auto now = ServerSteadyClock::now();
    phase_micros.push_back(
        {std::move(phase), ServerElapsedMicros(phase_last, now)});
    phase_last = now;
  };

  std::string operation_envelope;
  operation_envelope += "operation_id=";
  operation_envelope += dispatch_operation_id;
  operation_envelope += "\n";
  operation_envelope += "opcode=";
  operation_envelope += opcode;
  operation_envelope += "\n";
  operation_envelope += "sblr_operation_family=";
  operation_envelope += dispatch_operation_family;
  operation_envelope += "\n";
  operation_envelope += "result_shape=engine.api.result.v1\n";
  operation_envelope += "diagnostic_shape=engine.diagnostic.v1\n";
  operation_envelope += "trace_key=sbsql.parser.full_route.transaction\n";
  operation_envelope += "contains_sql_text=false\n";
  operation_envelope += "parser_resolved_names_to_uuids=true\n";
  operation_envelope += "requires_security_context=true\n";
  operation_envelope += OperationNeedsTransactionContext(dispatch_operation_id)
                            ? "requires_transaction_context=true\n"
                            : "requires_transaction_context=false\n";
  operation_envelope += IsClusterDispatchOperation(dispatch_operation_id) ||
                                dispatch_operation_family == "sblr.cluster.control.v3" ||
                                dispatch_operation_family == "sblr.cluster.report.v3" ||
                                dispatch_operation_family == "sblr.replication.consumer.v3"
                            ? "requires_cluster_authority=true\n"
                            : "requires_cluster_authority=false\n";
  mark_phase("header");
  if (!virtual_projection.empty()) {
    operation_envelope += "operand=text\tprojection\t";
    operation_envelope += EscapeOperationOperandField(virtual_projection);
    operation_envelope += "\n";
    operation_envelope += "operand=text\tcatalog_projection\t";
    operation_envelope += EscapeOperationOperandField(virtual_projection);
    operation_envelope += "\n";
    constexpr std::string_view kVirtualQueryFields[] = {
        "result_projection",
        "aggregate_function",
        "assertion_id",
        "actual_column_name",
        "expected_column_name",
        "expected_count",
        "expected_value",
        "count_compare_op",
        "count_compare_value",
        "predicate_kind",
        "predicate_column",
        "predicate_value",
        "predicate_value_type",
        "additional_predicate_kind",
        "additional_predicate_column",
        "additional_predicate_value",
        "additional_predicate_value_type",
        "subquery_projection",
        "subquery_select_column",
        "subquery_predicate_kind",
        "subquery_predicate_column",
        "subquery_predicate_value",
        "subquery_predicate_value_type",
        "subquery_additional_predicate_kind",
        "subquery_additional_predicate_column",
        "subquery_additional_predicate_value",
        "subquery_additional_predicate_value_type",
        "subquery_nested_projection",
        "subquery_nested_select_column",
        "subquery_nested_predicate_kind",
        "subquery_nested_predicate_column",
        "subquery_nested_predicate_value",
        "subquery_nested_predicate_value_type"};
    for (const auto field : kVirtualQueryFields) {
      const std::string value = EncodedTextField(encoded, std::string(field));
      if (!value.empty()) {
        AppendOperationOperand(&operation_envelope, field, value);
      }
    }
  }
  if (dispatch_operation_id == "observability.show_catalog" && virtual_projection.empty()) {
    constexpr std::string_view kCatalogProjectionFields[] = {
        "projection",
        "catalog_projection",
        "result_projection",
        "aggregate_function",
        "assertion_id",
        "actual_column_name",
        "expected_column_name",
        "expected_count",
        "expected_value",
        "count_compare_op",
        "count_compare_value",
        "predicate_kind",
        "predicate_column",
        "predicate_value",
        "predicate_value_type",
        "additional_predicate_kind",
        "additional_predicate_column",
        "additional_predicate_value",
        "additional_predicate_value_type",
        "subquery_projection",
        "subquery_select_column",
        "subquery_predicate_kind",
        "subquery_predicate_column",
        "subquery_predicate_value",
        "subquery_predicate_value_type",
        "subquery_additional_predicate_kind",
        "subquery_additional_predicate_column",
        "subquery_additional_predicate_value",
        "subquery_additional_predicate_value_type",
        "subquery_nested_projection",
        "subquery_nested_select_column",
        "subquery_nested_predicate_kind",
        "subquery_nested_predicate_column",
        "subquery_nested_predicate_value",
        "subquery_nested_predicate_value_type"};
    for (const auto field : kCatalogProjectionFields) {
      const std::string value = JsonTextField(encoded, std::string(field)).value_or(
          TextLineValue(encoded, std::string(field)).value_or(""));
      if (value.empty()) { continue; }
      AppendOperationOperand(&operation_envelope, field, value);
    }
  }
  auto append_session_operand = [&operation_envelope](std::string_view name,
                                                      const std::string& value) {
    if (value.empty()) { return; }
    operation_envelope += "operand=text\t";
    operation_envelope += name;
    operation_envelope += "\t";
    operation_envelope += EscapeOperationOperandField(value);
    operation_envelope += "\n";
  };
  append_session_operand("principal_name", session.principal_claim);
  append_session_operand("requested_role_name", session.requested_role_name);
  append_session_operand("active_role_name", session.requested_role_name);
  if (!sbps::IsZeroUuid(session.active_role_uuid)) {
    append_session_operand("current_role_uuid", UuidBytesToText(session.active_role_uuid));
  }
  append_session_operand("effective_role_uuid_set", JoinUuidList(session.effective_role_uuids));
  append_session_operand("effective_group_uuid_set", JoinUuidList(session.effective_group_uuids));
  for (const auto& tag : session.engine_authorization_trace_tags) {
    append_session_operand("authorization_tag", tag);
  }
  if (const auto savepoint_name = SavepointOperandForDispatch(encoded, dispatch_operation_id)) {
    operation_envelope += "savepoint_name=";
    operation_envelope += EscapeOperationOperandField(*savepoint_name);
    operation_envelope += "\n";
    operation_envelope += "operand=text\tsavepoint_name\t";
    operation_envelope += EscapeOperationOperandField(*savepoint_name);
    operation_envelope += "\n";
  }
  const auto append_dml_operands = [&]() {
    if (dispatch_operation_id == "dml.insert_rows") {
      AppendDmlInsertValueOperands(session, encoded, &operation_envelope);
      mark_phase("dml_insert_value_operands");
    }
    constexpr std::string_view kDmlFields[] = {
        "target_object_uuid", "target_object_kind", "dml_surface_variant",
        "source_kind", "source_uuid", "source_fingerprint", "source_position",
        "routine_object_uuid", "routine_argument_count",
        "routine_argument_0_type", "routine_argument_0_binding", "routine_argument_0_value",
        "routine_argument_1_type", "routine_argument_1_binding", "routine_argument_1_value",
        "routine_argument_2_type", "routine_argument_2_binding", "routine_argument_2_value",
        "routine_argument_3_type", "routine_argument_3_binding", "routine_argument_3_value",
        "redacted_source_handle", "source_handle_sensitive", "format_family",
        "encoding", "line_ending", "delimiter", "quote", "escape",
        "header_policy", "estimated_row_count", "order_by", "order_direction", "order_nulls",
        "limit", "offset",
        "result_projection", "aggregate_function", "aggregate_source_column",
        "assertion_id", "actual_source_column", "actual_column_name",
        "expected_column_name", "expected_count", "expected_value",
        "predicate_kind", "predicate_column", "predicate_value",
        "predicate_value_type", "assignment_column", "assignment_value",
        "subquery_projection", "subquery_select_column",
        "subquery_predicate_kind", "subquery_predicate_column",
        "subquery_predicate_value", "subquery_predicate_value_type",
        "subquery_additional_predicate_kind", "subquery_additional_predicate_column",
        "subquery_additional_predicate_value", "subquery_additional_predicate_value_type",
        "subquery_nested_projection", "subquery_nested_select_column",
        "subquery_nested_predicate_kind", "subquery_nested_predicate_column",
        "subquery_nested_predicate_value", "subquery_nested_predicate_value_type",
        "assignment_value_type", "assignment_plan",
        "on_conflict_action", "conflict_target_column",
        "on_conflict_update_column", "on_conflict_update_source_column",
        "on_conflict_assignment_plan",
        "strict_bulk_load_requested", "reference_relaxed_semantics_requested",
        "bulk.allow_opaque_columns", "bulk.allow_triggers",
        "bulk.allow_foreign_keys", "bulk.target_empty_required",
        "duplicate_mode", "insert_mode", "require_generated_row_uuid", "reject_mode",
        "insert_select_source_kind", "insert_select_cte_name",
        "insert_select_counter_column", "insert_select_counter_start",
        "insert_select_counter_step", "insert_select_counter_limit",
        "insert_select_counter_predicate", "insert_select_projection_count",
        "insert_select_source_uuid_0", "insert_select_source_uuid_1",
        "insert_select_projection_0", "insert_select_projection_1",
        "insert_select_projection_2", "insert_select_projection_3",
        "insert_select_projection_4", "insert_select_projection_5",
        "insert_select_projection_6", "insert_select_projection_7",
        "insert_select_projection_8", "insert_select_projection_9",
        "reject_limit_rows", "reject_limit_percent", "reject_payload_policy",
        "native_bulk_ingest_enabled", "native_bulk_ingest",
        "result_payload_policy", "resume_policy", "reject_target_uuid", "reject_target_kind",
        "checkpoint_mode", "checkpoint_interval_rows", "checkpoint_interval_bytes",
        "checkpoint_interval_millis", "checkpoint_resume_policy", "replay_policy",
        "failure_action", "checkpoint_target_uuid", "checkpoint_target_kind",
        "require_source_fingerprint", "require_source_position"};
    for (const auto field : kDmlFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    mark_phase("dml_field_scan");
  };
  const auto finish_operation_envelope = [&]() -> std::string {
    AppendExistingOperationOperands(encoded, &operation_envelope);
    mark_phase("append_existing_operands");

    if (const char* trace_path = std::getenv("SCRATCHBIRD_PUBLIC_ABI_ENVELOPE_TRACE");
        trace_path != nullptr && trace_path[0] != '\0') {
      std::ofstream trace(trace_path, std::ios::app);
      if (trace) {
        trace << "----- " << CurrentUtcTimestampText() << " -----\n";
        trace << operation_envelope;
        if (!operation_envelope.empty() && operation_envelope.back() != '\n') {
          trace << '\n';
        }
      }
    }

    const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                            .operation(PublicAbiFamilyForServerFamily(dispatch_operation_family), 1)
                            .append_bytes(reinterpret_cast<const std::uint8_t*>(operation_envelope.data()),
                                          operation_envelope.size())
                            .encode();
    mark_phase("envelope_encode");
    WritePublicAbiPhaseTrace(dispatch_operation_id,
                             encoded,
                             phase_micros,
                             binary.size());
    return std::string(reinterpret_cast<const char*>(binary.data()), binary.size());
  };
  if (dispatch_operation_id.starts_with("dml.")) {
    mark_phase("pre_dml_bridge");
    append_dml_operands();
    return finish_operation_envelope();
  }
  if (dispatch_operation_id == "transaction.begin") {
    const std::string isolation = JsonTextField(encoded, "transaction_isolation_level").value_or(
        TextLineValue(encoded, "transaction_isolation_level").value_or(""));
    if (isolation.empty() && !session.default_transaction_isolation_level.empty()) {
      operation_envelope += "operand=text\ttransaction_isolation_level\t";
      operation_envelope += EscapeOperationOperandField(session.default_transaction_isolation_level);
      operation_envelope += "\n";
    }
    if (!TransactionBeginRequestsReadOnly(encoded)) {
      operation_envelope += "operand=text\ttransaction_read_only\t";
      operation_envelope += session.default_transaction_read_only ? "true\n" : "false\n";
      operation_envelope += "operand=text\ttransaction_read_mode\t";
      operation_envelope += session.default_transaction_read_only ? "read_only\n" : "read_write\n";
    }
  }
  if (dispatch_operation_id == "transaction.set_characteristics") {
    constexpr std::string_view kTransactionCharacteristicFields[] = {
        "transaction_read_mode",
        "transaction_read_only",
        "transaction_isolation_level"};
    for (const auto field : kTransactionCharacteristicFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id.starts_with("lifecycle.")) {
    const std::string database_name =
        JsonTextField(encoded, "database_name")
            .value_or(JsonTextField(encoded, "target_database_name").value_or(""));
    append_session_operand("database_name", database_name);
    append_session_operand("database_path",
                           DerivedLifecycleDatabasePath(session, encoded));
  }
  if (dispatch_operation_id == "lifecycle.create_database") {
    append_session_operand("resource_seed_pack_root",
                           session.resource_seed_pack_root);
    append_session_operand("policy_seed_pack_root",
                           session.policy_seed_pack_root);
  }
  if (dispatch_operation_id == "lifecycle.repair_database") {
    append_session_operand("repair_plan_id",
                           JsonTextField(encoded, "repair_plan_id").value_or(""));
    append_session_operand("repair_admission_proven", "true");
    append_session_operand("restricted_or_maintenance_admission", "true");
    append_session_operand("allow_repair", "true");
    append_session_operand("allow_mutation", "true");
    append_session_operand("engine_read_identity_proof", "true");
  }
  if (dispatch_operation_id == "lifecycle.drop_database") {
    append_session_operand("drop_mode",
                           JsonTextField(encoded, "drop_mode").value_or("logical"));
    append_session_operand("drop_safety_preconditions", "true");
    append_session_operand("session_drain_complete", "true");
    append_session_operand("ownership_release_verified", "true");
    append_session_operand("retention_policy_satisfied", "true");
    append_session_operand("backup_coverage_verified", "true");
    append_session_operand("legal_hold_clear", "true");
  }
  if (dispatch_operation_id == "artifact.external_git.export_snapshot" ||
      dispatch_operation_id == "artifact.external_git.diff_snapshot" ||
      dispatch_operation_id == "artifact.external_git.rollback_plan") {
    AppendOperationOperand(&operation_envelope, "external_git_policy", "enabled");
    AppendOperationOperand(&operation_envelope, "git_runtime_authority", "false");
    AppendOperationOperand(&operation_envelope, "external_git_repository_authority", "false");
    if (dispatch_operation_id == "artifact.external_git.diff_snapshot" ||
        dispatch_operation_id == "artifact.external_git.rollback_plan") {
      constexpr std::string_view row_uuid = "external-git-candidate-row-1";
      AppendOperationRowTextField(&operation_envelope, row_uuid, "artifact_format",
                                  "sb.external_git.catalog_snapshot.v1");
      AppendOperationRowTextField(&operation_envelope, row_uuid, "snapshot_entry_kind",
                                  "object");
      AppendOperationRowTextField(&operation_envelope, row_uuid, "object_uuid",
                                  "019f0000-0000-7000-8000-00000000e901");
      AppendOperationRowTextField(&operation_envelope, row_uuid, "object_kind",
                                  "schema");
      AppendOperationRowTextField(&operation_envelope, row_uuid, "default_name",
                                  "external_git_candidate");
      AppendOperationRowTextField(&operation_envelope, row_uuid, "payload",
                                  "localized_name=en,default,external_git_candidate,external_git_candidate,default");
    }
  }
  if (dispatch_operation_id == "security.privilege.grant" ||
      dispatch_operation_id == "security.privilege.revoke") {
    constexpr std::string_view kSecurityFields[] = {
        "target_object_uuid", "target_object_kind", "grantee_uuid",
        "grantee_kind", "privilege", "grant_effect", "grant_uuid"};
    for (const auto field : kSecurityFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "security.membership.grant" ||
      dispatch_operation_id == "security.membership.revoke") {
    constexpr std::string_view kSecurityMembershipFields[] = {
        "membership_uuid", "member_principal_uuid", "container_uuid", "container_kind"};
    for (const auto field : kSecurityMembershipFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "security.session.set_role") {
    constexpr std::string_view kSecurityRoleFields[] = {"role_uuid", "role_mode"};
    for (const auto field : kSecurityRoleFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id.starts_with("filespace.")) {
    constexpr std::string_view kFilespaceFields[] = {
        "target_object_uuid", "target_object_kind", "target_filespace_uuid",
        "requested_pages", "requested_bytes",
        "filespace.page_size_bytes", "filespace.current_pages",
        "filespace.preallocated_pages", "filespace.maximum_pages",
        "filespace.path", "filespace.role", "filespace.physical_id",
        "filespace.total_pages", "filespace.free_pages",
        "filespace.allocation_root_page", "filespace.header_generation",
        "filespace.merge_target_uuid", "filespace.lifecycle.operation",
        "filespace.allow_primary_detach", "filespace.allow_primary_replacement",
        "filespace.allow_promotion",
        "filespace.require_no_active_pins_for_detach",
        "filespace.require_no_active_pins_for_promote",
        "filespace.require_no_active_pins_for_quarantine",
        "filespace.require_no_active_pins_for_move",
        "filespace.require_no_active_pins_for_merge",
        "filespace.require_no_active_pins_for_delete_physical",
        "filespace.require_no_active_pins_for_repair",
        "filespace.require_no_active_pins_for_rebuild",
        "filespace.require_no_active_pins_for_salvage",
        "filespace.allow_filespace_move", "filespace.allow_filespace_merge",
        "filespace.allow_filespace_repair", "filespace.allow_filespace_rebuild",
        "filespace.allow_filespace_salvage",
        "filespace.allow_physical_filespace_delete",
        "filespace.require_physical_header_for_attach",
        "filespace.require_physical_header_for_promote",
        "filespace.allow_archive_owner_assignment"};
    for (const auto field : kFilespaceFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string target_uuid = JsonTextField(encoded, "target_object_uuid").value_or(
        TextLineValue(encoded, "target_object_uuid").value_or(""));
    const std::string filespace_uuid = JsonTextField(encoded, "target_filespace_uuid").value_or(
        TextLineValue(encoded, "target_filespace_uuid").value_or(""));
    if (!target_uuid.empty() && filespace_uuid.empty()) {
      AppendOperationOperand(&operation_envelope, "target_filespace_uuid", target_uuid);
    }
    const std::string target_kind = JsonTextField(encoded, "target_object_kind").value_or(
        TextLineValue(encoded, "target_object_kind").value_or(""));
    if (!target_uuid.empty() && target_kind.empty()) {
      AppendOperationOperand(&operation_envelope, "target_object_kind", "filespace");
    }
  }
  if (dispatch_operation_id == "security.role.create" ||
      dispatch_operation_id == "security.role.drop" ||
      dispatch_operation_id == "security.group.create" ||
      dispatch_operation_id == "security.group.drop" ||
      dispatch_operation_id == "security.principal.create" ||
      dispatch_operation_id == "security.principal.alter") {
    constexpr std::string_view kSecurityPrincipalFields[] = {
        "principal_uuid", "role_uuid", "group_uuid",
        "principal_name", "role_name", "group_name",
        "principal_kind", "lifecycle_state",
        "credential_protected_material_ref", "credential_fingerprint"};
    for (const auto field : kSecurityPrincipalFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "security.policy.attach" ||
      dispatch_operation_id == "security.policy.create" ||
      dispatch_operation_id == "security.policy.alter" ||
      dispatch_operation_id == "security.policy.drop" ||
      dispatch_operation_id == "security.policy.lifecycle_drop" ||
      dispatch_operation_id == "security.mask.drop" ||
      dispatch_operation_id == "security.rls.drop" ||
      dispatch_operation_id == "security.policy.activate" ||
      dispatch_operation_id == "security.policy.deactivate" ||
      dispatch_operation_id == "security.policy.validate" ||
      dispatch_operation_id == "security.policy.show") {
    constexpr std::string_view kSecurityPolicyFields[] = {
        "policy_uuid", "mask_uuid", "rls_uuid",
        "policy_name", "target_object_uuid", "target_object_kind",
        "policy_scope", "policy_effect", "predicate_envelope",
        "definer_principal_uuid", "lifecycle_state", "observed_policy_generation",
        "observed_cache_invalidation_epoch", "include_rows"};
    for (const auto field : kSecurityPolicyFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    if (dispatch_operation_id == "security.policy.create" &&
        !JsonTextField(encoded, "policy_uuid").has_value() &&
        !TextLineValue(encoded, "policy_uuid").has_value()) {
      const std::string policy_seed =
          std::string("security.policy.create:") + std::string(encoded);
      operation_envelope += "operand=text\tpolicy_uuid\t";
      operation_envelope += UuidBytesToText(ServerVirtualSyntheticUuid(policy_seed));
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "op.sbsql.surface_replay") {
    constexpr std::string_view kReplayFields[] = {
        "surface_key", "target_ref", "target_ref_kind", "surface_id"};
    for (const auto field : kReplayFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "op.migration.begin_from_reference" ||
      dispatch_operation_id == "op.migration.alter" ||
      dispatch_operation_id == "op.show.migration") {
    auto migration_field_value = [encoded](std::string_view field) -> std::string {
      auto value = ExistingTextOperandValue(encoded, field).value_or(
          JsonTextField(encoded, field).value_or(TextLineValue(encoded, field).value_or("")));
      if (field == "reference_profile" && (value.empty() || IsPublicRegistryPlaceholder(value))) {
        value = ExistingTextOperandValue(encoded, "target_ref").value_or(
            JsonTextField(encoded, "target_ref").value_or(TextLineValue(encoded, "target_ref").value_or("")));
      } else if (field == "reference_package" &&
                 (value.empty() || IsPublicRegistryPlaceholder(value))) {
        value = ExistingTextOperandValue(encoded, "secondary_ref").value_or(
            JsonTextField(encoded, "secondary_ref").value_or(
                TextLineValue(encoded, "secondary_ref").value_or("")));
      } else if (field == "migration_ref" && (value.empty() || IsPublicRegistryPlaceholder(value))) {
        value = ExistingTextOperandValue(encoded, "target_ref").value_or(
            JsonTextField(encoded, "target_ref").value_or(TextLineValue(encoded, "target_ref").value_or("")));
      } else if (field == "migration_action" && (value.empty() || IsPublicRegistryPlaceholder(value))) {
        value = ExistingTextOperandValue(encoded, "secondary_ref").value_or(
            JsonTextField(encoded, "secondary_ref").value_or(
                TextLineValue(encoded, "secondary_ref").value_or("")));
      }
      return value;
    };
    const std::vector<std::string_view> migration_fields =
        dispatch_operation_id == "op.migration.begin_from_reference"
            ? std::vector<std::string_view>{"reference_profile", "reference_package"}
        : dispatch_operation_id == "op.migration.alter"
            ? std::vector<std::string_view>{"migration_ref", "migration_action"}
            : std::vector<std::string_view>{"migration_ref"};
    for (const auto field : migration_fields) {
      const auto value = migration_field_value(field);
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "catalog.get_descriptor") {
    constexpr std::string_view kCatalogDescriptorFields[] = {
        "target_object_uuid", "target_object_kind", "descriptor_uuid",
        "descriptor_kind", "show_create_target_kind", "descriptor_rendering"};
    for (const auto field : kCatalogDescriptorFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id.starts_with("catalog.mutation.")) {
    constexpr std::string_view kCatalogDescriptorMutationFields[] = {
        "target_object_uuid", "target_object_kind", "catalog_authority",
        "descriptor_ref", "catalog_action", "ddl_operation_id",
        "target_name_parts", "name", "schema_parent_path",
        "name_text_authority",
        "catalog_descriptor_read_only", "mga_catalog_commit_required",
        "parser_executes_sql", "surface_id", "surface_name"};
    for (const auto field : kCatalogDescriptorMutationFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "event.channel.create" ||
      dispatch_operation_id == "event.channel.listen" ||
      dispatch_operation_id == "event.channel.unlisten" ||
      dispatch_operation_id == "event.channel.notify" ||
      dispatch_operation_id == "event.subscription.list" ||
      dispatch_operation_id == "event.delivery.poll" ||
      dispatch_operation_id == "event.delivery.ack" ||
      dispatch_operation_id == "session.notification.unlisten" ||
      dispatch_operation_id == "session.notification.unlisten_all") {
    constexpr std::string_view kEventFields[] = {
        "target_object_uuid", "target_object_kind", "channel_uuid", "channel",
        "payload", "payload_descriptor_uuid", "queue_policy_uuid", "visibility",
        "redaction_policy", "delivery_profile", "subscription_uuid",
        "principal_uuid", "session_uuid", "event_uuid", "source_object_uuid",
        "max_payload_bytes", "admin_scope"};
    for (const auto field : kEventFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string channel_uuid = JsonTextField(encoded, "channel_uuid").value_or(
        TextLineValue(encoded, "channel_uuid").value_or(""));
    if (!channel_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(channel_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\tevent_channel\n";
    }
    const std::string channel_name = JsonTextField(encoded, "channel_name").value_or(
        TextLineValue(encoded, "channel_name").value_or(""));
    if (!channel_name.empty() &&
        JsonTextField(encoded, "channel").value_or(
            TextLineValue(encoded, "channel").value_or("")).empty()) {
      operation_envelope += "operand=text\tchannel\t";
      operation_envelope += EscapeOperationOperandField(channel_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.create_index") {
    constexpr std::string_view kIndexFields[] = {
        "target_object_uuid", "target_object_kind",
        "index_object_uuid", "index_name",
        "index_target_uuid", "index_target_kind",
        "target_table_uuid", "index_profile",
        "index_key_envelope", "index_key_column",
        "index_key_count", "index_unique"};
    for (const auto field : kIndexFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string index_uuid = JsonTextField(encoded, "index_object_uuid").value_or(
        TextLineValue(encoded, "index_object_uuid").value_or(""));
    if (!index_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(index_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\tindex\n";
    }
    const std::string index_name = JsonTextField(encoded, "index_name").value_or(
        TextLineValue(encoded, "index_name").value_or(""));
    if (!index_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(index_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.create_schema") {
    constexpr std::string_view kSchemaFields[] = {
        "target_object_uuid", "target_object_kind",
        "schema_object_uuid", "schema_name",
        "target_schema_uuid", "schema_uuid",
        "schema_parent_uuid", "schema_parent_path",
        "name"};
    for (const auto field : kSchemaFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string schema_uuid = JsonTextField(encoded, "schema_object_uuid").value_or(
        TextLineValue(encoded, "schema_object_uuid").value_or(""));
    const std::string target_uuid = JsonTextField(encoded, "target_object_uuid").value_or(
        TextLineValue(encoded, "target_object_uuid").value_or(""));
    if (!schema_uuid.empty() &&
        target_uuid.empty()) {
      AppendOperationOperand(&operation_envelope, "target_object_uuid", schema_uuid);
      AppendOperationOperand(&operation_envelope, "target_object_kind", "schema");
    } else if (schema_uuid.empty() && target_uuid.empty()) {
      const std::string generated_schema_uuid = engine_api::GenerateCrudEngineUuid("schema");
      if (!generated_schema_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "target_object_uuid", generated_schema_uuid);
        AppendOperationOperand(&operation_envelope, "schema_object_uuid", generated_schema_uuid);
        AppendOperationOperand(&operation_envelope, "target_object_kind", "schema");
      }
    }
    const std::string schema_name = JsonTextField(encoded, "schema_name").value_or(
        TextLineValue(encoded, "schema_name").value_or(""));
    if (!schema_name.empty() &&
        JsonTextField(encoded, "name").value_or(
            TextLineValue(encoded, "name").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "name", schema_name);
    }
    const std::string parent_uuid = JsonTextField(encoded, "schema_parent_uuid").value_or(
        TextLineValue(encoded, "schema_parent_uuid").value_or(""));
    if (!parent_uuid.empty() &&
        JsonTextField(encoded, "target_schema_uuid").value_or(
            TextLineValue(encoded, "target_schema_uuid").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "target_schema_uuid", parent_uuid);
    } else if (parent_uuid.empty() &&
               JsonTextField(encoded, "target_schema_uuid").value_or(
                   TextLineValue(encoded, "target_schema_uuid").value_or("")).empty()) {
      const std::string schema_parent_path = JsonTextField(encoded, "schema_parent_path").value_or(
          TextLineValue(encoded, "schema_parent_path").value_or(""));
      const std::string resolved_parent_uuid =
          ResolveSchemaParentPathForDispatch(session, schema_parent_path);
      if (!resolved_parent_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "schema_parent_uuid", resolved_parent_uuid);
        AppendOperationOperand(&operation_envelope, "target_schema_uuid", resolved_parent_uuid);
      }
    }
  }
  if (dispatch_operation_id == "ddl.create_table") {
    std::unordered_map<std::string, std::string> table_field_cache;
    auto table_field_value = [&](std::string_view field) -> std::string {
      const std::string cache_key(field);
      const auto cached = table_field_cache.find(cache_key);
      if (cached != table_field_cache.end()) {
        return cached->second;
      }
      std::string value = ExistingTextOperandValue(encoded, field).value_or(
          JsonTextField(encoded, field).value_or(
              JsonPrimitiveField(encoded, field).value_or(
                  TextLineValue(encoded, field).value_or(""))));
      table_field_cache.emplace(cache_key, value);
      return value;
    };
    constexpr std::string_view kTableFields[] = {
        "target_object_uuid", "target_object_kind",
        "table_object_uuid", "table_name",
        "target_schema_uuid", "schema_uuid",
        "schema_parent_uuid", "schema_parent_path",
        "column_count", "column_definition_count",
        "physical_profile",
        "canonical_type_name"};
    for (const auto field : kTableFields) {
      const auto value = table_field_value(field);
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string column_count_value = JsonPrimitiveField(encoded, "column_count").value_or(
        ExistingTextOperandValue(encoded, "column_count").value_or(
            TextLineValue(encoded, "column_count").value_or("")));
    const std::uint64_t column_count = ParseU64Text(column_count_value);
    for (std::uint64_t ordinal = 0; ordinal < column_count; ++ordinal) {
      const std::string prefix = "column_" + std::to_string(ordinal) + "_";
      const std::string column_type = table_field_value(prefix + "type");
      std::string column_descriptor = table_field_value(prefix + "descriptor");
      std::string raw_column_default = table_field_value(prefix + "default");
      if (raw_column_default.empty()) {
        raw_column_default = DescriptorFieldValue(column_descriptor, "default");
      }
      const std::string column_default =
          ResolveColumnDefaultForDispatch(session, raw_column_default);
      const std::string domain_uuid =
          ResolveColumnDomainUuidForDispatch(session, column_type, column_descriptor);
      if (!domain_uuid.empty()) {
        if (column_descriptor.empty()) {
          column_descriptor = column_type.empty() ? "type=domain" : "type=" + column_type;
        }
        AppendDescriptorField(&column_descriptor, "domain_uuid", domain_uuid);
      }
      if (!column_default.empty()) {
        if (column_descriptor.empty()) {
          column_descriptor = column_type.empty() ? "type=scalar" : "type=" + column_type;
        }
        SetDescriptorField(&column_descriptor, "default", column_default);
      }
      for (const auto suffix : {"name", "type", "nullable", "default"}) {
        const std::string field = prefix + suffix;
        const std::string value =
            std::string_view(suffix) == "default" ? column_default : table_field_value(field);
        if (value.empty()) continue;
        AppendOperationOperand(&operation_envelope, field, value);
      }
      if (!column_descriptor.empty()) {
        AppendOperationOperand(&operation_envelope, prefix + "descriptor", column_descriptor);
      }
    }
    const std::string explicit_target_schema_uuid = table_field_value("target_schema_uuid");
    const std::string explicit_schema_uuid = table_field_value("schema_uuid");
    const std::string explicit_parent_uuid = table_field_value("schema_parent_uuid");
    const std::string schema_parent_path = table_field_value("schema_parent_path");
    if (explicit_target_schema_uuid.empty() &&
        explicit_schema_uuid.empty() &&
        explicit_parent_uuid.empty() &&
        !schema_parent_path.empty()) {
      const std::string resolved_parent_uuid =
          ResolveSchemaParentPathForDispatch(session, schema_parent_path);
      if (!resolved_parent_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "schema_parent_uuid", resolved_parent_uuid);
        AppendOperationOperand(&operation_envelope, "target_schema_uuid", resolved_parent_uuid);
        AppendOperationOperand(&operation_envelope, "schema_uuid", resolved_parent_uuid);
      } else {
        AppendOperationOperand(&operation_envelope,
                               "unresolved_schema_parent_path",
                               schema_parent_path);
      }
    }
    const bool has_explicit_schema_target =
        !explicit_target_schema_uuid.empty() ||
        !explicit_schema_uuid.empty() ||
        !explicit_parent_uuid.empty() ||
        !schema_parent_path.empty();
    if (!has_explicit_schema_target) {
      const std::string default_schema_uuid = ResolveDefaultSchemaForDispatch(session);
      if (!default_schema_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "target_schema_uuid", default_schema_uuid);
        AppendOperationOperand(&operation_envelope, "schema_uuid", default_schema_uuid);
      }
    }
    const std::string table_uuid = table_field_value("table_object_uuid");
    if (!table_uuid.empty() &&
        table_field_value("target_object_uuid").empty()) {
      AppendOperationOperand(&operation_envelope, "target_object_uuid", table_uuid);
      AppendOperationOperand(&operation_envelope, "target_object_kind", "table");
    }
    const std::string table_name = table_field_value("table_name");
    if (!table_name.empty()) {
      AppendOperationOperand(&operation_envelope, "name", table_name);
    }
    const bool temporary =
        JsonBoolField(encoded, "temporary_table", false) ||
        JsonBoolField(encoded, "temporary", false) ||
        TextBoolField(encoded, "temporary_table", false) ||
        TextBoolField(encoded, "temporary", false);
    if (temporary) {
      AppendOperationOperand(&operation_envelope, "temporary", "true");
      const std::string scope = JsonTextField(encoded, "temporary_scope").value_or(
          TextLineValue(encoded, "temporary_scope").value_or("private"));
      AppendOperationOperand(&operation_envelope, "temporary_scope",
                             scope.empty() ? "session" : scope);
      const std::string on_commit = JsonTextField(encoded, "on_commit_action").value_or(
          JsonTextField(encoded, "on_commit").value_or(
              TextLineValue(encoded, "on_commit_action").value_or(
                  TextLineValue(encoded, "on_commit").value_or("delete_rows"))));
      AppendOperationOperand(&operation_envelope, "on_commit",
                             on_commit.empty() ? "delete_rows" : on_commit);
    }
  }
  if (dispatch_operation_id == "ddl.create_index_template") {
    constexpr std::string_view kIndexTemplateFields[] = {
        "target_object_uuid", "target_object_kind",
        "index_template_object_uuid", "index_template_name",
        "index_template_kind", "index_template_pattern_count",
        "index_template_composed_of_count", "template_document_present",
        "meta_document_present", "priority_present", "version_present"};
    for (const auto field : kIndexTemplateFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string template_uuid = JsonTextField(encoded, "index_template_object_uuid").value_or(
        TextLineValue(encoded, "index_template_object_uuid").value_or(""));
    if (!template_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(template_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\t";
      operation_envelope += EscapeOperationOperandField(
          JsonTextField(encoded, "index_template_kind").value_or("index_template"));
      operation_envelope += "\n";
    }
    const std::string template_name = JsonTextField(encoded, "index_template_name").value_or(
        TextLineValue(encoded, "index_template_name").value_or(""));
    if (!template_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(template_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.comment_on_object") {
    constexpr std::string_view kCommentFields[] = {
        "target_object_uuid", "target_object_kind", "comment_target_uuid",
        "comment_target_kind", "comment_is_null", "comment_text",
        "comment_language"};
    for (const auto field : kCommentFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string comment_target_uuid = JsonTextField(encoded, "comment_target_uuid").value_or(
        TextLineValue(encoded, "comment_target_uuid").value_or(""));
    if (!comment_target_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(comment_target_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\t";
      operation_envelope += EscapeOperationOperandField(
          JsonTextField(encoded, "comment_target_kind").value_or("object"));
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.alter_object") {
	    constexpr std::string_view kAlterObjectFields[] = {
	        "target_object_uuid", "target_object_kind", "rename_target_uuid",
	        "rename_target_kind", "new_name", "rename_new_name",
	        "target_schema_uuid", "schema_uuid",
	        "domain_target_uuid", "default_expression", "check_constraint",
	        "check_constraint_append", "nullable",
	        "table_alter_action", "column_name", "new_column_name", "column_descriptor",
	        "sequence_target_uuid", "sequence_lookup_key",
	        "sequence_cache", "sequence_max_value", "sequence_restart_value"};
    for (const auto field : kAlterObjectFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string rename_target_uuid = JsonTextField(encoded, "rename_target_uuid").value_or(
        TextLineValue(encoded, "rename_target_uuid").value_or(""));
    if (!rename_target_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(rename_target_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\t";
      operation_envelope += EscapeOperationOperandField(
          JsonTextField(encoded, "rename_target_kind").value_or("object"));
      operation_envelope += "\n";
    }
    const std::string rename_new_name = JsonTextField(encoded, "rename_new_name").value_or(
        TextLineValue(encoded, "rename_new_name").value_or(""));
    if (!rename_new_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(rename_new_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id.starts_with("ddl.constraint.")) {
    constexpr std::string_view kConstraintFields[] = {
        "target_object_uuid", "target_object_kind", "owner_object_uuid",
        "constraint_name", "constraint_kind",
        "canonical_constraint_envelope", "enforcement_timing"};
    for (const auto field : kConstraintFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string owner_uuid = JsonTextField(encoded, "owner_object_uuid").value_or(
        TextLineValue(encoded, "owner_object_uuid").value_or(""));
    if (!owner_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "target_object_uuid", owner_uuid);
      AppendOperationOperand(&operation_envelope, "target_object_kind", "table");
    }
  }
  if (dispatch_operation_id == "ddl.drop_object") {
    constexpr std::string_view kDropObjectFields[] = {
        "target_object_uuid", "target_object_kind", "drop_target_uuid",
        "drop_target_kind"};
    for (const auto field : kDropObjectFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.create_statistics") {
    constexpr std::string_view kStatisticsFields[] = {
        "target_object_uuid", "target_object_kind",
        "statistics_object_uuid", "statistics_name",
        "statistics_target_uuid", "statistics_target_kind",
        "target_table_uuid", "statistics_kind",
        "statistics_expression_count", "statistics_expression"};
    for (const auto field : kStatisticsFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string statistics_uuid = JsonTextField(encoded, "statistics_object_uuid").value_or(
        TextLineValue(encoded, "statistics_object_uuid").value_or(""));
    if (!statistics_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(statistics_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\tstatistics\n";
    }
    const std::string statistics_name = JsonTextField(encoded, "statistics_name").value_or(
        TextLineValue(encoded, "statistics_name").value_or(""));
    if (!statistics_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(statistics_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.create_sequence") {
    constexpr std::string_view kSequenceFields[] = {
        "target_object_uuid", "target_object_kind",
        "sequence_object_uuid", "sequence_name", "sequence_lookup_key",
        "target_schema_uuid", "schema_uuid", "schema_parent_path",
        "sequence_type",
        "sequence_start_value", "sequence_increment",
        "sequence_min_value", "sequence_max_value",
        "sequence_cache", "sequence_no_cache",
        "sequence_cycle", "sequence_descriptor"};
    for (const auto field : kSequenceFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string sequence_uuid = JsonTextField(encoded, "sequence_object_uuid").value_or(
        TextLineValue(encoded, "sequence_object_uuid").value_or(""));
    if (!sequence_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(sequence_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\tsequence\n";
    }
    const std::string sequence_name = JsonTextField(encoded, "sequence_name").value_or(
        TextLineValue(encoded, "sequence_name").value_or(""));
    if (!sequence_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(sequence_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.create_domain") {
    constexpr std::string_view kDomainFields[] = {
        "target_object_uuid", "target_object_kind",
        "domain_object_uuid", "domain_name",
        "target_schema_uuid", "schema_uuid",
        "schema_parent_uuid", "schema_parent_path",
        "base_descriptor_uuid", "base_descriptor_kind",
        "base_canonical_type_name", "base_encoded_descriptor",
        "default_expression", "check_constraint", "nullable"};
    for (const auto field : kDomainFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string domain_uuid = JsonTextField(encoded, "domain_object_uuid").value_or(
        TextLineValue(encoded, "domain_object_uuid").value_or(""));
    if (!domain_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(domain_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\tdomain\n";
    }
    const std::string domain_name = JsonTextField(encoded, "domain_name").value_or(
        TextLineValue(encoded, "domain_name").value_or(""));
    if (!domain_name.empty() &&
        JsonTextField(encoded, "name").value_or(
            TextLineValue(encoded, "name").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "name", domain_name);
    }
    const std::string parent_uuid = JsonTextField(encoded, "schema_parent_uuid").value_or(
        TextLineValue(encoded, "schema_parent_uuid").value_or(""));
    if (!parent_uuid.empty() &&
        JsonTextField(encoded, "target_schema_uuid").value_or(
            TextLineValue(encoded, "target_schema_uuid").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "target_schema_uuid", parent_uuid);
    }
  }
  if (dispatch_operation_id == "ddl.create_view") {
    constexpr std::string_view kViewFields[] = {
        "target_object_uuid", "target_object_kind",
        "view_object_uuid", "view_name",
        "target_schema_uuid", "schema_uuid", "schema_parent_path",
        "view_projection_count", "view_query_shape", "view_materialized",
        "view_source_path", "view_source_uuid", "view_source_name",
        "view_predicate_kind", "view_predicate_column",
        "view_predicate_value", "view_predicate_value_type",
        "view_group_key_field", "view_aggregate_function",
        "view_aggregate_value_field"};
    for (const auto field : kViewFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string view_uuid = JsonTextField(encoded, "view_object_uuid").value_or(
        TextLineValue(encoded, "view_object_uuid").value_or(""));
    if (!view_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "target_object_uuid", view_uuid);
    }
    if (JsonTextField(encoded, "target_object_kind").value_or(
            TextLineValue(encoded, "target_object_kind").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope,
                             "target_object_kind",
                             JsonBoolField(encoded, "view_materialized", false) ||
                                     TextBoolField(encoded, "view_materialized", false)
                                 ? "materialized_view"
                                 : "view");
    }
    const std::string view_name = JsonTextField(encoded, "view_name").value_or(
        TextLineValue(encoded, "view_name").value_or(""));
    if (!view_name.empty() &&
        JsonTextField(encoded, "name").value_or(
            TextLineValue(encoded, "name").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "name", view_name);
    }
    const std::string schema_parent_path = JsonTextField(encoded, "schema_parent_path").value_or(
        TextLineValue(encoded, "schema_parent_path").value_or(""));
    if (!schema_parent_path.empty() &&
        JsonTextField(encoded, "target_schema_uuid").value_or(
            TextLineValue(encoded, "target_schema_uuid").value_or("")).empty() &&
        JsonTextField(encoded, "schema_uuid").value_or(
            TextLineValue(encoded, "schema_uuid").value_or("")).empty()) {
      const std::string resolved_parent_uuid =
          ResolveSchemaParentPathForDispatch(session, schema_parent_path);
      if (!resolved_parent_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "target_schema_uuid", resolved_parent_uuid);
        AppendOperationOperand(&operation_envelope, "schema_uuid", resolved_parent_uuid);
      }
    }
    const std::string source_path = JsonTextField(encoded, "view_source_path").value_or(
        TextLineValue(encoded, "view_source_path").value_or(""));
    if (!source_path.empty() &&
        JsonTextField(encoded, "view_source_uuid").value_or(
            TextLineValue(encoded, "view_source_uuid").value_or("")).empty()) {
      const std::string source_uuid = ResolveRelationPathForDispatch(session, source_path);
      if (!source_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "view_source_uuid", source_uuid);
      }
    }
    const std::string projection_count_text = JsonPrimitiveField(encoded, "view_projection_count").value_or(
        JsonTextField(encoded, "view_projection_count").value_or(
            TextLineValue(encoded, "view_projection_count").value_or("0")));
    const std::uint64_t projection_count = ParseU64Text(projection_count_text);
    for (std::uint64_t projection = 0; projection < projection_count; ++projection) {
      const std::string field = "view_projection_" + std::to_string(projection);
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
  }
  if (dispatch_operation_id == "ddl.create_function" ||
      dispatch_operation_id == "ddl.create_procedure" ||
      dispatch_operation_id == "ddl.create_trigger") {
    const std::string object_kind =
        dispatch_operation_id == "ddl.create_function" ? "function" :
        (dispatch_operation_id == "ddl.create_procedure" ? "procedure" : "trigger");
    const std::string object_uuid_field = object_kind + "_object_uuid";
    const std::string object_name_field = object_kind + "_name";
    constexpr std::string_view kExecutableFields[] = {
        "target_object_uuid", "target_object_kind",
        "target_schema_uuid", "schema_uuid",
        "schema_parent_path",
        "executable_object_kind", "descriptor_kind",
        "signature_descriptor_kind", "signature_descriptor_uuid",
        "signature_descriptor", "routine_language",
        "routine_parameter_descriptor_present", "routine_parameter_count",
        "routine_parameter_0_name_descriptor", "routine_parameter_0_type",
        "routine_parameter_0_mode", "routine_parameter_0_descriptor_kind",
        "routine_return_descriptor_present", "routine_return_count",
        "body_compilation_included", "executable_descriptor_kind",
        "executor", "internal_procedure_id", "side_effect_class",
        "compiled_body_provenance", "compiled_body_descriptor",
        "trigger_timing", "trigger_event", "trigger_scope",
        "trigger_target_table_uuid", "trigger_target_table_name",
        "routine_cursor_argument", "routine_cursor_argument_binding",
        "routine_cursor_argument_parser_executes_cursor"};
    for (const auto field : kExecutableFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    for (std::size_t related = 0; related < 64; ++related) {
      const std::string prefix = "related_object_" + std::to_string(related);
      const auto uuid = JsonTextField(encoded, prefix + "_uuid").value_or("");
      if (uuid.empty() && !JsonTextField(encoded, prefix + "_kind").has_value()) break;
      if (!uuid.empty()) AppendOperationOperand(&operation_envelope, prefix + "_uuid", uuid);
      if (const auto kind = JsonTextField(encoded, prefix + "_kind")) {
        AppendOperationOperand(&operation_envelope, prefix + "_kind", *kind);
      }
    }
    for (std::size_t index = 0; index < 64; ++index) {
      const std::string prefix = "routine_parameter_" + std::to_string(index);
      const auto name = JsonTextField(encoded, prefix + "_name").value_or("");
      const auto type = JsonTextField(encoded, prefix + "_type").value_or("");
      const auto mode = JsonTextField(encoded, prefix + "_mode").value_or("");
      const auto descriptor_kind = JsonTextField(encoded, prefix + "_descriptor_kind").value_or("");
      if (name.empty() && type.empty() && mode.empty() && descriptor_kind.empty()) break;
      if (!name.empty()) AppendOperationOperand(&operation_envelope, prefix + "_name", name);
      if (!type.empty()) AppendOperationOperand(&operation_envelope, prefix + "_type", type);
      if (!mode.empty()) AppendOperationOperand(&operation_envelope, prefix + "_mode", mode);
      if (!descriptor_kind.empty()) {
        AppendOperationOperand(&operation_envelope, prefix + "_descriptor_kind", descriptor_kind);
      }
    }
    for (std::size_t index = 0; index < 64; ++index) {
      const std::string prefix = "routine_return_" + std::to_string(index);
      const auto name = JsonTextField(encoded, prefix + "_name").value_or("");
      const auto type = JsonTextField(encoded, prefix + "_type").value_or("");
      if (name.empty() && type.empty()) break;
      if (!name.empty()) AppendOperationOperand(&operation_envelope, prefix + "_name", name);
      if (!type.empty()) AppendOperationOperand(&operation_envelope, prefix + "_type", type);
    }
    std::string object_uuid = JsonTextField(encoded, object_uuid_field).value_or(
        TextLineValue(encoded, object_uuid_field).value_or(""));
    if (object_uuid.empty()) {
      object_uuid = engine_api::GenerateCrudEngineUuid("object");
    }
    if (!object_uuid.empty()) {
      operation_envelope += "operand=text\t";
      operation_envelope += object_uuid_field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(object_uuid);
      operation_envelope += "\n";
    }
    if (!object_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(object_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\t";
      operation_envelope += object_kind;
      operation_envelope += "\n";
    }
    const std::string explicit_target_schema_uuid = JsonTextField(encoded, "target_schema_uuid").value_or(
        TextLineValue(encoded, "target_schema_uuid").value_or(""));
    const std::string explicit_schema_uuid = JsonTextField(encoded, "schema_uuid").value_or(
        TextLineValue(encoded, "schema_uuid").value_or(""));
    const std::string schema_parent_path = JsonTextField(encoded, "schema_parent_path").value_or(
        TextLineValue(encoded, "schema_parent_path").value_or(""));
    if (explicit_target_schema_uuid.empty() && explicit_schema_uuid.empty()) {
      std::string resolved_schema_uuid;
      if (!schema_parent_path.empty()) {
        resolved_schema_uuid = ResolveSchemaParentPathForDispatch(session, schema_parent_path);
      }
      if (resolved_schema_uuid.empty()) {
        resolved_schema_uuid = ResolveDefaultSchemaForDispatch(session);
      }
      if (!resolved_schema_uuid.empty()) {
        AppendOperationOperand(&operation_envelope, "target_schema_uuid", resolved_schema_uuid);
        AppendOperationOperand(&operation_envelope, "schema_uuid", resolved_schema_uuid);
      }
    }
    const std::string object_name = JsonTextField(encoded, object_name_field).value_or(
        TextLineValue(encoded, object_name_field).value_or(""));
    if (!object_name.empty()) {
      operation_envelope += "operand=text\t";
      operation_envelope += object_name_field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(object_name);
      operation_envelope += "\n";
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(object_name);
      operation_envelope += "\n";
    }
    AppendOperationOperand(&operation_envelope, "permission", "manage_executable");
  }
  if (dispatch_operation_id == "routine.procedure_invoke" ||
      dispatch_operation_id == "routine.function_invoke") {
    constexpr std::string_view kRoutineInvokeFields[] = {
        "target_object_uuid", "object_uuid", "routine_object_uuid",
        "target_object_kind", "routine_invocation_kind", "routine_argument_count",
        "routine_name", "target_schema_uuid"};
    for (const auto field : kRoutineInvokeFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, std::string(field), value);
    }
    for (std::size_t argument = 0; argument < 64; ++argument) {
      const std::string prefix = "routine_argument_" + std::to_string(argument);
      const auto value = JsonTextField(encoded, prefix + "_value").value_or("");
      if (value.empty() && !JsonTextField(encoded, prefix + "_binding").has_value()) break;
      AppendOperationOperand(&operation_envelope, prefix + "_value", value);
      if (const auto type = JsonTextField(encoded, prefix + "_type")) {
        AppendOperationOperand(&operation_envelope, prefix + "_type", *type);
      }
      if (const auto binding = JsonTextField(encoded, prefix + "_binding")) {
        AppendOperationOperand(&operation_envelope, prefix + "_binding", *binding);
      }
      if (const auto descriptor_kind = JsonTextField(encoded, prefix + "_descriptor_kind")) {
        AppendOperationOperand(&operation_envelope,
                               prefix + "_descriptor_kind",
                               *descriptor_kind);
      }
    }
    const std::string target_uuid = JsonTextField(encoded, "target_object_uuid").value_or(
        TextLineValue(encoded, "target_object_uuid").value_or(""));
    const std::string object_uuid = JsonTextField(encoded, "object_uuid").value_or(
        TextLineValue(encoded, "object_uuid").value_or(""));
    if (!target_uuid.empty() && object_uuid.empty()) {
      AppendOperationOperand(&operation_envelope, "object_uuid", target_uuid);
    }
    AppendOperationOperand(&operation_envelope, "permission", "invoke_executable");
  }
  if (dispatch_operation_id.starts_with("agents.")) {
    const std::string agent_type = JsonTextField(encoded, "agent_type").value_or(
        TextLineValue(encoded, "agent_type").value_or(""));
    if (!agent_type.empty()) {
      operation_envelope += "operand=text\tagent_type\t";
      operation_envelope += EscapeOperationOperandField(agent_type);
      operation_envelope += "\n";
    }
    operation_envelope += "operand=text\twall_now_us\t1\n";
    operation_envelope += "operand=text\tmonotonic_now_us\t1\n";
    operation_envelope += "operand=text\tprivate_features\ttrue\n";
    operation_envelope += "operand=text\tstandalone_edition\ttrue\n";
  }
  if (dispatch_operation_id == "management.inspect_runtime" ||
      dispatch_operation_id == "management.control_runtime") {
    const std::string runtime_component = JsonTextField(encoded, "runtime_component").value_or(
        TextLineValue(encoded, "runtime_component").value_or(""));
    const std::string runtime_target_name = JsonTextField(encoded, "runtime_target_name").value_or(
        TextLineValue(encoded, "runtime_target_name").value_or(""));
    if (!runtime_component.empty()) {
      operation_envelope += "operand=text\truntime_component\t";
      operation_envelope += EscapeOperationOperandField(runtime_component);
      operation_envelope += "\n";
    }
    if (!runtime_target_name.empty()) {
      operation_envelope += "operand=text\truntime_target_name\t";
      operation_envelope += EscapeOperationOperandField(runtime_target_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id.starts_with("extensibility.register_udr_package") ||
      dispatch_operation_id.starts_with("extensibility.load_udr_package") ||
      dispatch_operation_id.starts_with("extensibility.unload_udr_package") ||
      dispatch_operation_id.starts_with("extensibility.inspect_udr_packages") ||
      dispatch_operation_id.starts_with("extensibility.invoke_udr_package")) {
    constexpr std::string_view kUdrFields[] = {
        "target_object_uuid", "target_object_kind", "udr_package_name",
        "runtime_component", "permission", "right", "trust", "abi",
        "linked_udr_package", "source_revision", "binary_hash",
        "signature_policy", "capability_role", "entrypoint", "payload",
        "context_packet", "memory_budget_bytes", "cpu_budget_microseconds",
        "operation_family"};
    for (const auto field : kUdrFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    if (dispatch_operation_id == "extensibility.inspect_udr_packages") {
      operation_envelope += "operand=text\tpermission\tinspect_udr\n";
    }
  }
  if (dispatch_operation_id == "query.evaluate_projection") {
    const auto projection_count_value = JsonTextField(encoded, "projection_count").value_or(
        TextLineValue(encoded, "projection_count").value_or(""));
    std::uint64_t projection_count = 0;
    bool projection_count_valid = !projection_count_value.empty();
    for (const unsigned char ch : projection_count_value) {
      if (!std::isdigit(ch)) {
        projection_count_valid = false;
        break;
      }
      projection_count = projection_count * 10u + static_cast<std::uint64_t>(ch - '0');
    }
    if (projection_count_valid) {
      operation_envelope += "operand=text\tprojection_count\t";
      operation_envelope += EscapeOperationOperandField(projection_count_value);
      operation_envelope += "\n";
      for (std::uint64_t index = 0; index < projection_count; ++index) {
        const std::string prefix = "projection_" + std::to_string(index) + "_";
        AppendOperationOperand(&operation_envelope, prefix + "name", EncodedTextField(encoded, prefix + "name"));
        AppendProjectionExpressionOperands(encoded, prefix, &operation_envelope);
      }
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"values_rowset\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=values_rowset") != std::string_view::npos)) {
    auto parse_u64 = [](const std::string& value) -> std::optional<std::uint64_t> {
      if (value.empty()) return std::nullopt;
      std::uint64_t parsed = 0;
      for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) return std::nullopt;
        parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
      }
      return parsed;
    };
    const auto row_count = parse_u64(JsonTextField(encoded, "values_row_count").value_or(
        TextLineValue(encoded, "values_row_count").value_or("")));
    const auto column_count = parse_u64(JsonTextField(encoded, "values_column_count").value_or(
        TextLineValue(encoded, "values_column_count").value_or("")));
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tvalues\n";
    if (row_count && column_count) {
      for (std::uint64_t row = 0; row < *row_count; ++row) {
        for (std::uint64_t column = 0; column < *column_count; ++column) {
          const std::string prefix =
              "values_" + std::to_string(row) + "_" + std::to_string(column) + "_";
          const std::string name = JsonTextField(encoded, prefix + "name").value_or(
              TextLineValue(encoded, prefix + "name").value_or("c" + std::to_string(column)));
          const std::string type = JsonTextField(encoded, prefix + "type").value_or(
              TextLineValue(encoded, prefix + "type").value_or("text"));
          const std::string value = JsonTextField(encoded, prefix + "value").value_or(
              TextLineValue(encoded, prefix + "value").value_or(""));
          const std::string is_null = JsonTextField(encoded, prefix + "is_null").value_or(
              TextLineValue(encoded, prefix + "is_null").value_or("false"));
          operation_envelope += "operand=";
          operation_envelope += (is_null == "true" || is_null == "1") ? "row_null_field:" : "row_field:";
          operation_envelope += EscapeOperationOperandField(type.empty() ? "text" : type);
          operation_envelope += "\tvalues-row-";
          operation_envelope += std::to_string(row);
          operation_envelope += "|";
          operation_envelope += EscapeOperationOperandField(name.empty() ? "c" + std::to_string(column) : name);
          operation_envelope += "\t";
          operation_envelope += EscapeOperationOperandField(value);
          operation_envelope += "\n";
        }
      }
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"values_set_operation\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=values_set_operation") != std::string_view::npos)) {
    auto parse_u64 = [](const std::string& value) -> std::optional<std::uint64_t> {
      if (value.empty()) return std::nullopt;
      std::uint64_t parsed = 0;
      for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) return std::nullopt;
        parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
      }
      return parsed;
    };
    const auto column_count = parse_u64(JsonTextField(encoded, "values_column_count").value_or(
        TextLineValue(encoded, "values_column_count").value_or("")));
    const std::string set_operation = JsonTextField(encoded, "set_operation").value_or(
        TextLineValue(encoded, "set_operation").value_or("union_distinct"));
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\t";
    operation_envelope += EscapeOperationOperandField(set_operation.empty() ? "union_distinct" : set_operation);
    operation_envelope += "\n";
    operation_envelope += "operand=text\tset_operation\t";
    operation_envelope += EscapeOperationOperandField(set_operation.empty() ? "union_distinct" : set_operation);
    operation_envelope += "\n";
    if (column_count) {
      for (std::uint64_t relation = 0; relation < 2; ++relation) {
        const auto row_count = parse_u64(JsonTextField(encoded, "relation_" + std::to_string(relation) + "_row_count").value_or(
            TextLineValue(encoded, "relation_" + std::to_string(relation) + "_row_count").value_or("")));
        if (!row_count) continue;
        for (std::uint64_t row = 0; row < *row_count; ++row) {
          for (std::uint64_t column = 0; column < *column_count; ++column) {
            const std::string prefix = "relation_" + std::to_string(relation) + "_" +
                                       std::to_string(row) + "_" + std::to_string(column) + "_";
            const std::string name = JsonTextField(encoded, prefix + "name").value_or(
                TextLineValue(encoded, prefix + "name").value_or("c" + std::to_string(column)));
            const std::string type = JsonTextField(encoded, prefix + "type").value_or(
                TextLineValue(encoded, prefix + "type").value_or("bigint"));
            const std::string value = JsonTextField(encoded, prefix + "value").value_or(
                TextLineValue(encoded, prefix + "value").value_or(""));
            const std::string is_null = JsonTextField(encoded, prefix + "is_null").value_or(
                TextLineValue(encoded, prefix + "is_null").value_or("false"));
            operation_envelope += "operand=";
            operation_envelope += (is_null == "true" || is_null == "1") ? "row_null_field:" : "row_field:";
            operation_envelope += EscapeOperationOperandField(type.empty() ? "bigint" : type);
            operation_envelope += "\trelation-";
            operation_envelope += std::to_string(relation);
            operation_envelope += "-row-";
            operation_envelope += std::to_string(row);
            operation_envelope += "|";
            operation_envelope += EscapeOperationOperandField(name.empty() ? "c" + std::to_string(column) : name);
            operation_envelope += "\t";
            operation_envelope += EscapeOperationOperandField(value);
            operation_envelope += "\n";
          }
        }
      }
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"values_materialized_cte\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=values_materialized_cte") != std::string_view::npos)) {
    auto parse_u64 = [](const std::string& value) -> std::optional<std::uint64_t> {
      if (value.empty()) return std::nullopt;
      std::uint64_t parsed = 0;
      for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) return std::nullopt;
        parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
      }
      return parsed;
    };
    const auto column_count = parse_u64(JsonTextField(encoded, "values_column_count").value_or(
        TextLineValue(encoded, "values_column_count").value_or("")));
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tmaterialized_cte\n";
    constexpr std::string_view kMaterializedCteFields[] = {
        "result_projection",
        "aggregate_function",
        "aggregate_value_field",
        "aggregate_pair_value_field",
        "order_by",
        "aggregate_fraction",
        "aggregate_limit",
        "listagg_separator",
        "listagg_overflow_mode",
        "listagg_max_output_bytes",
        "listagg_truncation_indicator",
        "listagg_with_count",
        "hypothetical_value",
        "hypothetical_value_type",
        "window_function",
        "window_value_field",
        "window_n",
        "window_offset",
        "window_default_value",
        "window_default_type",
        "window_default_is_null",
        "window_lookup_field",
        "window_lookup_value",
        "window_limit_first",
        "window_filter_present",
        "window_filter_field",
        "window_filter_min",
        "window_filter_max",
        "assertion_id",
        "actual_column_name",
        "expected_column_name",
        "expected_value",
        "expected_value_is_null"};
    for (const auto field : kMaterializedCteFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    if (column_count) {
      const auto row_count = parse_u64(JsonTextField(encoded, "relation_0_row_count").value_or(
          TextLineValue(encoded, "relation_0_row_count").value_or("")));
      if (row_count) {
        for (std::uint64_t row = 0; row < *row_count; ++row) {
          for (std::uint64_t column = 0; column < *column_count; ++column) {
            const std::string prefix = "relation_0_" +
                                       std::to_string(row) + "_" +
                                       std::to_string(column) + "_";
            const std::string name = JsonTextField(encoded, prefix + "name").value_or(
                TextLineValue(encoded, prefix + "name").value_or("c" + std::to_string(column)));
            const std::string type = JsonTextField(encoded, prefix + "type").value_or(
                TextLineValue(encoded, prefix + "type").value_or("bigint"));
            const std::string value = JsonTextField(encoded, prefix + "value").value_or(
                TextLineValue(encoded, prefix + "value").value_or(""));
            const std::string is_null = JsonTextField(encoded, prefix + "is_null").value_or(
                TextLineValue(encoded, prefix + "is_null").value_or("false"));
            operation_envelope += "operand=";
            operation_envelope += (is_null == "true" || is_null == "1") ? "row_null_field:" : "row_field:";
            operation_envelope += EscapeOperationOperandField(type.empty() ? "bigint" : type);
            operation_envelope += "\trelation-0-row-";
            operation_envelope += std::to_string(row);
            operation_envelope += "|";
            operation_envelope += EscapeOperationOperandField(name.empty() ? "c" + std::to_string(column) : name);
            operation_envelope += "\t";
            operation_envelope += EscapeOperationOperandField(value);
            operation_envelope += "\n";
          }
        }
      }
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"values_recursive_cte\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=values_recursive_cte") != std::string_view::npos)) {
    auto parse_u64 = [](const std::string& value) -> std::optional<std::uint64_t> {
      if (value.empty()) return std::nullopt;
      std::uint64_t parsed = 0;
      for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) return std::nullopt;
        parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
      }
      return parsed;
    };
    const auto column_count = parse_u64(JsonTextField(encoded, "values_column_count").value_or(
        TextLineValue(encoded, "values_column_count").value_or("")));
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\trecursive_cte\n";
    const std::string recursive_iterations = JsonTextField(encoded, "recursive_iterations").value_or(
        TextLineValue(encoded, "recursive_iterations").value_or("32"));
    operation_envelope += "operand=text\trecursive_iterations\t";
    operation_envelope += EscapeOperationOperandField(recursive_iterations.empty() ? "32" : recursive_iterations);
    operation_envelope += "\n";
    constexpr std::string_view kRecursiveCteFields[] = {
        "recursive_step_mode",
        "recursive_counter_column",
        "recursive_counter_step",
        "recursive_counter_limit",
        "recursive_counter_predicate",
        "result_projection",
        "aggregate_function",
        "aggregate_value_field",
        "aggregate_pair_value_field",
        "order_by",
        "aggregate_fraction",
        "aggregate_limit",
        "listagg_separator",
        "listagg_overflow_mode",
        "listagg_max_output_bytes",
        "listagg_truncation_indicator",
        "listagg_with_count",
        "hypothetical_value",
        "hypothetical_value_type",
        "assertion_id",
        "actual_column_name",
        "expected_column_name",
        "expected_value",
        "expected_value_is_null"};
    for (const auto field : kRecursiveCteFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    if (column_count) {
      for (std::uint64_t relation = 0; relation < 2; ++relation) {
        const auto row_count = parse_u64(JsonTextField(encoded, "relation_" + std::to_string(relation) + "_row_count").value_or(
            TextLineValue(encoded, "relation_" + std::to_string(relation) + "_row_count").value_or("")));
        if (!row_count) continue;
        for (std::uint64_t row = 0; row < *row_count; ++row) {
          for (std::uint64_t column = 0; column < *column_count; ++column) {
            const std::string prefix = "relation_" + std::to_string(relation) + "_" +
                                       std::to_string(row) + "_" + std::to_string(column) + "_";
            const std::string name = JsonTextField(encoded, prefix + "name").value_or(
                TextLineValue(encoded, prefix + "name").value_or("c" + std::to_string(column)));
            const std::string type = JsonTextField(encoded, prefix + "type").value_or(
                TextLineValue(encoded, prefix + "type").value_or("bigint"));
            const std::string value = JsonTextField(encoded, prefix + "value").value_or(
                TextLineValue(encoded, prefix + "value").value_or(""));
            const std::string is_null = JsonTextField(encoded, prefix + "is_null").value_or(
                TextLineValue(encoded, prefix + "is_null").value_or("false"));
            operation_envelope += "operand=";
            operation_envelope += (is_null == "true" || is_null == "1") ? "row_null_field:" : "row_field:";
            operation_envelope += EscapeOperationOperandField(type.empty() ? "bigint" : type);
            operation_envelope += "\trelation-";
            operation_envelope += std::to_string(relation);
            operation_envelope += "-row-";
            operation_envelope += std::to_string(row);
            operation_envelope += "|";
            operation_envelope += EscapeOperationOperandField(name.empty() ? "c" + std::to_string(column) : name);
            operation_envelope += "\t";
            operation_envelope += EscapeOperationOperandField(value);
            operation_envelope += "\n";
          }
        }
      }
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_inner_join\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_inner_join") != std::string_view::npos)) {
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\t";
    operation_envelope += EscapeOperationOperandField(JsonTextField(encoded, "query_operation").value_or(
        TextLineValue(encoded, "query_operation").value_or("inner_join")));
    operation_envelope += "\n";
    operation_envelope += "operand=text\tjoin_algorithm\t";
    operation_envelope += EscapeOperationOperandField(JsonTextField(encoded, "join_algorithm").value_or(
        TextLineValue(encoded, "join_algorithm").value_or("hash")));
    operation_envelope += "\n";
    constexpr std::string_view kJoinFields[] = {
        "target_object_uuid", "target_object_kind",
        "related_object_0_uuid", "related_object_0_kind",
        "projection", "catalog_projection",
        "left_key_field", "right_key_field",
        "left_key_column", "right_key_column", "right_key_offset",
        "left_null_filter_field", "right_null_filter_field",
        "group_key_field", "aggregate_value_field",
        "lateral_filter_value", "cross_join_equality_filter",
        "distinct_count_field",
        "left_filter_count",
        "left_filter_0_kind", "left_filter_0_column",
        "left_filter_0_value", "left_filter_0_value_type",
        "left_filter_1_kind", "left_filter_1_column",
        "left_filter_1_value", "left_filter_1_value_type",
        "left_filter_2_kind", "left_filter_2_column",
        "left_filter_2_value", "left_filter_2_value_type",
        "left_filter_3_kind", "left_filter_3_column",
        "left_filter_3_value", "left_filter_3_value_type",
        "left_filter_4_kind", "left_filter_4_column",
        "left_filter_4_value", "left_filter_4_value_type",
        "left_filter_5_kind", "left_filter_5_column",
        "left_filter_5_value", "left_filter_5_value_type",
        "left_filter_6_kind", "left_filter_6_column",
        "left_filter_6_value", "left_filter_6_value_type",
        "left_filter_7_kind", "left_filter_7_column",
        "left_filter_7_value", "left_filter_7_value_type",
        "having_threshold",
        "partition_key_field", "order_by",
        "limit", "offset", "order_column", "order",
        "result_projection", "aggregate_function",
        "assertion_id", "actual_column_name",
        "expected_column_name", "expected_count", "expected_value",
        "count_compare_op", "count_compare_value"};
    for (const auto field : kJoinFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_set_operation\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_set_operation") != std::string_view::npos)) {
    const std::string set_operation = JsonTextField(encoded, "set_operation").value_or(
        TextLineValue(encoded, "set_operation").value_or("union_distinct"));
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\t";
    operation_envelope += EscapeOperationOperandField(set_operation.empty() ? "union_distinct" : set_operation);
    operation_envelope += "\n";
    operation_envelope += "operand=text\tset_operation\t";
    operation_envelope += EscapeOperationOperandField(set_operation.empty() ? "union_distinct" : set_operation);
    operation_envelope += "\n";
    constexpr std::string_view kTableSetFields[] = {
        "target_object_uuid", "target_object_kind",
        "related_object_0_uuid", "related_object_0_kind",
        "set_by_name", "left_project_field", "right_project_field",
        "result_projection", "aggregate_function", "aggregate_value_field",
        "assertion_id", "actual_column_name", "expected_column_name",
        "expected_count", "expected_value", "limit", "offset"};
    for (const auto field : kTableSetFields) {
      std::string value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty() && field == std::string_view("set_by_name")) {
        const std::string true_needle = "\"" + std::string(field) + "\":true";
        const std::string false_needle = "\"" + std::string(field) + "\":false";
        if (encoded.find(true_needle) != std::string_view::npos) {
          value = "true";
        } else if (encoded.find(false_needle) != std::string_view::npos) {
          value = "false";
        }
      }
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    for (std::size_t index = 1; index < 16; ++index) {
      const std::string prefix = "related_object_" + std::to_string(index) + "_";
      for (const auto suffix : {"uuid", "kind"}) {
        const std::string field = prefix + suffix;
        const auto value = JsonTextField(encoded, field).value_or(
            TextLineValue(encoded, field).value_or(""));
        if (value.empty()) continue;
        operation_envelope += "operand=text\t";
        operation_envelope += field;
        operation_envelope += "\t";
        operation_envelope += EscapeOperationOperandField(value);
        operation_envelope += "\n";
      }
    }
    for (std::size_t index = 0; index < 16; ++index) {
      const std::string field = "relation_" + std::to_string(index) + "_project_field";
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    for (std::size_t index = 0; index < 16; ++index) {
      const std::string field = "relation_" + std::to_string(index) + "_not_null_filter_field";
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    for (std::size_t index = 0; index < 16; ++index) {
      for (const auto suffix : {"filter_kind", "filter_field", "filter_value",
                                "filter_value_type"}) {
        const std::string field = "relation_" + std::to_string(index) + "_" + suffix;
        const auto value = JsonTextField(encoded, field).value_or(
            TextLineValue(encoded, field).value_or(""));
        if (value.empty()) continue;
        operation_envelope += "operand=text\t";
        operation_envelope += field;
        operation_envelope += "\t";
        operation_envelope += EscapeOperationOperandField(value);
        operation_envelope += "\n";
      }
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_row_number_window\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_row_number_window") != std::string_view::npos ||
       encoded.find("\"query_envelope_kind\":\"table_window\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_window") != std::string_view::npos ||
       encoded.find("\"query_envelope_kind\":\"table_partition_count_window\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_partition_count_window") != std::string_view::npos)) {
    const std::string query_operation = JsonTextField(encoded, "query_operation").value_or(
        TextLineValue(encoded, "query_operation").value_or("row_number_window"));
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\t";
    operation_envelope += EscapeOperationOperandField(query_operation.empty() ? "row_number_window" : query_operation);
    operation_envelope += "\n";
    constexpr std::string_view kWindowFields[] = {
        "target_object_uuid", "target_object_kind",
        "order_by", "order_column", "window_function",
        "window_value_field", "window_value_column",
        "partition_by", "partition_column",
        "window_n", "window_bucket_count",
        "limit", "offset"};
    for (const auto field : kWindowFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_group_sum\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_group_sum") != std::string_view::npos ||
       encoded.find("\"query_envelope_kind\":\"table_group_aggregate\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_group_aggregate") != std::string_view::npos)) {
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tgroup_by\n";
    constexpr std::string_view kGroupFields[] = {
        "target_object_uuid", "target_object_kind",
        "group_key_field", "aggregate_value_field", "aggregate_function",
        "group_key_column", "aggregate_value_column",
        "aggregate_pair_value_field", "aggregate_pair_value_column",
        "aggregate_fraction", "aggregate_limit",
        "order_by", "order_column",
        "having_predicate", "having_threshold",
        "having_aggregate_function", "having_value_field",
        "having_value_column",
        "listagg_separator", "listagg_overflow_mode", "listagg_max_output_bytes",
        "listagg_truncation_indicator", "listagg_with_count",
        "limit", "offset"};
    for (const auto field : kGroupFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_count\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_count") != std::string_view::npos)) {
    const std::string original_target_uuid = JsonTextField(encoded, "target_object_uuid").value_or(
        TextLineValue(encoded, "target_object_uuid").value_or(""));
    std::optional<DispatchViewDescriptor> view_descriptor;
    if (!original_target_uuid.empty()) {
      view_descriptor = LoadDispatchViewDescriptor(session, original_target_uuid);
    }
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tcount_all\n";
    if (view_descriptor) {
      AppendOperationOperand(&operation_envelope, "target_object_uuid", view_descriptor->source_uuid);
      AppendOperationOperand(&operation_envelope, "target_object_kind", "table");
      AppendViewPredicateOperands(*view_descriptor, encoded, &operation_envelope);
    }
    constexpr std::string_view kCountFields[] = {
        "target_object_uuid", "target_object_kind",
        "aggregate_function", "aggregate_value_field",
        "count_all", "count_distinct", "count_distinct_include_null", "limit", "offset",
        "result_projection", "assertion_id", "actual_column_name",
        "expected_column_name", "expected_count", "expected_value",
        "count_compare_op", "count_compare_value",
        "predicate_kind", "predicate_column", "predicate_value",
        "predicate_value_type"};
    for (const auto field : kCountFields) {
      if (view_descriptor &&
          (field == "target_object_uuid" || field == "target_object_kind" ||
           field == "predicate_kind" || field == "predicate_column" ||
           field == "predicate_value" || field == "predicate_value_type")) {
        continue;
      }
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_materialized_cte\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_materialized_cte") != std::string_view::npos)) {
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tmaterialized_cte\n";
    constexpr std::string_view kCteFields[] = {
        "target_object_uuid", "target_object_kind",
        "limit", "offset"};
    for (const auto field : kCteFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "query.plan_operation" &&
      (encoded.find("\"query_envelope_kind\":\"table_scalar_subquery\"") != std::string_view::npos ||
       encoded.find("query_envelope_kind=table_scalar_subquery") != std::string_view::npos)) {
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tscalar_subquery\n";
    constexpr std::string_view kSubqueryFields[] = {
        "target_object_uuid", "target_object_kind",
        "project_columns", "limit", "offset"};
    for (const auto field : kSubqueryFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
  }
  mark_phase("pre_dml_bridge");
  return finish_operation_envelope();
}

void ApplyTransactionResultToSession(std::string_view operation_id,
                                     std::string_view payload,
                                     ServerSessionRecord* session) {
  if (session == nullptr) return;
  if (operation_id == "transaction.begin") {
    if (const auto local_id = TextLineU64(payload, "local_transaction_id")) {
      session->local_transaction_id = *local_id;
      session->snapshot_visible_through_local_transaction_id =
          EvidenceU64(payload, "snapshot_visible_through_local_transaction_id").value_or(
              TextLineU64(payload, "snapshot_visible_through_local_transaction_id").value_or(0));
      session->transaction_uuid = TextLineValue(payload, "transaction_uuid").value_or("");
      session->transaction_timestamp = EvidenceText(payload, "transaction_timestamp").value_or(
          TextLineValue(payload, "transaction_timestamp").value_or(""));
    }
    return;
  }
  if (operation_id == "transaction.set_characteristics") {
    const std::string read_only =
        EvidenceText(payload, "transaction_read_only").value_or(
            TextLineValue(payload, "transaction_read_only").value_or(
                session->default_transaction_read_only ? "true" : "false"));
    session->default_transaction_read_only =
        read_only == "true" || read_only == "1" || read_only == "yes" || read_only == "on";
    const std::string isolation =
        EvidenceText(payload, "transaction_isolation_level").value_or(
            TextLineValue(payload, "transaction_isolation_level").value_or(""));
    if (!isolation.empty()) {
      session->default_transaction_isolation_level = isolation;
    }
    return;
  }
  if (operation_id == "transaction.commit" || operation_id == "transaction.rollback") {
    if (const auto replacement_id = TextLineU64(payload, "replacement_local_transaction_id")) {
      session->local_transaction_id = *replacement_id;
      session->snapshot_visible_through_local_transaction_id =
          TextLineU64(payload, "replacement_snapshot_visible_through_local_transaction_id").value_or(0);
      session->transaction_uuid =
          TextLineValue(payload, "replacement_transaction_uuid").value_or("");
      session->transaction_timestamp =
          TextLineValue(payload, "replacement_transaction_timestamp").value_or("");
    }
  }
}

std::string FirstEngineDiagnosticCode(sb_engine_result_t result) {
  if (result == nullptr) return {};
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK ||
      diagnostics.diagnostic_count == 0 || diagnostics.diagnostics == nullptr) {
    return {};
  }
  return StringViewToString(diagnostics.diagnostics[0].symbolic_code);
}

std::string FirstEngineDiagnosticDetail(sb_engine_result_t result) {
  if (result == nullptr) return {};
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK ||
      diagnostics.diagnostic_count == 0 || diagnostics.diagnostics == nullptr) {
    return {};
  }
  return StringViewToString(diagnostics.diagnostics[0].safe_detail);
}

struct PublicAbiDispatchResult {
  bool attempted = false;
  bool ok = false;
  std::uint64_t row_count = 0;
  std::uint64_t affected_rows = 0;
  std::string payload;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  sb_engine_result_t result_handle = nullptr;
};

std::string ServerApiRowValue(const engine_api::EngineApiResult& api_result,
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

std::string ServerApiRowMetadataValue(const engine_api::EngineApiResult& api_result,
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
    out << field.first << ":" << type_name << ":"
        << (field.second.is_null ? "null" : "not_null");
  }
  return out.str();
}

std::string ServerApiResultPayload(const engine_api::EngineApiResult& api_result) {
  std::ostringstream out;
  const std::uint64_t row_count =
      static_cast<std::uint64_t>(api_result.result_shape.rows.size());
  out << "operation_id=" << api_result.operation_id << "\n";
  out << "result_kind=" << api_result.result_shape.result_kind << "\n";
  out << "row_count=" << row_count << "\n";
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    const auto index = static_cast<std::size_t>(row_index);
    out << "row[" << row_index << "]=" << ServerApiRowValue(api_result, index) << "\n";
    out << "row_meta[" << row_index << "]="
        << ServerApiRowMetadataValue(api_result, index) << "\n";
  }
  for (const auto& evidence : api_result.evidence) {
    out << "evidence=" << evidence.evidence_kind << ":" << evidence.evidence_id << "\n";
  }
  return out.str();
}

std::string EncodedOrOperandTextField(std::string_view encoded, std::string_view field) {
  return ExistingTextOperandValue(encoded, field).value_or(
      JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or("")));
}

std::string CatalogProjectionPathForDispatch(std::string_view encoded) {
  std::string projection_path = ServerVirtualProjectionForTarget(encoded);
  if (!projection_path.empty()) {
    return projection_path;
  }
  projection_path = EncodedOrOperandTextField(encoded, "projection");
  if (projection_path.empty()) {
    projection_path = EncodedOrOperandTextField(encoded, "catalog_projection");
  }
  return projection_path;
}

bool IsServerLiveIparCatalogProjection(std::string_view encoded) {
  const std::string projection_path = CatalogProjectionPathForDispatch(encoded);
  return projection_path.rfind("sys.ipar.", 0) == 0;
}

void AddCatalogProjectionDispatchOptions(engine_api::EngineApiRequest* request,
                                         std::string_view encoded) {
  if (request == nullptr) {
    return;
  }
  const std::string projection_path = CatalogProjectionPathForDispatch(encoded);
  if (!projection_path.empty()) {
    request->option_envelopes.push_back("projection:" + projection_path);
    request->option_envelopes.push_back("catalog_projection:" + projection_path);
  }
  constexpr std::string_view kCatalogProjectionFields[] = {
      "result_projection",
      "aggregate_function",
      "assertion_id",
      "actual_column_name",
      "expected_column_name",
      "expected_count",
      "expected_value",
      "count_compare_op",
      "count_compare_value",
      "predicate_kind",
      "predicate_column",
      "predicate_value",
      "predicate_value_type",
      "additional_predicate_kind",
      "additional_predicate_column",
      "additional_predicate_value",
      "additional_predicate_value_type",
      "subquery_projection",
      "subquery_select_column",
      "subquery_predicate_kind",
      "subquery_predicate_column",
      "subquery_predicate_value",
      "subquery_predicate_value_type",
      "subquery_additional_predicate_kind",
      "subquery_additional_predicate_column",
      "subquery_additional_predicate_value",
      "subquery_additional_predicate_value_type",
      "subquery_nested_projection",
      "subquery_nested_select_column",
      "subquery_nested_predicate_kind",
      "subquery_nested_predicate_column",
      "subquery_nested_predicate_value",
      "subquery_nested_predicate_value_type"};
  for (const auto field : kCatalogProjectionFields) {
    const std::string value = EncodedOrOperandTextField(encoded, field);
    if (!value.empty()) {
      request->option_envelopes.push_back(std::string(field) + ":" + value);
    }
  }
}

PublicAbiDispatchResult DispatchServerLiveIparCatalogProjection(
    const ServerSessionRecord& session,
    const std::string& encoded,
    const ServerIparProjectionSources& ipar_sources) {
  PublicAbiDispatchResult dispatch_result;
  dispatch_result.attempted = true;
  engine_api::EngineShowCatalogRequest request;
  request.context = PublicAbiDispatchEngineContext(session);
  AddCatalogProjectionDispatchOptions(&request, encoded);
  request.ipar_agent_lifecycle = ipar_sources.agent_lifecycle;
  request.ipar_metric_counters = ipar_sources.metric_counters;
  request.ipar_telemetry_controls = ipar_sources.telemetry_controls;
  request.ipar_slow_path_reasons = ipar_sources.slow_path_reasons;
  const auto api_result = engine_api::EngineShowCatalog(request);
  dispatch_result.ok = api_result.ok;
  dispatch_result.row_count =
      static_cast<std::uint64_t>(api_result.result_shape.rows.size());
  dispatch_result.payload = ServerApiResultPayload(api_result);
  if (!api_result.ok) {
    dispatch_result.diagnostic_code = api_result.diagnostics.empty()
                                          ? "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED"
                                          : api_result.diagnostics.front().code;
    dispatch_result.diagnostic_detail = api_result.diagnostics.empty()
                                            ? "engine_dispatch_rejected"
                                            : api_result.diagnostics.front().detail;
  }
  return dispatch_result;
}

PublicAbiDispatchResult DispatchThroughPublicAbi(ServerSessionRegistry* registry,
                                                 const ServerSessionRecord& session,
                                                 std::string_view operation_id,
                                                 std::string_view operation_family,
                                                 const std::string& encoded,
                                                 const std::vector<std::uint8_t>& data_packet,
                                                 bool retain_result_handle) {
  PublicAbiDispatchResult dispatch_result;
  dispatch_result.attempted = true;
  const auto phase_start = ServerSteadyClock::now();
  auto phase_last = phase_start;
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(12);
  const auto mark_phase = [&](std::string phase) {
    const auto now = ServerSteadyClock::now();
    phase_micros.push_back({std::move(phase), ServerElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const std::string public_abi_envelope =
      PublicAbiEnvelopeForDispatch(session, encoded, operation_id, operation_family);
  mark_phase("public_abi_envelope");

  if (registry == nullptr) {
    dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
    dispatch_result.diagnostic_detail = "session_registry_missing";
    WriteServerPhaseTrace("SCRATCHBIRD_PUBLIC_ABI_DISPATCH_TRACE_FILE",
                          "public_abi_dispatch",
                          operation_id,
                          encoded.size(),
                          phase_micros);
    return dispatch_result;
  }
  const std::string session_key = UuidBytesToText(session.session_uuid);
  auto cached_it = registry->public_abi_sessions_by_session_uuid.find(session_key);
  if (cached_it != registry->public_abi_sessions_by_session_uuid.end()) {
    const auto& cached = cached_it->second;
    const bool cache_matches =
        cached.engine != nullptr &&
        cached.engine_session != nullptr &&
        cached.database_path == session.database_path &&
        cached.session_uuid == session.session_uuid &&
        cached.effective_user_uuid == session.effective_user_uuid &&
        cached.embedded_in_process == session.embedded_in_process;
    if (!cache_matches) {
      CloseServerPublicAbiSessionForSession(registry,
                                            session.session_uuid,
                                            "public_abi_context_authority_changed");
      cached_it = registry->public_abi_sessions_by_session_uuid.end();
    }
  }
  mark_phase("public_abi_cache_lookup");
  if (cached_it == registry->public_abi_sessions_by_session_uuid.end()) {
    sb_engine_open_params_v1_t open_params{};
    open_params.struct_size = sizeof(open_params);
    open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open_params.database_path_utf8 = session.database_path.data();
    open_params.database_path_size =
        static_cast<std::uint64_t>(session.database_path.size());
    open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    sb_engine_handle_t engine = nullptr;
    const auto open_status = sb_engine_open(&open_params, &engine, nullptr);
    mark_phase("engine_open_miss");
    if (open_status != SB_ENGINE_STATUS_OK || engine == nullptr) {
      dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
      dispatch_result.diagnostic_detail = "engine_open_failed";
      WriteServerPhaseTrace("SCRATCHBIRD_PUBLIC_ABI_DISPATCH_TRACE_FILE",
                            "public_abi_dispatch",
                            operation_id,
                            encoded.size(),
                            phase_micros);
      return dispatch_result;
    }
    sb_engine_session_params_v1_t session_params{};
    session_params.struct_size = sizeof(session_params);
    session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    std::memcpy(session_params.effective_user_uuid.bytes,
                session.effective_user_uuid.data(),
                16);
    std::memcpy(session_params.session_uuid.bytes,
                session.session_uuid.data(),
                16);
    session_params.trust_mode = session.embedded_in_process
                                    ? SB_ENGINE_TRUST_EMBEDDED_TRUSTED
                                    : SB_ENGINE_TRUST_SERVER_ISOLATED;
    session_params.default_language_utf8 = "en";
    session_params.default_language_size = 2;
    sb_engine_session_t engine_session = nullptr;
    const auto session_status =
        sb_engine_session_begin(engine, &session_params, &engine_session, nullptr);
    mark_phase("engine_session_begin_miss");
    if (session_status != SB_ENGINE_STATUS_OK || engine_session == nullptr) {
      (void)sb_engine_close(engine, nullptr);
      dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
      dispatch_result.diagnostic_detail = "engine_session_begin_failed";
      WriteServerPhaseTrace("SCRATCHBIRD_PUBLIC_ABI_DISPATCH_TRACE_FILE",
                            "public_abi_dispatch",
                            operation_id,
                            encoded.size(),
                            phase_micros);
      return dispatch_result;
    }
    ServerPublicAbiSessionContext cached_context;
    cached_context.engine = engine;
    cached_context.engine_session = engine_session;
    cached_context.database_path = session.database_path;
    cached_context.session_uuid = session.session_uuid;
    cached_context.effective_user_uuid = session.effective_user_uuid;
    cached_context.embedded_in_process = session.embedded_in_process;
    cached_it =
        registry->public_abi_sessions_by_session_uuid
            .emplace(session_key, std::move(cached_context))
            .first;
  } else {
    mark_phase("engine_context_reuse");
  }

  auto& cached_context = cached_it->second;
  ++cached_context.reuse_count;
  if (cached_context.engine_session != nullptr) {
    sb_engine_request_context_v1_t context{};
    context.struct_size = sizeof(context);
    context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    context.trust_mode = session.embedded_in_process ? SB_ENGINE_TRUST_EMBEDDED_TRUSTED
                                                     : SB_ENGINE_TRUST_SERVER_ISOLATED;
    std::memcpy(context.effective_user_uuid.bytes, session.effective_user_uuid.data(), 16);
    std::memcpy(context.session_uuid.bytes, session.session_uuid.data(), 16);
    context.rights_set_ref = 1;
    context.capability_set_ref = 1;
    context.transaction_ref =
        operation_id == "transaction.set_characteristics" ? 0 : session.local_transaction_id;
    sb_engine_sblr_dispatch_params_v1_t dispatch{};
    dispatch.struct_size = sizeof(dispatch);
    dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    dispatch.envelope_bytes = reinterpret_cast<const std::uint8_t*>(public_abi_envelope.data());
    dispatch.envelope_size_bytes = static_cast<std::uint64_t>(public_abi_envelope.size());
    dispatch.data_packet_bytes = data_packet.empty() ? nullptr : data_packet.data();
    dispatch.data_packet_size_bytes = static_cast<std::uint64_t>(data_packet.size());
    sb_engine_result_t abi_result = nullptr;
    const auto status = sb_engine_dispatch_sblr(cached_context.engine_session,
                                                nullptr,
                                                &context,
                                                &dispatch,
                                                &abi_result);
    mark_phase("engine_dispatch_sblr");
    dispatch_result.ok = status == SB_ENGINE_STATUS_OK;
    if (abi_result != nullptr) {
      sb_engine_execution_summary_view_v1_t summary{};
      if (sb_engine_result_summary(abi_result, &summary) == SB_ENGINE_STATUS_OK) {
        dispatch_result.row_count = summary.rows_produced;
      }
      mark_phase("result_summary");
      sb_engine_command_completion_view_v1_t completion{};
      if (sb_engine_result_completion(abi_result, &completion) == SB_ENGINE_STATUS_OK) {
        dispatch_result.affected_rows = completion.affected_rows;
      }
      mark_phase("result_completion");
      if (!dispatch_result.ok || !retain_result_handle) {
        sb_engine_string_view_t payload{};
        if (sb_engine_result_payload(abi_result, &payload) == SB_ENGINE_STATUS_OK) {
          dispatch_result.payload = StringViewToString(payload);
        }
      }
      mark_phase("result_payload");
      if (!dispatch_result.ok) {
        dispatch_result.diagnostic_code = FirstEngineDiagnosticCode(abi_result);
        dispatch_result.diagnostic_detail = FirstEngineDiagnosticDetail(abi_result);
      }
      mark_phase("result_diagnostics");
      if (dispatch_result.ok && retain_result_handle) {
        dispatch_result.result_handle = abi_result;
      } else {
        (void)sb_engine_result_release(abi_result);
      }
      mark_phase("result_release_or_retain");
    }
    if (!dispatch_result.ok && dispatch_result.diagnostic_code.empty()) {
      dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
    }
  } else {
    dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
    dispatch_result.diagnostic_detail = "engine_session_missing";
  }
  mark_phase("engine_context_retained");
  WriteServerPhaseTrace("SCRATCHBIRD_PUBLIC_ABI_DISPATCH_TRACE_FILE",
                        "public_abi_dispatch",
                        operation_id,
                        encoded.size(),
                        phase_micros);
  return dispatch_result;
}

void ReleaseCursorEngineResult(ServerCursorRecord* cursor) {
  if (cursor == nullptr || cursor->engine_result == nullptr) {
    return;
  }
  (void)sb_engine_result_release(cursor->engine_result);
  cursor->engine_result = nullptr;
}

struct EngineCursorBatch {
  bool ok = false;
  std::uint64_t row_count = 0;
  std::string row_packet;
  bool end_of_stream = true;
  std::string diagnostic_detail;
};

EngineCursorBatch FetchEngineCursorBatch(sb_engine_result_t result,
                                         std::uint64_t max_rows,
                                         std::uint64_t max_bytes) {
  EngineCursorBatch out;
  if (result == nullptr) {
    out.diagnostic_detail = "engine_result_missing";
    return out;
  }
  sb_engine_batch_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  request.max_rows = max_rows;
  request.max_bytes = max_bytes;
  sb_engine_row_batch_view_v1_t batch{};
  const auto status = sb_engine_result_next_batch(result, &request, &batch);
  if (status != SB_ENGINE_STATUS_OK) {
    out.diagnostic_detail = std::string("engine_next_batch_") + sb_engine_status_name(status);
    return out;
  }
  sb_engine_string_view_t payload{};
  if (sb_engine_result_payload(result, &payload) != SB_ENGINE_STATUS_OK) {
    out.diagnostic_detail = "engine_batch_payload_unavailable";
    return out;
  }
  out.ok = true;
  out.row_count = batch.row_count;
  out.row_packet = StringViewToString(payload);
  out.end_of_stream = batch.end_of_stream != 0;
  return out;
}

std::string CursorMetadataDetail(const ServerCursorRecord& cursor) {
  std::ostringstream out;
  out << "{\"cursor_metadata\":{\"operation_id\":\"" << JsonEscape(cursor.operation_id) << "\","
      << "\"metadata_contract\":\"cursor.metadata.v1\","
      << "\"fetch_count\":" << cursor.fetch_count << ","
      << "\"next_row_index\":" << cursor.next_row_index << ","
      << "\"total_rows\":" << cursor.total_row_count << ","
      << "\"max_chunk_rows\":" << cursor.max_chunk_rows << ","
      << "\"max_chunk_bytes\":" << cursor.max_chunk_bytes << ","
      << "\"capability\":\"forward_only\","
      << "\"scrollable\":false,\"updatable\":false,\"holdable\":false,"
      << "\"end_of_cursor\":" << (cursor.exhausted ? "true" : "false") << ","
      << "\"finality\":{\"kind\":\"" << JsonEscape(cursor.finality_kind) << "\","
      << "\"state\":\"" << JsonEscape(cursor.finality_state) << "\","
      << "\"reason\":\"" << JsonEscape(cursor.finality_reason) << "\"}"
      << "}}";
  return out.str();
}

std::optional<std::array<std::uint8_t, 16>> ParseUuidTextForDispatch(
    std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::size_t nibble = 0;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return std::nullopt;
    if ((nibble % 2) == 0) {
      out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    }
    ++nibble;
  }
  if (nibble != 32) return std::nullopt;
  return out;
}

std::string RoutineCursorInvocationDetail(const ServerCursorRecord& cursor,
                                          std::string_view context_kind,
                                          std::string_view action,
                                          std::string_view borrow_policy,
                                          std::string_view cleanup_state) {
  std::ostringstream out;
  out << "{\"routine_cursor_argument\":{\"contract\":\"routine.cursor_argument.v1\","
      << "\"cursor_uuid\":\"" << JsonEscape(UuidBytesToText(cursor.cursor_uuid))
      << "\",\"context_kind\":\"" << JsonEscape(context_kind)
      << "\",\"action\":\"" << JsonEscape(action)
      << "\",\"borrow_policy\":\"" << JsonEscape(borrow_policy)
      << "\",\"cleanup_state\":\"" << JsonEscape(cleanup_state)
      << "\",\"descriptor_bound\":true,"
      << "\"security_rechecked\":true,"
      << "\"protected_material_rechecked\":true,"
      << "\"parser_executes_cursor\":false,"
      << "\"fetch_count\":" << cursor.fetch_count
      << ",\"next_row_index\":" << cursor.next_row_index
      << ",\"total_rows\":" << cursor.total_row_count
      << ",\"closed\":" << (cursor.closed ? "true" : "false")
      << ",\"exhausted\":" << (cursor.exhausted ? "true" : "false")
      << "}}";
  return out.str();
}

std::string StreamFinalityPacket(const ServerCursorRecord& cursor,
                                 const std::string& state,
                                 const std::string& reason) {
  std::ostringstream out;
  out << "{\"stream_finality\":{\"cursor_uuid\":\"" << JsonEscape(UuidBytesToText(cursor.cursor_uuid))
      << "\",\"operation_id\":\"" << JsonEscape(cursor.operation_id)
      << "\",\"state\":\"" << JsonEscape(state)
      << "\",\"reason\":\"" << JsonEscape(reason)
      << "\",\"fetch_count\":" << cursor.fetch_count
      << ",\"deterministic\":true}}\n";
  return out.str();
}

void MarkCursorFinality(ServerCursorRecord* cursor,
                        std::string state,
                        std::string reason) {
  if (cursor == nullptr) return;
  cursor->finality_state = std::move(state);
  cursor->finality_reason = std::move(reason);
  cursor->exhausted = true;
  ReleaseCursorEngineResult(cursor);
}

SessionOperationResult AcceptedFetchFinality(const std::array<std::uint8_t, 16>& session_uuid,
                                             const std::array<std::uint8_t, 16>& cursor_uuid,
                                             const std::string& packet,
                                             const ServerCursorRecord& cursor) {
  SessionOperationResult result;
  result.accepted = true;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetchResult);
  result.response_schema_id = kSchemaFetchResultTestV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = session_uuid;
  result.payload = EncodeFetchResult(cursor_uuid, 1, packet, true, CursorMetadataDetail(cursor));
  return result;
}

}  // namespace

std::vector<std::uint8_t> EncodePrepareSblrPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::string& encoded_sblr_envelope) {
  std::vector<std::uint8_t> out;
  out.reserve(16 + 16 + 8 + 8 + 8 + 2 + encoded_sblr_envelope.size());
  PutUuid(&out, session_uuid);
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  PutU64(&out, 1);
  PutU64(&out, 1);
  PutU64(&out, 1);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodeExecuteSblrPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    const std::string& encoded_sblr_envelope,
    bool cursor_requested,
    const std::vector<std::uint8_t>& data_packet) {
  std::vector<std::uint8_t> out;
  out.reserve(16 + 16 + 1 + 2 + encoded_sblr_envelope.size() +
              (data_packet.empty() ? 0 : 8 + data_packet.size()));
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  if (!data_packet.empty()) {
    PutBytes(&out, data_packet);
  }
  return out;
}

std::vector<std::uint8_t> EncodeFetchPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid,
    std::uint64_t max_rows,
    std::uint64_t max_bytes,
    std::uint32_t fetch_flags) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, cursor_uuid);
  PutU64(&out, max_rows);
  PutU64(&out, max_bytes);
  PutU32(&out, fetch_flags);
  return out;
}

std::vector<std::uint8_t> EncodeCloseCursorPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid) {
  return EncodeFetchPayloadForTest(session_uuid, cursor_uuid);
}

std::vector<std::uint8_t> EncodeCancelCursorPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid) {
  return EncodeFetchPayloadForTest(session_uuid, cursor_uuid, 1, 0, kCursorCloseFlagCancel);
}

std::optional<std::array<std::uint8_t, 16>> DecodePreparedStatementUuidForTest(
    const std::vector<std::uint8_t>& prepare_result_payload) {
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(prepare_result_payload, &offset, &outcome)) return std::nullopt;
  if (offset + 16 > prepare_result_payload.size()) return std::nullopt;
  return GetUuid(prepare_result_payload, offset);
}

std::optional<std::array<std::uint8_t, 16>> DecodeCursorUuidForTest(
    const std::vector<std::uint8_t>& execute_result_payload) {
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(execute_result_payload, &offset, &outcome)) return std::nullopt;
  if (offset + 32 > execute_result_payload.size()) return std::nullopt;
  offset += 16;
  return GetUuid(execute_result_payload, offset);
}

std::optional<FetchResultForTest> DecodeFetchResultForTest(
    const std::vector<std::uint8_t>& fetch_result_payload) {
  if (fetch_result_payload.size() < 16 + 8) return std::nullopt;
  FetchResultForTest result;
  std::size_t offset = 0;
  result.cursor_uuid = GetUuid(fetch_result_payload, offset);
  offset += 16;
  result.row_count = GetU64(fetch_result_payload, offset);
  offset += 8;
  if (!ReadString(fetch_result_payload, &offset, &result.row_packet)) return std::nullopt;
  if (offset >= fetch_result_payload.size()) return std::nullopt;
  result.end_of_cursor = fetch_result_payload[offset++] != 0;
  if (!ReadString(fetch_result_payload, &offset, &result.detail)) return std::nullopt;
  return result;
}

std::string EncodeShowVersionSblrForTest() { return "sblr.query.show.version"; }
std::string EncodeRawSqlSblrBypassForTest() { return "select * from forbidden_raw_sql"; }
std::string EncodeClusterSblrForTest() { return "sblr.cluster.route.inspect"; }
std::string EncodeCrudInsertSblrForTest() { return "sblr.crud.insert"; }
std::string EncodeCrudSelectSblrForTest() { return "sblr.crud.select"; }
std::string EncodeCrudUpdateSblrForTest() { return "sblr.crud.update"; }
std::string EncodeCrudDeleteSblrForTest() { return "sblr.crud.delete"; }
std::string EncodeCatalogCreateTableSblrForTest() { return "sblr.catalog.create_table"; }
std::string EncodeCatalogGetDescriptorSblrForTest() { return "sblr.catalog.get_descriptor"; }
std::string EncodeIndexCreateSblrForTest() { return "sblr.index.create"; }
std::string EncodeDatatypeCastSblrForTest() { return "sblr.datatype.cast"; }
std::string EncodeDatatypeExtractSblrForTest() { return "sblr.datatype.extract"; }
std::string EncodeDatatypeSetSblrForTest() { return "sblr.datatype.set"; }
std::string EncodeOptimizerExplainSblrForTest() { return "sblr.optimizer.explain"; }
std::string EncodeOptimizerPlanSblrForTest() { return "sblr.optimizer.plan"; }
std::string EncodeLlvmCompileSblrForTest() { return "sblr.llvm.compile"; }
std::string EncodeBeginTransactionSblrForTest() { return "sblr.transaction.begin"; }
std::string EncodeEventChannelCreateSblrForTest(const std::string& channel_uuid) {
  return "sblr.event.channel.create:" + channel_uuid;
}
std::string EncodeEventChannelNotifySblrForTest(const std::string& channel_uuid,
                                                const std::string& payload) {
  return "sblr.event.channel.notify:" + channel_uuid + ":" + payload;
}

SessionOperationResult HandlePrepareSblr(ServerSessionRegistry* registry,
                                         const HostedEngineState&,
                                         const sbps::Frame& request) {
  auto decoded = DecodePreparePayload(request.payload);
  auto session = decoded ? FindSession(*registry, decoded->session_uuid) : std::nullopt;
  if (!decoded || !session) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kPrepareResult),
                   kSchemaPrepareResultTestV1,
                   request.header.session_uuid,
                   "PARSER_SERVER_IPC.PREPARE_INVALID",
                   "The SBLR prepare payload is invalid.",
                   "prepare_invalid");
  }
  auto request_record = RegisterServerRequestLifecycle(registry,
                                                       request,
                                                       *session,
                                                       "prepare_sblr",
                                                       "sblr.prepare.pending");
  const auto admission = AdmitServerSblrEnvelope(
      ServerSblrAdmissionRequest{decoded->encoded_sblr_envelope, false});
  if (!admission.admitted) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   admission.diagnostics.empty()
                                       ? "sblr_admission_rejected"
                                       : admission.diagnostics.front().code);
    return FailureWithDiagnostics(
        static_cast<std::uint16_t>(sbps::MessageType::kPrepareResult),
        kSchemaPrepareResultTestV1,
        decoded->session_uuid,
        admission.diagnostics,
        admission.diagnostics.empty() ? "sblr_admission_rejected"
                                      : admission.diagnostics.front().code);
  }
  UpdateServerRequestLifecycleOperation(registry,
                                        request_record.request_uuid,
                                        admission.operation_id);
  ServerPreparedStatementRecord prepared;
  prepared.prepared_statement_uuid = sbps::MakeUuidV7Bytes();
  prepared.client_statement_uuid = decoded->client_statement_uuid;
  CapturePreparedAuthorityContext(&prepared, *session);
  prepared.encoded_sblr_envelope = decoded->encoded_sblr_envelope;
  prepared.operation_family = admission.operation_family;
  prepared.operation_id = admission.operation_id;
  prepared.requires_public_abi_dispatch = admission.requires_public_abi_dispatch;
  prepared.row_count_hint = admission.row_count_hint;
  prepared.catalog_generation = session->catalog_generation;
  prepared.security_epoch = session->security_epoch;
  prepared.descriptor_epoch = session->descriptor_epoch;
  prepared.grant_epoch = session->grant_epoch;
  prepared.policy_generation = session->policy_generation;
  prepared.role_set_hash = session->role_set_hash;
  prepared.group_set_hash = session->group_set_hash;
  prepared.search_path_hash = session->search_path_hash;
  CapturePreparedLanguageContext(&prepared, *session);
  BindPreparedSessionObjectHandle(registry,
                                  &prepared,
                                  *session,
                                  prepared.encoded_sblr_envelope,
                                  prepared.operation_id);
  SealPreparedAuthorityProof(&prepared, *session);
  StorePreparedExecutionContext(registry, prepared, *session);
  registry->prepared_by_uuid[UuidBytesToText(prepared.prepared_statement_uuid)] = prepared;
  LinkServerRequestPreparedStatement(registry,
                                     request_record.request_uuid,
                                     prepared.prepared_statement_uuid);
  CompleteServerRequestLifecycle(registry,
                                 request_record.request_uuid,
                                 ServerRequestLifecycleState::kCompleted,
                                 "prepare_completed");

  SessionOperationResult result;
  result.accepted = true;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareResult);
  result.response_schema_id = kSchemaPrepareResultTestV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = decoded->session_uuid;
  result.payload = EncodePrepareResult("accepted", prepared.prepared_statement_uuid, prepared.operation_id);
  return result;
}

SessionOperationResult HandleExecuteSblr(ServerSessionRegistry* registry,
                                         const HostedEngineState& engine_state,
                                         const sbps::Frame& request,
                                         const ServerIparProjectionSourceFactory* ipar_source_factory) {
  auto execute_phase_last = ServerSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> execute_phase_micros;
  execute_phase_micros.reserve(24);
  const auto mark_execute_phase = [&](std::string phase) {
    const auto now = ServerSteadyClock::now();
    execute_phase_micros.push_back(
        {std::move(phase), ServerElapsedMicros(execute_phase_last, now)});
    execute_phase_last = now;
  };
  auto decoded = DecodeExecutePayload(request.payload);
  mark_execute_phase("decode_execute_payload");
  if (!decoded) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   request.header.session_uuid,
                   "PARSER_SERVER_IPC.EXECUTE_INVALID",
                   "The SBLR execute payload is invalid.",
                   "execute_invalid");
  }
  ServerSessionRecord* session = FindMutableSession(registry, decoded->session_uuid);
  mark_execute_phase("find_session");
  if (session == nullptr) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.SESSION_REQUIRED",
                   "Execute requires a bound session.",
                   "session_required");
  }
  auto request_record = RegisterServerRequestLifecycle(registry,
                                                       request,
                                                       *session,
                                                       "execute_sblr",
                                                       "sblr.dispatch.pending");
  mark_execute_phase("register_request_lifecycle");
  std::string encoded = decoded->encoded_sblr_envelope;
  bool cursor_requested = decoded->cursor_requested;
  const auto prepared_it = registry->prepared_by_uuid.find(UuidBytesToText(decoded->prepared_statement_uuid));
  const ServerPreparedStatementRecord* prepared_statement =
      prepared_it == registry->prepared_by_uuid.end() ? nullptr : &prepared_it->second;
  if (encoded.empty() && prepared_statement == nullptr) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   "prepared_statement_not_found");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
                   "The prepared SBLR statement is not available for this session.",
                   "prepared_statement_not_found");
  }
  if (prepared_statement != nullptr) {
    const std::string mismatch =
        PreparedStatementAuthorityMismatchReason(*registry,
                                                 *prepared_statement,
                                                 *session);
    const std::string detail = PreparedStatementRefusalDetail(mismatch);
    if (mismatch == "prepared_statement_closed" ||
        mismatch == "prepared_statement_cross_session") {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     detail);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
                     "The prepared SBLR statement is not available for this session.",
                     detail);
    }
    if (!mismatch.empty()) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     detail);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                     "The prepared SBLR statement was invalidated by a server authority epoch change.",
                     detail);
    }
    LinkServerRequestPreparedStatement(registry,
                                       request_record.request_uuid,
                                       decoded->prepared_statement_uuid);
    if (encoded.empty()) {
      encoded = prepared_statement->encoded_sblr_envelope;
    }
  }
  mark_execute_phase("prepared_statement_authority");
  const std::string statement_shape_hash = DispatchStatementShapeHash(encoded);
  const std::string operation_hint = EncodedTextField(encoded, "operation_id");
  const std::string target_object_hint =
      EncodedTextField(encoded, "target_object_uuid");
  const bool implicit_negative_cache_allowed =
      !operation_hint.empty() && !target_object_hint.empty();
  mark_execute_phase("shape_and_hint_extract");
  const auto fail_authority_cache_before_dispatch =
      [&](std::string detail) -> SessionOperationResult {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   detail);
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE",
                   "The server refused a stale or cross-authority cache entry before SBLR dispatch.",
                   std::move(detail));
  };
  const auto validate_cache_reference =
      [&](std::string_view field,
          const std::string& cache_kind,
          bool negative_refusal)
          -> std::optional<SessionOperationResult> {
    const std::string cache_key = EncodedTextField(encoded, std::string(field));
    if (cache_key.empty()) return std::nullopt;
    const auto validation =
        ValidateServerAuthorityCacheEntry(*registry,
                                          *session,
                                          cache_key,
                                          cache_kind,
                                          operation_hint,
                                          target_object_hint,
                                          statement_shape_hash);
    if (!validation.accepted) {
      return fail_authority_cache_before_dispatch(validation.detail.empty()
                                                     ? "authority_cache_stale"
                                                     : validation.detail);
    }
    MarkServerAuthorityCacheHit(registry, cache_key);
    if (!negative_refusal) return std::nullopt;
    const std::string detail =
        validation.record == nullptr || validation.record->diagnostic_detail.empty()
            ? "negative_authorization_cache_hit"
            : validation.record->diagnostic_detail;
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   detail);
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   decoded->session_uuid,
                   validation.record == nullptr ||
                           validation.record->diagnostic_code.empty()
                       ? "PARSER_SERVER_IPC.NEGATIVE_AUTHORIZATION_CACHE_HIT"
                       : validation.record->diagnostic_code,
                   "The cached negative authorization decision refused this statement before SBLR dispatch.",
                   detail);
  };
  if (auto cached_refusal = validate_cache_reference(
          "negative_authorization_cache_key", "negative_authorization", true)) {
    return *cached_refusal;
  }
  if (auto stale_capability = validate_cache_reference(
          "capability_cache_key", "capability_route", false)) {
    return *stale_capability;
  }
  if (auto stale_preflight = validate_cache_reference(
          "statement_preflight_cache_key", "statement_preflight", false)) {
    return *stale_preflight;
  }
  if (implicit_negative_cache_allowed) {
    const std::string implicit_negative_key =
        ServerAuthorityCacheKey("negative_authorization",
                                *session,
                                operation_hint,
                                target_object_hint,
                                statement_shape_hash);
    const auto implicit_negative =
        ValidateServerAuthorityCacheEntry(*registry,
                                          *session,
                                          implicit_negative_key,
                                          "negative_authorization",
                                          operation_hint,
                                          target_object_hint,
                                          statement_shape_hash);
    if (implicit_negative.accepted && implicit_negative.record != nullptr &&
        implicit_negative.record->refusal) {
      MarkServerAuthorityCacheHit(registry, implicit_negative_key);
      const std::string detail =
          implicit_negative.record->diagnostic_detail.empty()
              ? "negative_authorization_cache_hit"
              : implicit_negative.record->diagnostic_detail;
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     detail);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     implicit_negative.record->diagnostic_code.empty()
                         ? "PARSER_SERVER_IPC.NEGATIVE_AUTHORIZATION_CACHE_HIT"
                         : implicit_negative.record->diagnostic_code,
                     "The cached negative authorization decision refused this statement before SBLR dispatch.",
                     detail);
    }
  }
  mark_execute_phase("authority_cache_validation");
  encoded = StripDispatchAuthorityCacheMetadata(encoded);
  mark_execute_phase("strip_authority_cache_metadata");
  ServerSblrAdmissionResult admission;
  const bool prepared_operation_matches =
      prepared_statement != nullptr &&
      !prepared_statement->operation_id.empty() &&
      operation_hint == prepared_statement->operation_id;
  const bool prepared_target_matches =
      prepared_statement == nullptr ||
      prepared_statement->target_object_uuid.empty() ||
      target_object_hint.empty() ||
      target_object_hint == prepared_statement->target_object_uuid;
  const auto prepared_reuse_dml_operation = [](std::string_view operation_id) {
    return operation_id == "dml.insert_rows" ||
           operation_id == "dml.update_rows" ||
           operation_id == "dml.delete_rows" ||
           operation_id == "dml.merge_rows" ||
           operation_id == "dml.execute_import_rows" ||
           operation_id == "dml.execute_native_bulk_ingest";
  };
  const bool prepared_admission_reuse_allowed =
      prepared_operation_matches &&
      prepared_target_matches &&
      prepared_statement != nullptr &&
      prepared_reuse_dml_operation(prepared_statement->operation_id);
  if (prepared_statement != nullptr && !prepared_target_matches) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   "prepared_statement_target_mismatch");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.PREPARED_STATEMENT_TARGET_MISMATCH",
                   "The execute SBLR envelope does not match the prepared statement target.",
                   "prepared_statement_target_mismatch");
  }
  if (prepared_admission_reuse_allowed) {
    admission.admitted = true;
    admission.operation_id = prepared_statement->operation_id;
    admission.operation_family = prepared_statement->operation_family;
    admission.requires_public_abi_dispatch = prepared_statement->requires_public_abi_dispatch;
    admission.row_count_hint =
        TextLineU64(encoded, "estimated_row_count").value_or(prepared_statement->row_count_hint);
    mark_execute_phase("prepared_admission_reuse");
  } else {
    const auto admission_phase_start = ServerSteadyClock::now();
    admission = AdmitServerSblrEnvelope(ServerSblrAdmissionRequest{encoded, false});
    (void)admission_phase_start;
    mark_execute_phase("admit_server_sblr_envelope");
    if (!admission.admitted) {
      const std::string diagnostic_code =
          admission.diagnostics.empty()
              ? "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"
              : admission.diagnostics.front().code;
      const std::string diagnostic_detail =
          admission.diagnostics.empty()
              ? "sblr_admission_rejected"
              : DiagnosticDetailField(admission.diagnostics.front());
      if (implicit_negative_cache_allowed) {
        StoreServerAuthorityCacheDecision(registry,
                                          *session,
                                          "negative_authorization",
                                          operation_hint,
                                          target_object_hint,
                                          statement_shape_hash,
                                          diagnostic_code,
                                          diagnostic_detail,
                                          true);
      }
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     admission.diagnostics.empty()
                                         ? "sblr_admission_rejected"
                                         : admission.diagnostics.front().code);
      return FailureWithDiagnostics(
          static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
          kSchemaExecuteResultTestV1,
          decoded->session_uuid,
          admission.diagnostics,
          admission.diagnostics.empty() ? "sblr_admission_rejected"
                                        : admission.diagnostics.front().code);
    }
    if (prepared_statement != nullptr &&
        (!prepared_statement->operation_id.empty() &&
         prepared_statement->operation_id != admission.operation_id)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "prepared_statement_shape_mismatch");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.PREPARED_STATEMENT_SHAPE_MISMATCH",
                     "The execute SBLR envelope does not match the prepared statement operation.",
                     "prepared_statement_shape_mismatch");
    }
  }
  StoreServerAuthorityCacheDecision(registry,
                                    *session,
                                    "capability_route",
                                    admission.operation_id,
                                    target_object_hint,
                                    statement_shape_hash,
                                    {},
                                    "capability_route_admitted",
                                    false);
  if (IsLanguageSessionOperation(admission.operation_id)) {
    if (admission.operation_id == "language.session.set") {
      const std::string target_language_profile =
          JsonTextField(encoded, "target_language_profile").value_or("");
      if (target_language_profile.empty()) {
        CompleteServerRequestLifecycle(registry,
                                       request_record.request_uuid,
                                       ServerRequestLifecycleState::kFailed,
                                       "target_language_profile_required");
        return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                       kSchemaExecuteResultTestV1,
                       decoded->session_uuid,
                       "PARSER_SERVER_IPC.LANGUAGE_PROFILE_REQUIRED",
                       "SET LANGUAGE requires a server-admitted target language profile.",
                       "target_language_profile_required");
      }
      ApplyRequestedLanguageProfile(session, target_language_profile);
      BumpSessionLanguageResourceEpochs(session);
      return LanguageSessionControlResult(
          registry,
          request_record.request_uuid,
          decoded->session_uuid,
          admission.operation_id,
          LanguageSessionContextPacket(admission.operation_id,
                                       *session,
                                       "language_session_set",
                                       true),
          0,
          "language_session_set");
    }
    if (admission.operation_id == "language.session.reset") {
      ApplyRequestedLanguageProfile(session, session->default_language_tag.empty()
                                                 ? "en"
                                                 : session->default_language_tag);
      BumpSessionLanguageResourceEpochs(session);
      return LanguageSessionControlResult(
          registry,
          request_record.request_uuid,
          decoded->session_uuid,
          admission.operation_id,
          LanguageSessionContextPacket(admission.operation_id,
                                       *session,
                                       "language_session_reset",
                                       true),
          0,
          "language_session_reset");
    }
    return LanguageSessionControlResult(
        registry,
        request_record.request_uuid,
        decoded->session_uuid,
        admission.operation_id,
        LanguageSessionContextPacket(admission.operation_id,
                                     *session,
                                     "language_session_show",
                                     false),
        1,
        "language_session_show");
  }
  if (IsLanguageBundleOperation(admission.operation_id)) {
    if (!LanguageBundleManifestAdmitted(encoded)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_bundle_admission_required");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_ADMISSION_REQUIRED",
                     "Language bundle load unload and validate operations require admitted signed resource manifests.",
                     "language_bundle_admission_required");
    }

    ServerLanguageBundleRecord record =
        LanguageBundleRecordFromEnvelope(encoded, *session);
    if (!LanguageBundleRecordIsComplete(record)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_bundle_manifest_incomplete");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_MANIFEST_INCOMPLETE",
                     "Language bundle manifests require bundle profile language tag and resource hash identity.",
                     "language_bundle_manifest_incomplete");
    }

    if (admission.operation_id == "language.bundle.validate") {
      record.loaded = registry->language_bundles_by_uuid.count(record.bundle_uuid) != 0;
      record.language_resource_epoch = session->language_resource_epoch;
      return LanguageSessionControlResult(
          registry,
          request_record.request_uuid,
          decoded->session_uuid,
          admission.operation_id,
          LanguageBundleRegistryPacket(admission.operation_id,
                                       record,
                                       "language_bundle_validated",
                                       false),
          1,
          "language_bundle_validated");
    }

    if (admission.operation_id == "language.bundle.load") {
      BumpSessionLanguageResourceEpochs(session);
      record.loaded = true;
      record.language_resource_epoch = session->language_resource_epoch;
      registry->language_bundles_by_uuid[record.bundle_uuid] = record;
      return LanguageSessionControlResult(
          registry,
          request_record.request_uuid,
          decoded->session_uuid,
          admission.operation_id,
          LanguageBundleRegistryPacket(admission.operation_id,
                                       record,
                                       "language_bundle_loaded",
                                       true),
          0,
          "language_bundle_loaded");
    }

    const auto bundle_it = registry->language_bundles_by_uuid.find(record.bundle_uuid);
    if (bundle_it == registry->language_bundles_by_uuid.end()) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_bundle_not_loaded");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_NOT_LOADED",
                     "Language bundle unload requires a loaded server registry record.",
                     "language_bundle_not_loaded");
    }

    ServerLanguageBundleRecord loaded_record = bundle_it->second;
    const auto language = ServerLanguageContextForSession(*session);
    if (loaded_record.language_profile_id == language.language_profile_id) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_bundle_active_profile_in_use");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_ACTIVE_PROFILE_IN_USE",
                     "The active language profile cannot be unloaded from the server resource registry.",
                     "language_bundle_active_profile_in_use");
    }
    if (loaded_record.required_profile) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_bundle_required_profile");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_BUNDLE_REQUIRED_PROFILE",
                     "Required language profiles cannot be unloaded from the server resource registry.",
                     "language_bundle_required_profile");
    }

    BumpSessionLanguageResourceEpochs(session);
    loaded_record.loaded = false;
    loaded_record.language_resource_epoch = session->language_resource_epoch;
    registry->language_bundles_by_uuid.erase(bundle_it);
    return LanguageSessionControlResult(
        registry,
        request_record.request_uuid,
        decoded->session_uuid,
        admission.operation_id,
        LanguageBundleRegistryPacket(admission.operation_id,
                                     loaded_record,
                                     "language_bundle_unloaded",
                                     true),
        0,
        "language_bundle_unloaded");
  }
  if (IsLanguageResourceDirectoryOperation(admission.operation_id)) {
    if (admission.operation_id == "language.resource_directory.show") {
      ServerLanguageResourceDirectoryRecord record =
          LanguageResourceDirectoryRecordFromEnvelope(encoded);
      if (!record.directory_id.empty()) {
        const auto it =
            registry->language_resource_directories_by_id.find(record.directory_id);
        if (it != registry->language_resource_directories_by_id.end()) {
          record = it->second;
        }
      } else if (!registry->language_resource_directories_by_id.empty()) {
        record = registry->language_resource_directories_by_id.begin()->second;
      }
      return LanguageSessionControlResult(
          registry,
          request_record.request_uuid,
          decoded->session_uuid,
          admission.operation_id,
          LanguageResourceDirectoryPacket(admission.operation_id,
                                          record,
                                          "language_resource_directory_show",
                                          false),
          record.directory_id.empty() ? 0 : 1,
          "language_resource_directory_show");
    }

    if (!LanguageResourceDirectoryManifestAdmitted(encoded)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_resource_directory_admission_required");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_RESOURCE_DIRECTORY_ADMISSION_REQUIRED",
                     "Language resource directory scan and reload require admitted signed manifests.",
                     "language_resource_directory_admission_required");
    }

    ServerLanguageResourceDirectoryRecord record =
        LanguageResourceDirectoryRecordFromEnvelope(encoded);
    if (!LanguageResourceDirectoryRecordIsComplete(record)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "language_resource_directory_manifest_incomplete");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.LANGUAGE_RESOURCE_DIRECTORY_MANIFEST_INCOMPLETE",
                     "Language resource directory manifests require id hash signing evidence audit reason and security admission.",
                     "language_resource_directory_manifest_incomplete");
    }

    BumpSessionLanguageResourceEpochs(session);
    record.language_resource_epoch = session->language_resource_epoch;
    record.localized_name_epoch = session->localized_name_epoch;
    record.message_resource_epoch = session->message_resource_epoch;
    registry->language_resource_directories_by_id[record.directory_id] = record;
    const std::string outcome =
        admission.operation_id == "language.resource_directory.reload"
            ? "language_resource_directory_reloaded"
            : "language_resource_directory_scanned";
    return LanguageSessionControlResult(
        registry,
        request_record.request_uuid,
        decoded->session_uuid,
        admission.operation_id,
        LanguageResourceDirectoryPacket(admission.operation_id,
                                        record,
                                        outcome,
                                        true),
        0,
        outcome);
  }
  if (admission.operation_id == "session.prepare_statement") {
    const std::string statement_name = JsonTextField(encoded, "prepared_statement_name").value_or("");
    if (statement_name.empty()) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "prepared_statement_name_required");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.PREPARED_STATEMENT_NAME_REQUIRED",
                     "PREPARE requires a session-scoped prepared statement name.",
                     "prepared_statement_name_required");
    }
    if (auto* existing = FindPreparedByName(registry, decoded->session_uuid, statement_name)) {
      existing->closed = true;
    }
    ServerPreparedStatementRecord prepared;
    prepared.prepared_statement_uuid = sbps::MakeUuidV7Bytes();
    prepared.client_statement_uuid = decoded->prepared_statement_uuid;
    CapturePreparedAuthorityContext(&prepared, *session);
    prepared.statement_name = statement_name;
    prepared.encoded_sblr_envelope = PreparedInnerEnvelopeFromControl(encoded);
    const auto inner_admission =
        AdmitServerSblrEnvelope(ServerSblrAdmissionRequest{prepared.encoded_sblr_envelope, false});
    if (!inner_admission.admitted) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "prepared_inner_sblr_rejected");
      return FailureWithDiagnostics(
          static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
          kSchemaExecuteResultTestV1,
          decoded->session_uuid,
          inner_admission.diagnostics,
          "prepared_inner_sblr_rejected");
    }
    prepared.operation_family = inner_admission.operation_family;
    prepared.operation_id = inner_admission.operation_id;
    prepared.requires_public_abi_dispatch = inner_admission.requires_public_abi_dispatch;
    prepared.row_count_hint = inner_admission.row_count_hint;
    prepared.catalog_generation = session->catalog_generation;
    prepared.security_epoch = session->security_epoch;
    prepared.descriptor_epoch = session->descriptor_epoch;
    prepared.grant_epoch = session->grant_epoch;
    prepared.policy_generation = session->policy_generation;
    prepared.role_set_hash = session->role_set_hash;
    prepared.group_set_hash = session->group_set_hash;
    prepared.search_path_hash = session->search_path_hash;
    CapturePreparedLanguageContext(&prepared, *session);
    BindPreparedSessionObjectHandle(registry,
                                    &prepared,
                                    *session,
                                    prepared.encoded_sblr_envelope,
                                    prepared.operation_id);
    SealPreparedAuthorityProof(&prepared, *session);
    StorePreparedExecutionContext(registry, prepared, *session);
    registry->prepared_by_uuid[UuidBytesToText(prepared.prepared_statement_uuid)] = prepared;
    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    LinkServerRequestPreparedStatement(registry,
                                       request_record.request_uuid,
                                       prepared.prepared_statement_uuid);
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "prepare_completed");
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult("accepted",
                                         request_record.request_uuid,
                                         {},
                                         0,
                                         admission.operation_id,
                                         ResultPacket(admission.operation_id, true, 0));
    return result;
  }
  if (admission.operation_id == "session.execute_prepared_statement") {
    const std::string statement_name = JsonTextField(encoded, "prepared_statement_name").value_or("");
    auto* prepared = FindPreparedByName(registry, decoded->session_uuid, statement_name);
    const std::string mismatch =
        prepared == nullptr ? std::string("prepared_statement_not_found")
                            : PreparedStatementAuthorityMismatchReason(*registry,
                                                                       *prepared,
                                                                       *session);
    const std::string detail = PreparedStatementRefusalDetail(mismatch);
    if (prepared == nullptr || !mismatch.empty()) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     detail);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     prepared == nullptr
                         ? "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND"
                         : "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                     "The prepared SBLR statement is not available for this session.",
                     detail);
    }
    encoded = prepared->encoded_sblr_envelope;
    cursor_requested = cursor_requested || JsonBoolField(decoded->encoded_sblr_envelope,
                                                        "prepared_cursor_requested");
    LinkServerRequestPreparedStatement(registry,
                                       request_record.request_uuid,
                                       prepared->prepared_statement_uuid);
    admission.admitted = true;
    admission.operation_id = prepared->operation_id;
    admission.operation_family = prepared->operation_family;
    admission.requires_public_abi_dispatch = prepared->requires_public_abi_dispatch;
    admission.row_count_hint = prepared->row_count_hint;
  }
  if (admission.operation_id == "session.deallocate_prepared_statement") {
    const std::string statement_name = JsonTextField(encoded, "prepared_statement_name").value_or("");
    auto* prepared = FindPreparedByName(registry, decoded->session_uuid, statement_name);
    if (prepared == nullptr) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "prepared_statement_not_found");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
                     "The prepared SBLR statement is not available for this session.",
                     "prepared_statement_not_found");
    }
    prepared->closed = true;
    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    LinkServerRequestPreparedStatement(registry,
                                       request_record.request_uuid,
                                       prepared->prepared_statement_uuid);
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "deallocate_prepared_completed");
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult("accepted",
                                         request_record.request_uuid,
                                         {},
                                         0,
                                         admission.operation_id,
                                         ResultPacket(admission.operation_id, true, 0));
    return result;
  }
  if (admission.operation_id == "session.cursor_open") {
    const std::string cursor_name = JsonTextField(encoded, "cursor_name").value_or("");
    if (cursor_name.empty()) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "cursor_name_required");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.CURSOR_NAME_REQUIRED",
                     "OPEN/DECLARE CURSOR requires a session-scoped cursor name.",
                     "cursor_name_required");
    }
    CloseOpenCursorsByName(registry, decoded->session_uuid, cursor_name);
    const auto cursor_uuid = sbps::MakeUuidV7Bytes();
    const std::uint64_t row_count = JsonU64Field(encoded, "stream_row_count").value_or(1);
    ServerCursorRecord cursor;
    cursor.cursor_uuid = cursor_uuid;
    cursor.request_uuid = request_record.request_uuid;
    cursor.session_uuid = decoded->session_uuid;
    cursor.cursor_name = cursor_name;
    cursor.operation_id = admission.operation_id;
    auto request_it = registry->requests_by_uuid.find(UuidBytesToText(request_record.request_uuid));
    cursor.finality_token_uuid = request_it == registry->requests_by_uuid.end()
                                     ? std::array<std::uint8_t, 16>{}
                                     : request_it->second.finality_token_uuid;
    cursor.total_row_count = row_count;
    cursor.next_row_index = 0;
    cursor.exhausted = row_count == 0;
    registry->cursors_by_uuid[UuidBytesToText(cursor_uuid)] = cursor;
    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    LinkServerRequestCursor(registry, request_record.request_uuid, cursor_uuid, false);
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult("accepted",
                                         request_record.request_uuid,
                                         cursor_uuid,
                                         row_count,
                                         admission.operation_id,
                                         "",
                                         CursorMetadataDetail(
                                             registry->cursors_by_uuid[UuidBytesToText(cursor_uuid)]));
    return result;
  }
  if (admission.operation_id == "session.cursor_fetch") {
    const std::string cursor_name = JsonTextField(encoded, "cursor_name").value_or("");
    auto* cursor = FindCursorByName(registry, decoded->session_uuid, cursor_name);
    if (cursor == nullptr) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "cursor_not_found");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
                     "The requested cursor does not exist.",
                     "cursor_not_found");
    }
    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    LinkServerRequestCursor(registry,
                            request_record.request_uuid,
                            cursor->cursor_uuid,
                            cursor->engine_result != nullptr);
    const auto remaining = cursor->total_row_count > cursor->next_row_index
                               ? cursor->total_row_count - cursor->next_row_index
                               : 0;
    const auto chunk_rows = std::min<std::uint64_t>(1, remaining);
    const auto first_row = cursor->next_row_index;
    const bool end_of_cursor = first_row + chunk_rows >= cursor->total_row_count;
    const std::string packet = cursor->row_packet.empty()
                                   ? ResultPacket(cursor->operation_id,
                                                  true,
                                                  chunk_rows,
                                                  {},
                                                  first_row,
                                                  cursor->total_row_count,
                                                  end_of_cursor)
                                   : cursor->row_packet;
    cursor->next_row_index += chunk_rows;
    cursor->fetch_count++;
    cursor->exhausted = end_of_cursor;
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   end_of_cursor ? "cursor_fetch_completed_end_of_cursor"
                                                 : "cursor_fetch_completed");
    if (end_of_cursor) {
      MarkServerRequestClosedByCursor(registry,
                                      cursor->cursor_uuid,
                                      ServerRequestLifecycleState::kCompleted,
                                      "cursor_exhausted");
    }
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult("accepted",
                                         request_record.request_uuid,
                                         cursor->cursor_uuid,
                                         chunk_rows,
                                         admission.operation_id,
                                         packet,
                                         CursorMetadataDetail(*cursor));
    return result;
  }
  if (admission.operation_id == "session.cursor_close") {
    const std::string cursor_name = JsonTextField(encoded, "cursor_name").value_or("");
    auto* cursor = FindCursorByName(registry, decoded->session_uuid, cursor_name);
    if (cursor == nullptr) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "cursor_not_found");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
                     "The requested cursor does not exist.",
                     "cursor_not_found");
    }
    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    LinkServerRequestCursor(registry,
                            request_record.request_uuid,
                            cursor->cursor_uuid,
                            cursor->engine_result != nullptr);
    MarkCursorFinality(cursor, "closed", "client_closed");
    cursor->closed = true;
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "cursor_close_completed");
    MarkServerRequestClosedByCursor(registry,
                                    cursor->cursor_uuid,
                                    ServerRequestLifecycleState::kCompleted,
                                    "client_closed");
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult("accepted",
                                         request_record.request_uuid,
                                         cursor->cursor_uuid,
                                         0,
                                         admission.operation_id,
                                         ResultPacket(admission.operation_id, true, 0),
                                         CursorMetadataDetail(*cursor));
    return result;
  }
  if (admission.operation_id.starts_with("jobs.scheduler.")) {
    const std::string job_name = JsonTextField(encoded, "job_name").value_or("");
    const std::string job_uuid = JsonTextField(encoded, "job_uuid").value_or("");
    const std::string schedule_uuid =
        JsonTextField(encoded, "schedule_uuid").value_or("");
    const auto fail_job_route = [&](std::string code,
                                    std::string message,
                                    std::string detail) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     code);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     std::move(code),
                     std::move(message),
                     std::move(detail));
    };
    const bool schedule_only =
        admission.operation_id == "jobs.scheduler.alter_schedule";
    if (!schedule_only && (job_name.empty() || job_uuid.empty())) {
      return fail_job_route("PARSER_SERVER_IPC.JOB_NAME_REQUIRED",
                            "Jobs scheduler routes require a structured database-local job name.",
                            "job_name_or_uuid_required");
    }
    if (schedule_only && schedule_uuid.empty()) {
      return fail_job_route("PARSER_SERVER_IPC.SCHEDULE_NAME_REQUIRED",
                            "Schedule routes require a structured database-local schedule name.",
                            "schedule_name_or_uuid_required");
    }
    auto job_context = EnsureJobScheduler(registry, *session);
    if (!job_context.status.ok || job_context.scheduler == nullptr ||
        job_context.quota == nullptr) {
      return fail_job_route(job_context.status.diagnostic_code,
                            "Jobs scheduler authority could not be established.",
                            job_context.status.detail);
    }
    auto& scheduler = *job_context.scheduler;
    auto& quota = *job_context.quota;
    const std::uint64_t now = JobSchedulerNowMicroseconds(scheduler, quota);
    agents::AgentRuntimeStatus status = agents::AgentOk();
    if (admission.operation_id == "jobs.scheduler.create_job") {
      if (scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.DUPLICATE_JOB", job_uuid);
      } else {
        agents::BackgroundJobDefinition definition;
        definition.job_uuid = job_uuid;
        definition.job_type = "sbsql.manual_job";
        definition.database_uuid = session->database_uuid;
        definition.pool_id = "background";
        definition.workload_class = agents::WorkloadClass::background;
        definition.source = agents::WorkloadAdmissionSource::engine;
        definition.resource_request = JobResourceVector();
        definition.enabled = true;
        definition.not_before_microseconds = 0;
        status = scheduler.RegisterJob(std::move(definition));
      }
    } else if (admission.operation_id == "jobs.scheduler.alter_job") {
      if (!scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
      } else {
        agents::BackgroundJobSchedule schedule;
        status = ScheduleFromEnvelope(encoded, now, &schedule);
        if (status.ok) {
          status = scheduler.AttachScheduleToJob(job_uuid,
                                                 std::move(schedule),
                                                 false,
                                                 now);
        }
      }
    } else if (admission.operation_id == "jobs.scheduler.create_schedule") {
      if (!scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
      } else {
        agents::BackgroundJobSchedule schedule;
        status = ScheduleFromEnvelope(encoded, now, &schedule);
        if (status.ok) {
          status = scheduler.AttachScheduleToJob(job_uuid,
                                                 std::move(schedule),
                                                 true,
                                                 now);
        }
      }
    } else if (admission.operation_id == "jobs.scheduler.alter_schedule") {
      agents::BackgroundJobSchedule schedule;
      status = ScheduleFromEnvelope(encoded, now, &schedule);
      if (status.ok) {
        status = scheduler.AlterSchedule(schedule_uuid, std::move(schedule), now);
      }
    } else if (admission.operation_id == "jobs.scheduler.run_job") {
      if (!scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
      } else {
        const auto decision = scheduler.RunNextDue(&quota, now);
        if (!decision.status.ok || !decision.admitted()) {
          status = decision.status;
        } else if (decision.job_uuid != job_uuid) {
          (void)scheduler.CompleteRunningJob(decision.job_uuid,
                                            agents::BackgroundJobRunOutcome::cancelled,
                                            &quota,
                                            now + 1,
                                            "wrong_named_job_for_run");
          status = agents::AgentError("BACKGROUND_JOBS.NAMED_JOB_NOT_DUE", job_uuid);
        }
      }
    } else if (admission.operation_id == "jobs.scheduler.pause_job") {
      if (!scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
      } else {
        status = scheduler.Pause("operator_pause:" + job_name, now);
      }
    } else if (admission.operation_id == "jobs.scheduler.resume_job") {
      if (!scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
      } else {
        status = scheduler.Resume(now);
      }
    } else if (admission.operation_id == "jobs.scheduler.cancel_job") {
      if (!scheduler.FindJob(job_uuid)) {
        status = agents::AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
      } else {
        status = scheduler.CompleteRunningJob(job_uuid,
                                             agents::BackgroundJobRunOutcome::cancelled,
                                             &quota,
                                             now,
                                             "operator_cancelled:" + job_name);
      }
    } else {
      status = agents::AgentError("BACKGROUND_JOBS.OPERATION_UNSUPPORTED",
                                  admission.operation_id);
    }
    if (!status.ok) {
      return fail_job_route(status.diagnostic_code,
                            "The background job scheduler rejected the requested operation.",
                            status.detail);
    }

    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "jobs_scheduler_completed");
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult("accepted",
                                         request_record.request_uuid,
                                         {},
                                         1,
                                         admission.operation_id,
                                         ResultPacket(admission.operation_id, true, 1),
                                         JobSchedulerRouteDetail(scheduler, quota));
    return result;
  }
  if (admission.operation_id == "backup_archive.start_logical_backup" ||
      admission.operation_id == "backup_archive.restore_logical_backup" ||
      admission.operation_id == "backup_archive.package_delta_stream" ||
      admission.operation_id == "backup_archive.apply_delta_stream") {
    const std::string archive_operation =
        JsonTextField(encoded, "archive_operation").value_or(admission.operation_id);
    const std::string target_name =
        JsonTextField(encoded, "target_name").value_or("database");
    std::string manifest_uri = JsonTextField(encoded, "manifest_uri").value_or("");
    if (manifest_uri.empty()) {
      manifest_uri = DefaultArchiveManifestUri(*session, archive_operation, target_name);
    }
    const bool restore_verify_only = JsonBoolField(encoded, "restore_verify_only", false);

    const auto fail_archive_route = [&](std::string code,
                                        std::string message,
                                        std::string detail) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     code);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     std::move(code),
                     std::move(message),
                     std::move(detail));
    };
    if (session->database_path.empty()) {
      return fail_archive_route("PARSER_SERVER_IPC.ARCHIVE_DATABASE_PATH_REQUIRED",
                                "Archive/replication routes require a database path in the bound session.",
                                "database_path_required");
    }

    engine_api::EngineApiResult api_result;
    if (admission.operation_id == "backup_archive.start_logical_backup") {
      engine_api::EngineStartLogicalBackupRequest api_request;
      api_request.context =
          ArchiveReplicationEngineContext(*session, request_record.request_uuid);
      api_request.operation_id = admission.operation_id;
      AddArchiveReplicationCommonOptions(&api_request, manifest_uri);
      api_result = engine_api::EngineStartLogicalBackup(api_request);
    } else if (admission.operation_id == "backup_archive.restore_logical_backup") {
      engine_api::EngineRestoreLogicalBackupRequest api_request;
      api_request.context =
          ArchiveReplicationEngineContext(*session, request_record.request_uuid);
      api_request.operation_id = admission.operation_id;
      AddArchiveReplicationRestoreOptions(&api_request, manifest_uri, restore_verify_only);
      api_result = engine_api::EngineRestoreLogicalBackup(api_request);
    } else if (admission.operation_id == "backup_archive.package_delta_stream") {
      engine_api::EnginePackageDeltaStreamRequest api_request;
      api_request.context =
          ArchiveReplicationEngineContext(*session, request_record.request_uuid);
      api_request.operation_id = admission.operation_id;
      AddArchiveReplicationDeltaOptions(&api_request,
                                        manifest_uri,
                                        archive_operation,
                                        target_name);
      api_result = engine_api::EnginePackageDeltaStream(api_request);
    } else {
      engine_api::EngineApplyDeltaStreamRequest api_request;
      api_request.context =
          ArchiveReplicationEngineContext(*session, request_record.request_uuid);
      api_request.operation_id = admission.operation_id;
      AddArchiveReplicationRestoreOptions(&api_request, manifest_uri);
      api_result = engine_api::EngineApplyDeltaStream(api_request);
    }

    if (!api_result.ok) {
      return fail_archive_route("PARSER_SERVER_IPC.ARCHIVE_REPLICATION_REJECTED",
                                "The engine backup/archive API rejected the requested route.",
                                ArchiveReplicationDiagnosticDetail(api_result));
    }

    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "archive_replication_completed");
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult(
        "accepted",
        request_record.request_uuid,
        {},
        1,
        admission.operation_id,
        ResultPacket(admission.operation_id, true, 1),
        ArchiveReplicationResultDetail(api_result, manifest_uri, archive_operation));
    return result;
  }
  UpdateServerRequestLifecycleOperation(registry,
                                        request_record.request_uuid,
                                        admission.operation_id);
  if (auto transaction_refusal =
          ValidateTransactionAdmission(engine_state, *session, admission, encoded)) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   transaction_refusal->diagnostics.empty()
                                       ? "transaction_admission_denied"
                                       : transaction_refusal->diagnostics.front().code);
    return *transaction_refusal;
  }
  StoreServerAuthorityCacheDecision(registry,
                                    *session,
                                    "statement_preflight",
                                    admission.operation_id,
                                    target_object_hint,
                                    statement_shape_hash,
                                    {},
                                    "statement_preflight_admitted",
                                    false);
  if (admission.operation_id == "routine.execute_cursor_argument") {
    const auto fail_routine_cursor = [&](std::string code,
                                         std::string message,
                                         std::string detail) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     detail.empty() ? code : detail);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     std::move(code),
                     std::move(message),
                     std::move(detail));
    };

    const std::string binding =
        JsonTextField(encoded, "routine_cursor_argument_binding").value_or("");
    if (binding != "descriptor.cursor_handle.session_registry") {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_DESCRIPTOR_MISMATCH",
          "Routine cursor arguments must bind through the server cursor registry descriptor.",
          "routine_cursor_binding_mismatch");
    }
    const std::string descriptor =
        JsonTextField(encoded, "routine_cursor_descriptor").value_or("");
    const std::string expected_descriptor =
        JsonTextField(encoded, "routine_expected_cursor_descriptor").value_or("");
    if (descriptor.empty() || expected_descriptor.empty() ||
        descriptor != expected_descriptor) {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_DESCRIPTOR_MISMATCH",
          "The supplied cursor row descriptor does not match the routine contract.",
          "routine_cursor_descriptor_mismatch");
    }
    if (JsonTextField(encoded, "routine_security_recheck").value_or("") !=
            "passed" ||
        JsonTextField(encoded, "routine_protected_material_policy").value_or("") !=
            "rechecked") {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_SECURITY_RECHECK_REQUIRED",
          "Routine cursor invocation requires security, masking, visibility, and protected-material rechecks.",
          "routine_cursor_security_recheck_required");
    }

    const std::string context_kind =
        JsonTextField(encoded, "routine_context_kind").value_or("procedure");
    const std::string action =
        JsonTextField(encoded, "routine_cursor_action").value_or("fetch");
    const std::string borrow_policy =
        JsonTextField(encoded, "routine_cursor_borrow_policy").value_or(
            "borrowed_read");
    const bool deterministic_context =
        JsonBoolField(encoded, "routine_deterministic_context", false) ||
        JsonTextField(encoded, "routine_function_context").value_or("") ==
            "deterministic_only";
    if (context_kind == "function" && deterministic_context) {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_DETERMINISTIC_CONTEXT_REFUSED",
          "Cursor-observing routine functions are refused in deterministic-only contexts.",
          "routine_cursor_deterministic_context_refused");
    }
    if (action == "close" && borrow_policy != "owned") {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_BORROWED_CLOSE_REFUSED",
          "A borrowed routine cursor argument cannot be closed by the callee.",
          "routine_cursor_borrowed_close_refused");
    }

    const std::string cursor_uuid_text =
        JsonTextField(encoded, "routine_cursor_uuid").value_or("");
    const auto parsed_cursor_uuid = ParseUuidTextForDispatch(cursor_uuid_text);
    if (!parsed_cursor_uuid) {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_HANDLE_INVALID",
          "Routine cursor invocation requires a valid cursor handle UUID.",
          "routine_cursor_uuid_invalid");
    }
    auto cursor_it =
        registry->cursors_by_uuid.find(UuidBytesToText(*parsed_cursor_uuid));
    if (cursor_it == registry->cursors_by_uuid.end() || cursor_it->second.closed ||
        cursor_it->second.session_uuid != decoded->session_uuid) {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
          "The requested cursor does not exist.",
          "cursor_not_found");
    }

    auto& cursor = cursor_it->second;
    UpdateServerRequestLifecycleOperation(registry,
                                          request_record.request_uuid,
                                          admission.operation_id);
    LinkServerRequestCursor(registry,
                            request_record.request_uuid,
                            cursor.cursor_uuid,
                            cursor.engine_result != nullptr);

    const bool trigger_scoped =
        context_kind == "trigger" &&
        JsonTextField(encoded, "routine_cursor_lifetime").value_or("") ==
            "trigger_event";
    std::uint64_t row_count = 0;
    std::string row_packet;
    std::string cleanup_state = "borrowed_cursor_retained";

    if (action == "inspect") {
      row_packet = ResultPacket(admission.operation_id,
                                true,
                                0,
                                "routine_cursor_metadata_inspected");
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kCompleted,
                                     "routine_cursor_inspect_completed");
    } else if (action == "close") {
      MarkCursorFinality(&cursor, "closed", "routine_owned_close");
      cursor.closed = true;
      cleanup_state = "owned_cursor_closed";
      row_packet = ResultPacket(admission.operation_id,
                                true,
                                0,
                                "routine_owned_cursor_closed");
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kCompleted,
                                     "routine_cursor_close_completed");
      MarkServerRequestClosedByCursor(registry,
                                      cursor.cursor_uuid,
                                      ServerRequestLifecycleState::kCompleted,
                                      "routine_owned_close");
    } else if (action == "fetch") {
      std::uint64_t max_rows =
          JsonU64Field(encoded, "routine_cursor_fetch_max_rows").value_or(1);
      if (max_rows == 0) max_rows = 1;
      const std::uint64_t max_bytes =
          JsonU64Field(encoded, "routine_cursor_fetch_max_bytes").value_or(0);
      if (max_rows > cursor.max_chunk_rows) {
        return fail_routine_cursor(
            "SERVER.STREAM.CHUNK_TOO_LARGE",
            "Routine cursor fetch exceeds the cursor chunk limit.",
            "max_rows_exceeds_server_limit");
      }
      if (max_bytes > cursor.max_chunk_bytes) {
        return fail_routine_cursor(
            "SERVER.STREAM.BYTES_TOO_LARGE",
            "Routine cursor fetch byte limit exceeds the cursor protocol limit.",
            "max_bytes_exceeds_server_limit");
      }
      if (cursor.engine_result != nullptr) {
        const auto batch =
            FetchEngineCursorBatch(cursor.engine_result, max_rows, max_bytes);
        if (!batch.ok) {
          return fail_routine_cursor(
              "PARSER_SERVER_IPC.ENGINE_FETCH_FAILED",
              "The engine could not produce the next routine cursor batch.",
              batch.diagnostic_detail);
        }
        row_count = batch.row_count;
        row_packet = batch.row_packet;
        cursor.next_row_index += batch.row_count;
        cursor.fetch_count++;
        cursor.exhausted = batch.end_of_stream;
      } else {
        const auto remaining = cursor.total_row_count > cursor.next_row_index
                                   ? cursor.total_row_count -
                                         cursor.next_row_index
                                   : 0;
        row_count = std::min(max_rows, remaining);
        const auto first_row = cursor.next_row_index;
        const bool end_of_cursor =
            first_row + row_count >= cursor.total_row_count;
        row_packet = ResultPacket(admission.operation_id,
                                  true,
                                  row_count,
                                  "routine_cursor_fetch",
                                  first_row,
                                  cursor.total_row_count,
                                  end_of_cursor);
        if (max_bytes != 0 && row_packet.size() > max_bytes) {
          return fail_routine_cursor(
              "SERVER.STREAM.BYTES_TOO_SMALL",
              "Routine cursor fetch byte limit is too small for the next result packet.",
              "max_bytes_below_next_packet");
        }
        cursor.next_row_index += row_count;
        cursor.fetch_count++;
        cursor.exhausted = end_of_cursor;
      }
      CompleteServerRequestLifecycle(
          registry,
          request_record.request_uuid,
          ServerRequestLifecycleState::kCompleted,
          cursor.exhausted ? "routine_cursor_fetch_completed_end_of_cursor"
                           : "routine_cursor_fetch_completed");
      if (cursor.exhausted) {
        MarkServerRequestClosedByCursor(registry,
                                        cursor.cursor_uuid,
                                        ServerRequestLifecycleState::kCompleted,
                                        "cursor_exhausted");
      }
    } else {
      return fail_routine_cursor(
          "PARSER_SERVER_IPC.ROUTINE_CURSOR_ACTION_UNSUPPORTED",
          "The routine cursor action is not supported by this server.",
          "routine_cursor_action_unsupported");
    }

    if (trigger_scoped && !cursor.closed) {
      MarkCursorFinality(&cursor, "closed", "trigger_event_scope_cleanup");
      cursor.closed = true;
      cleanup_state = "trigger_event_scope_cleanup";
      MarkServerRequestClosedByCursor(registry,
                                      cursor.cursor_uuid,
                                      ServerRequestLifecycleState::kCompleted,
                                      "trigger_event_scope_cleanup");
    }

    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type =
        static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
    result.response_schema_id = kSchemaExecuteResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeExecuteResult(
        "accepted",
        request_record.request_uuid,
        cursor.cursor_uuid,
        row_count,
        admission.operation_id,
        row_packet,
        RoutineCursorInvocationDetail(cursor,
                                      context_kind,
                                      action,
                                      borrow_policy,
                                      cleanup_state));
    return result;
  }
  const auto cursor_uuid = cursor_requested ? sbps::MakeUuidV7Bytes() : std::array<std::uint8_t, 16>{};
  const auto request_uuid = request.header.request_uuid;
  const auto requested_stream_rows = JsonU64Field(encoded, "stream_row_count");
  const auto requested_cursor_max_chunk_rows = JsonU64Field(encoded, "cursor_max_chunk_rows").value_or(
      TextLineU64(encoded, "cursor_max_chunk_rows").value_or(0));
  const auto requested_cursor_max_chunk_bytes = JsonU64Field(encoded, "cursor_max_chunk_bytes").value_or(
      TextLineU64(encoded, "cursor_max_chunk_bytes").value_or(0));
  const std::string bulk_stream_kind = JsonTextField(encoded, "copy_stream_kind").value_or(
      TextLineValue(encoded, "copy_stream_kind").value_or(""));
  const std::uint64_t requested_copy_total_rows = JsonU64Field(encoded, "copy_total_rows").value_or(
      TextLineU64(encoded, "copy_total_rows").value_or(0));
  const std::uint64_t requested_copy_reject_rows = JsonU64Field(encoded, "copy_reject_rows").value_or(
      TextLineU64(encoded, "copy_reject_rows").value_or(0));
  const auto multi_result_count = JsonU64Field(encoded, "multi_result_count");
  const auto warning_chain_count = JsonU64Field(encoded, "warning_chain_count");
  const auto partial_result_rows = JsonU64Field(encoded, "partial_result_rows");
  const auto finality_kind = JsonTextField(encoded, "stream_finality_mode");
  const auto finality_after_fetches = JsonU64Field(encoded, "stream_finality_after_fetches");
  mark_execute_phase("stream_option_extract");
  const bool autocommit_emulation =
      admission.operation_id != "dml.plan_import_rows" &&
      AutocommitEmulationRequested(encoded);
  const bool bulk_stream_cursor = cursor_requested && !bulk_stream_kind.empty();
  const bool multi_result_cursor = cursor_requested && multi_result_count.has_value();
  const bool warning_stream_cursor =
      cursor_requested && warning_chain_count.has_value() && partial_result_rows.has_value();
  const bool synthetic_stream_cursor = cursor_requested && requested_stream_rows.has_value();
  std::uint64_t row_count = warning_stream_cursor
                                ? WarningStreamEventCount(*partial_result_rows, *warning_chain_count)
                                : (multi_result_cursor
                                       ? MultiResultEventCount(*multi_result_count)
                                       : (bulk_stream_cursor
                                              ? BulkStreamEventCount(requested_copy_reject_rows)
                                              : requested_stream_rows.value_or(admission.row_count_hint)));
  std::string row_packet;
  sb_engine_result_t engine_result = nullptr;
  std::vector<std::string> bulk_reject_records;
  std::uint64_t bulk_total_rows = bulk_stream_cursor ? requested_copy_total_rows : 0;
  std::uint64_t bulk_rejected_rows = bulk_stream_cursor ? requested_copy_reject_rows : 0;
  if (admission.requires_public_abi_dispatch) {
    if (admission.operation_id == "transaction.begin" &&
        session->local_transaction_id != 0) {
      row_packet = SessionTransactionStatePayload(admission.operation_id,
                                                  *session,
                                                  "active_adopted");
      row_count = 0;
    } else {
      const auto retain_engine_result = cursor_requested && !synthetic_stream_cursor && !bulk_stream_cursor;
      const bool use_server_live_ipar_projection =
          (admission.operation_id == "observability.show_catalog" ||
           admission.operation_id == "dml.select_rows") &&
          ipar_source_factory != nullptr &&
          ipar_source_factory->build != nullptr &&
          IsServerLiveIparCatalogProjection(encoded);
      ServerIparProjectionSources live_ipar_sources;
      if (use_server_live_ipar_projection) {
        live_ipar_sources = ipar_source_factory->build(ipar_source_factory->context);
      }
      mark_execute_phase("pre_public_abi_dispatch");
      auto public_abi = use_server_live_ipar_projection
                            ? DispatchServerLiveIparCatalogProjection(*session,
                                                                      encoded,
                                                                      live_ipar_sources)
                            : DispatchThroughPublicAbi(registry,
                                                       *session,
                                                       admission.operation_id,
                                                       admission.operation_family,
                                                       encoded,
                                                       decoded->data_packet,
                                                       retain_engine_result);
      mark_execute_phase("public_abi_dispatch");
      if (!public_abi.ok) {
        if (autocommit_emulation) {
          const auto autocommit = FinalizeAutocommitBoundaryForSession(session,
                                                                       engine_state,
                                                                       request_record.request_uuid,
                                                                       false);
          if (!autocommit.ok) {
            CompleteServerRequestLifecycle(registry,
                                           request_record.request_uuid,
                                           ServerRequestLifecycleState::kFailed,
                                           autocommit.diagnostic_detail.empty()
                                               ? "autocommit_rollback_replacement_failed"
                                               : autocommit.diagnostic_detail);
            return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                           kSchemaExecuteResultTestV1,
                           decoded->session_uuid,
                           autocommit.diagnostic_code.empty()
                               ? "PARSER_SERVER_IPC.AUTOCOMMIT_FINALITY_FAILED"
                               : autocommit.diagnostic_code,
                           "The engine refused the command and autocommit rollback/replacement failed.",
                           autocommit.diagnostic_detail.empty()
                               ? "autocommit_rollback_replacement_failed"
                               : autocommit.diagnostic_detail);
          }
          CompleteServerRequestLifecycle(registry,
                                         request_record.request_uuid,
                                         ServerRequestLifecycleState::kFailed,
                                         public_abi.diagnostic_detail.empty()
                                             ? "engine_dispatch_rejected_autocommit_rolled_back"
                                             : public_abi.diagnostic_detail);
          return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                         kSchemaExecuteResultTestV1,
                         decoded->session_uuid,
                         public_abi.diagnostic_code.empty()
                             ? "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED"
                             : public_abi.diagnostic_code,
                         "The engine rejected the admitted SBLR envelope; autocommit rollback opened a replacement transaction.",
                         (public_abi.diagnostic_detail.empty()
                              ? "engine_dispatch_rejected"
                              : public_abi.diagnostic_detail) +
                             std::string(";") + autocommit.evidence);
        }
        CompleteServerRequestLifecycle(registry,
                                       request_record.request_uuid,
                                       ServerRequestLifecycleState::kFailed,
                                       public_abi.diagnostic_detail.empty()
                                           ? "engine_dispatch_rejected"
                                           : public_abi.diagnostic_detail);
        return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                       kSchemaExecuteResultTestV1,
                       decoded->session_uuid,
                       public_abi.diagnostic_code.empty()
                           ? "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED"
                           : public_abi.diagnostic_code,
                       "The engine rejected the admitted SBLR envelope.",
                       public_abi.diagnostic_detail.empty() ? "engine_dispatch_rejected"
                                                            : public_abi.diagnostic_detail);
      }
      row_packet = public_abi.payload;
      ClearStablePublicRelationNameCacheForMutation(registry, admission.operation_id);
      SeedStablePublicRelationNameCacheAfterDdl(registry,
                                                *session,
                                                admission.operation_id,
                                                encoded,
                                                row_packet);
      if (public_abi.affected_rows != 0) {
        if (!row_packet.empty() && row_packet.back() != '\n') {
          row_packet.push_back('\n');
        }
        row_packet += "server_affected_rows=" + std::to_string(public_abi.affected_rows) + "\n";
      }
      mark_execute_phase("public_abi_result_adopt");
      engine_result = public_abi.result_handle;
      if (bulk_stream_cursor) {
        bulk_reject_records = ImportRejectRecordsFromPayload(row_packet);
        bulk_rejected_rows = EvidenceU64(row_packet, "import_rejected_rows").value_or(
            bulk_rejected_rows == 0 ? static_cast<std::uint64_t>(bulk_reject_records.size()) : bulk_rejected_rows);
        const std::uint64_t inserted_rows = EvidenceU64(row_packet, "import_inserted_rows").value_or(0);
        bulk_total_rows = EvidenceU64(row_packet, "import_canonical_rows").value_or(
            bulk_total_rows == 0 ? inserted_rows + bulk_rejected_rows : bulk_total_rows);
        row_count = BulkStreamEventCount(bulk_rejected_rows);
      } else {
        row_count = synthetic_stream_cursor ? *requested_stream_rows : public_abi.row_count;
      }
      if (admission.operation_id == "transaction.commit" ||
          admission.operation_id == "transaction.rollback") {
        std::string replacement_code;
        std::string replacement_detail;
        if (!BeginReplacementTransactionForSession(session,
                                                   engine_state,
                                                   request_record.request_uuid,
                                                   &replacement_code,
                                                   &replacement_detail)) {
          CompleteServerRequestLifecycle(registry,
                                         request_record.request_uuid,
                                         ServerRequestLifecycleState::kFailed,
                                         replacement_detail.empty()
                                             ? "replacement_transaction_begin_failed"
                                             : replacement_detail);
          return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                         kSchemaExecuteResultTestV1,
                         decoded->session_uuid,
                         replacement_code.empty()
                             ? "PARSER_SERVER_IPC.TRANSACTION_REPLACEMENT_FAILED"
                             : replacement_code,
                         "The engine committed finality but could not open the required replacement transaction.",
                         replacement_detail.empty() ? "replacement_transaction_begin_failed"
                                                    : replacement_detail);
        }
        AppendReplacementTransactionState(&row_packet, *session);
        AppendTransactionPressureRestartEvidence(&row_packet, encoded, *session);
        mark_execute_phase("commit_replacement_boundary");
      } else if (autocommit_emulation && !cursor_requested) {
        const auto autocommit = FinalizeAutocommitBoundaryForSession(session,
                                                                     engine_state,
                                                                     request_record.request_uuid,
                                                                     true);
        if (!autocommit.ok) {
          CompleteServerRequestLifecycle(registry,
                                         request_record.request_uuid,
                                         ServerRequestLifecycleState::kFailed,
                                         autocommit.diagnostic_detail.empty()
                                             ? "autocommit_commit_replacement_failed"
                                             : autocommit.diagnostic_detail);
          return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                         kSchemaExecuteResultTestV1,
                         decoded->session_uuid,
                         autocommit.diagnostic_code.empty()
                             ? "PARSER_SERVER_IPC.AUTOCOMMIT_FINALITY_FAILED"
                             : autocommit.diagnostic_code,
                         "Autocommit command finality could not open the required replacement transaction.",
                         autocommit.diagnostic_detail.empty()
                             ? "autocommit_commit_replacement_failed"
                             : autocommit.diagnostic_detail);
        }
        if (!row_packet.empty() && row_packet.back() != '\n') row_packet.push_back('\n');
        row_packet += autocommit.evidence;
        mark_execute_phase("autocommit_finality_boundary");
      }
    }
    ApplyTransactionResultToSession(admission.operation_id, row_packet, session);
    mark_execute_phase("apply_transaction_result");
  } else {
    row_packet = ResultPacket(admission.operation_id, true, row_count);
    mark_execute_phase("synthetic_result_packet");
  }
  if (cursor_requested) {
    ServerCursorRecord cursor;
    cursor.cursor_uuid = cursor_uuid;
    cursor.request_uuid = request_record.request_uuid;
    cursor.session_uuid = decoded->session_uuid;
    auto request_it = registry->requests_by_uuid.find(UuidBytesToText(request_record.request_uuid));
    cursor.finality_token_uuid = request_it == registry->requests_by_uuid.end()
                                     ? std::array<std::uint8_t, 16>{}
                                     : request_it->second.finality_token_uuid;
    cursor.prepared_statement_uuid = decoded->prepared_statement_uuid;
    cursor.operation_id = admission.operation_id;
    cursor.row_packet = admission.requires_public_abi_dispatch && !synthetic_stream_cursor ? row_packet : "";
    cursor.engine_result = synthetic_stream_cursor || bulk_stream_cursor ? nullptr : engine_result;
    cursor.bulk_stream_kind = bulk_stream_cursor ? bulk_stream_kind : "";
    cursor.bulk_reject_records = std::move(bulk_reject_records);
    cursor.multi_result_kind = multi_result_cursor ? "multi_result_sequence" : "";
    cursor.warning_stream_kind = warning_stream_cursor ? "partial_result_warning_chain" : "";
    cursor.finality_kind = finality_kind.value_or("");
    cursor.finality_after_fetches = finality_after_fetches.value_or(0);
    cursor.bulk_total_rows = bulk_stream_cursor ? bulk_total_rows : 0;
    cursor.bulk_rejected_rows = bulk_stream_cursor ? bulk_rejected_rows : 0;
    cursor.multi_result_count = multi_result_cursor ? *multi_result_count : 0;
    cursor.warning_count = warning_stream_cursor ? *warning_chain_count : 0;
    cursor.partial_result_rows = warning_stream_cursor ? *partial_result_rows : 0;
    cursor.total_row_count = row_count;
    cursor.next_row_index = 0;
    if (requested_cursor_max_chunk_rows != 0) {
      cursor.max_chunk_rows = std::min<std::uint64_t>(requested_cursor_max_chunk_rows, 4096);
    }
    if (requested_cursor_max_chunk_bytes != 0) {
      cursor.max_chunk_bytes = std::min<std::uint64_t>(requested_cursor_max_chunk_bytes, 4u * 1024u * 1024u);
    }
    cursor.exhausted = row_count == 0;
    registry->cursors_by_uuid[UuidBytesToText(cursor_uuid)] = cursor;
    LinkServerRequestCursor(registry,
                            request_record.request_uuid,
                            cursor_uuid,
                            engine_result != nullptr);
    mark_execute_phase("cursor_register");
  } else {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "execute_completed");
    mark_execute_phase("complete_request_lifecycle");
  }
  SessionOperationResult result;
  result.accepted = true;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult);
  result.response_schema_id = kSchemaExecuteResultTestV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = decoded->session_uuid;
  result.payload = EncodeExecuteResult("accepted",
                                       request_uuid,
                                       cursor_uuid,
                                       row_count,
                                       admission.operation_id,
                                       cursor_requested ? "" : row_packet);
  mark_execute_phase("encode_execute_result");
  WriteServerPhaseTrace("SCRATCHBIRD_SERVER_EXECUTE_PHASE_TRACE_FILE",
                        "handle_execute_sblr_total",
                        admission.operation_id.empty() ? operation_hint : admission.operation_id,
                        encoded.size(),
                        execute_phase_micros);
  return result;
}

SessionOperationResult HandleFetch(ServerSessionRegistry* registry,
                                   const sbps::Frame& request) {
  auto decoded = DecodeCursorPayload(request.payload);
  if (!decoded) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   request.header.session_uuid,
                   "PARSER_SERVER_IPC.FETCH_INVALID",
                   "The fetch payload is invalid.",
                   "fetch_invalid");
  }
  auto it = registry->cursors_by_uuid.find(UuidBytesToText(decoded->cursor_uuid));
  if (it == registry->cursors_by_uuid.end() || it->second.closed) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
                   "The requested cursor does not exist.",
                   "cursor_not_found");
  }
  auto& cursor = it->second;
  if (cursor.session_uuid != decoded->session_uuid) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
                   "The requested cursor does not exist.",
                   "cursor_not_found");
  }
  auto session = FindSession(*registry, decoded->session_uuid);
  if (!session) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "PARSER_SERVER_IPC.SESSION_REQUIRED",
                   "Fetch requires a bound session.",
                   "session_required");
  }
  auto request_record = RegisterServerRequestLifecycle(registry,
                                                       request,
                                                       *session,
                                                       "fetch_cursor",
                                                       cursor.operation_id.empty()
                                                           ? "cursor.fetch"
                                                           : cursor.operation_id);
  LinkServerRequestCursor(registry,
                          request_record.request_uuid,
                          decoded->cursor_uuid,
                          cursor.engine_result != nullptr);
  if (cursor.finality_kind == "timeout" && cursor.fetch_count >= cursor.finality_after_fetches) {
    MarkCursorFinality(&cursor, "timed_out", "stream_timeout");
    cursor.closed = true;
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kTimedOut,
                                   "stream_timeout");
    MarkServerRequestTimedOutByCursor(registry, decoded->cursor_uuid, "stream_timeout");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "SERVER.STREAM.TIMEOUT",
                   "The active stream timed out before the next fetch could be delivered.",
                   "stream_timeout");
  }
  if (registry->channel_state == ServerChannelState::kDraining ||
      (cursor.finality_kind == "drain" && cursor.fetch_count >= cursor.finality_after_fetches)) {
    MarkCursorFinality(&cursor, "drained", "server_draining");
    const auto packet = StreamFinalityPacket(cursor, "drained", "server_draining");
    cursor.next_row_index = cursor.total_row_count;
    cursor.fetch_count++;
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kDrained,
                                   "server_draining");
    MarkServerRequestClosedByCursor(registry,
                                    decoded->cursor_uuid,
                                    ServerRequestLifecycleState::kDrained,
                                    "server_draining");
    return AcceptedFetchFinality(decoded->session_uuid, decoded->cursor_uuid, packet, cursor);
  }
  if (decoded->fetch_flags != 0) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   "forward_only_cursor");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "SERVER.CURSOR.SCROLL_UNSUPPORTED",
                   "The cursor supports forward-only fetch in this protocol slice.",
                   "forward_only_cursor");
  }
  if (decoded->max_rows > cursor.max_chunk_rows) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   "max_rows_exceeds_server_limit");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "SERVER.STREAM.CHUNK_TOO_LARGE",
                   "Stream chunk exceeds protocol limit.",
                   "max_rows_exceeds_server_limit");
  }
  if (decoded->max_bytes > cursor.max_chunk_bytes) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   "max_bytes_exceeds_server_limit");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "SERVER.STREAM.BYTES_TOO_LARGE",
                   "Stream byte limit exceeds protocol limit.",
                   "max_bytes_exceeds_server_limit");
  }
  if (cursor.engine_result != nullptr) {
    const auto batch = FetchEngineCursorBatch(cursor.engine_result, decoded->max_rows, decoded->max_bytes);
    if (!batch.ok) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     batch.diagnostic_detail);
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                     kSchemaFetchResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.ENGINE_FETCH_FAILED",
                     "The engine could not produce the next result batch.",
                     batch.diagnostic_detail);
    }
    cursor.next_row_index += batch.row_count;
    cursor.fetch_count++;
    cursor.exhausted = batch.end_of_stream;
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   batch.end_of_stream ? "fetch_completed_end_of_cursor"
                                                       : "fetch_completed");
    if (batch.end_of_stream) {
      MarkServerRequestClosedByCursor(registry,
                                      decoded->cursor_uuid,
                                      ServerRequestLifecycleState::kCompleted,
                                      "cursor_exhausted");
    }
    SessionOperationResult result;
    result.accepted = true;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetchResult);
    result.response_schema_id = kSchemaFetchResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
    result.session_uuid = decoded->session_uuid;
    result.payload = EncodeFetchResult(decoded->cursor_uuid,
                                       batch.row_count,
                                       batch.row_packet,
                                       batch.end_of_stream,
                                       CursorMetadataDetail(cursor));
    return result;
  }
  const auto remaining =
      cursor.total_row_count > cursor.next_row_index ? cursor.total_row_count - cursor.next_row_index : 0;
  const auto chunk_rows = std::min(decoded->max_rows, remaining);
  const auto first_row = cursor.next_row_index;
  const auto end_of_cursor = first_row + chunk_rows >= cursor.total_row_count;
  const auto packet = !cursor.warning_stream_kind.empty()
                          ? WarningStreamPacket(cursor, first_row, chunk_rows, end_of_cursor)
                          : (!cursor.multi_result_kind.empty()
                                 ? MultiResultPacket(cursor, first_row, chunk_rows, end_of_cursor)
                                 : (!cursor.bulk_stream_kind.empty()
                                        ? BulkStreamPacket(cursor, first_row, chunk_rows, end_of_cursor)
                                        : (cursor.row_packet.empty()
                                               ? ResultPacket(cursor.operation_id,
                                                              true,
                                                              chunk_rows,
                                                              {},
                                                              first_row,
                                                              cursor.total_row_count,
                                                              end_of_cursor)
                                               : cursor.row_packet)));
  if (decoded->max_bytes != 0 && packet.size() > decoded->max_bytes) {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kFailed,
                                   "max_bytes_below_next_packet");
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kFetchResult),
                   kSchemaFetchResultTestV1,
                   decoded->session_uuid,
                   "SERVER.STREAM.BYTES_TOO_SMALL",
                   "Stream byte limit is too small for the next result packet.",
                   "max_bytes_below_next_packet");
  }
  cursor.next_row_index += chunk_rows;
  cursor.fetch_count++;
  cursor.exhausted = end_of_cursor;
  CompleteServerRequestLifecycle(registry,
                                 request_record.request_uuid,
                                 ServerRequestLifecycleState::kCompleted,
                                 end_of_cursor ? "fetch_completed_end_of_cursor"
                                               : "fetch_completed");
  if (end_of_cursor) {
    MarkServerRequestClosedByCursor(registry,
                                    decoded->cursor_uuid,
                                    ServerRequestLifecycleState::kCompleted,
                                    "cursor_exhausted");
  }
  SessionOperationResult result;
  result.accepted = true;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetchResult);
  result.response_schema_id = kSchemaFetchResultTestV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = decoded->session_uuid;
  result.payload = EncodeFetchResult(decoded->cursor_uuid,
                                     chunk_rows,
                                     packet,
                                     cursor.exhausted,
                                     CursorMetadataDetail(cursor));
  return result;
}

SessionOperationResult HandleCloseCursor(ServerSessionRegistry* registry,
                                         const sbps::Frame& request) {
  auto decoded = DecodeCursorPayload(request.payload);
  if (!decoded) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kCloseCursorResult),
                   kSchemaCloseCursorResultTestV1,
                   request.header.session_uuid,
                   "PARSER_SERVER_IPC.CLOSE_CURSOR_INVALID",
                   "The close cursor payload is invalid.",
                   "close_cursor_invalid");
  }
  auto it = registry->cursors_by_uuid.find(UuidBytesToText(decoded->cursor_uuid));
  std::string detail;
  if (it != registry->cursors_by_uuid.end()) {
    if (it->second.session_uuid != decoded->session_uuid) {
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kCloseCursorResult),
                     kSchemaCloseCursorResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
                     "The requested cursor does not exist.",
                     "cursor_not_found");
    }
    auto session = FindSession(*registry, decoded->session_uuid);
    if (!session) {
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kCloseCursorResult),
                     kSchemaCloseCursorResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.SESSION_REQUIRED",
                     "Close cursor requires a bound session.",
                     "session_required");
    }
    const bool cancelled = (decoded->fetch_flags & kCursorCloseFlagCancel) != 0;
    auto request_record = RegisterServerRequestLifecycle(registry,
                                                         request,
                                                         *session,
                                                         cancelled ? "cancel_cursor"
                                                                   : "close_cursor",
                                                         it->second.operation_id.empty()
                                                             ? "cursor.close"
                                                             : it->second.operation_id);
    LinkServerRequestCursor(registry,
                            request_record.request_uuid,
                            decoded->cursor_uuid,
                            it->second.engine_result != nullptr);
    if (cancelled) {
      const auto cancel = CancelServerRequestLifecycle(registry,
                                                       UuidBytesToText(decoded->cursor_uuid),
                                                       *session,
                                                       false,
                                                       5000);
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     cancel.unknown_outcome
                                         ? ServerRequestLifecycleState::kUnknownOutcome
                                         : ServerRequestLifecycleState::kCancelled,
                                     cancel.unknown_outcome
                                         ? "client_cancelled_outcome_unknown_preserved"
                                         : "client_cancelled");
      auto cursor_after_cancel = registry->cursors_by_uuid.find(UuidBytesToText(decoded->cursor_uuid));
      if (cursor_after_cancel != registry->cursors_by_uuid.end()) {
        detail = CursorMetadataDetail(cursor_after_cancel->second);
      }
    } else {
      MarkCursorFinality(&it->second, "closed", "client_closed");
      it->second.closed = true;
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kCompleted,
                                     "close_cursor_completed");
      MarkServerRequestClosedByCursor(registry,
                                      decoded->cursor_uuid,
                                      ServerRequestLifecycleState::kCompleted,
                                      "client_closed");
      detail = CursorMetadataDetail(it->second);
    }
  }
  SessionOperationResult result;
  result.accepted = true;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kCloseCursorResult);
  result.response_schema_id = kSchemaCloseCursorResultTestV1;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.session_uuid = decoded->session_uuid;
  result.payload = EncodeCloseCursorResult("accepted", decoded->cursor_uuid, detail);
  return result;
}

}  // namespace scratchbird::server
