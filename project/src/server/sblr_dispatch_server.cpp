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
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"
#include "../engine/sblr/sblr_opcode_registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

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
                                         std::string_view manifest_uri) {
  if (request == nullptr) return;
  request->option_envelopes.push_back("filespace_uuid:filespace:session");
  request->option_envelopes.push_back("authoritative_wal:false");
  request->option_envelopes.push_back("restore_inspection_open:true");
  request->option_envelopes.push_back("recovery_classification:restore_inspection");
  request->option_envelopes.push_back("target_database_open:false");
  request->option_envelopes.push_back("engine_owned_path:true");
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

bool PreparedStatementEpochMatches(const ServerPreparedStatementRecord& prepared,
                                   const ServerSessionRecord& session) {
  const auto language = ServerLanguageContextForSession(session);
  return prepared.catalog_generation == session.catalog_generation &&
         prepared.security_epoch == session.security_epoch &&
         prepared.descriptor_epoch == session.descriptor_epoch &&
         prepared.grant_epoch == session.grant_epoch &&
         prepared.policy_generation == session.policy_generation &&
         prepared.role_set_hash == session.role_set_hash &&
         prepared.group_set_hash == session.group_set_hash &&
         prepared.search_path_hash == session.search_path_hash &&
         prepared.language_profile == language.language_profile_id &&
         prepared.language_tag == language.language_tag &&
         prepared.default_language_tag == language.default_language_tag &&
         prepared.input_syntax_profile == language.input_syntax_profile &&
         prepared.input_language_fallback_tag ==
             language.input_language_fallback_tag &&
         prepared.common_resource_hash == language.common_resource_hash &&
         prepared.language_resource_epoch == language.language_resource_epoch &&
         prepared.localized_name_epoch == language.localized_name_epoch &&
         prepared.message_resource_epoch == language.message_resource_epoch &&
         prepared.resource_compatibility_identity ==
             language.resource_compatibility_identity &&
         prepared.resource_version_identity == language.resource_version_identity;
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

void AppendProjectionExpressionOperands(std::string_view encoded,
                                        const std::string& prefix,
                                        std::string* operation_envelope,
                                        std::uint32_t depth = 0) {
  if (depth > 4) return;
  constexpr std::string_view kFields[] = {
      "expr_kind", "expr_opcode", "type", "value", "is_null",
      "function_id", "function_arg_count",
      "operator_id", "canonical_operator_id", "operator_arg_count"};
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

  const bool explicit_column_list =
      JsonBoolField(encoded, "insert_values_column_list_present", false) ||
      TextBoolField(encoded, "insert_values_column_list_present", false);
  std::vector<std::string> descriptor_columns;
  if (!explicit_column_list) {
    const std::string target_uuid = EncodedTextField(encoded, "target_object_uuid");
    descriptor_columns = VisibleInsertColumnNamesForTarget(session, target_uuid);
  }

  for (std::uint64_t row = 0; row < row_count; ++row) {
    const std::string row_uuid = engine_api::GenerateCrudEngineUuid("row");
    for (std::uint64_t column = 0; column < column_count; ++column) {
      const std::string prefix =
          "insert_values_" + std::to_string(row) + "_" + std::to_string(column) + "_";
      std::string name = EncodedTextField(encoded, prefix + "name");
      if (!explicit_column_list &&
          (name.empty() || LooksLikeOrdinalInsertFieldName(name)) &&
          column < descriptor_columns.size() &&
          !descriptor_columns[static_cast<std::size_t>(column)].empty()) {
        name = descriptor_columns[static_cast<std::size_t>(column)];
      }
      if (name.empty()) name = "c" + std::to_string(column);
      std::string type = EncodedTextField(encoded, prefix + "type");
      if (type.empty()) type = "text";
      const std::string value = EncodedTextField(encoded, prefix + "value");
      const bool is_null =
          JsonBoolField(encoded, prefix + "is_null", false) ||
          TextBoolField(encoded, prefix + "is_null", false);
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
  if (operation_id == "nosql.graph_query") return "SBLR_NOSQL_GRAPH_QUERY";
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
  if (operation_id == "catalog.get_descriptor") return "SBLR_CATALOG_GET_DESCRIPTOR";
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
  if (operation_id == "security.principal.create") return "SBLR_SECURITY_PRINCIPAL_CREATE";
  if (operation_id == "security.principal.alter") return "SBLR_SECURITY_PRINCIPAL_ALTER";
  if (operation_id == "security.privilege.grant") return "SBLR_SECURITY_PRIVILEGE_GRANT";
  if (operation_id == "security.privilege.revoke") return "SBLR_SECURITY_PRIVILEGE_REVOKE";
  if (operation_id == "security.session.set_role") return "SBLR_SECURITY_SESSION_SET_ROLE";
  if (operation_id == "security.policy.create") return "SBLR_SECURITY_POLICY_CREATE";
  if (operation_id == "security.policy.alter") return "SBLR_SECURITY_POLICY_ALTER";
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

bool TargetsServerVirtualSysVersion(std::string_view encoded) {
  const std::string target_uuid = JsonTextField(encoded, "target_object_uuid").value_or(
      TextLineValue(encoded, "target_object_uuid").value_or(""));
  return EqualsAsciiInsensitive(target_uuid, "b4a0fd27-e19b-7719-9105-5882443ee2bc");
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
  if (family == "sblr.security.mutation.v3" ||
      family == "sblr.security.mutation_or_inspect.v3") {
    return SblrOperationFamily::security_mutation;
  }
  if (family == "sblr.policy.operation.v3") return SblrOperationFamily::security_mutation;
  if (family == "sblr.management.control.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.management.report.v3") return SblrOperationFamily::management_inspect;
  if (family == "sblr.metrics.read.v3") return SblrOperationFamily::metrics_inspect;
  if (family == "sblr.mga.control.v3") return SblrOperationFamily::management_control;
  if (family == "sblr.mga.report.v3") return SblrOperationFamily::management_inspect;
  if (family == "sblr.filespace.management.v3") return SblrOperationFamily::management_control;
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
         (operation_id != "transaction.begin" &&
          operation_id != "transaction.set_characteristics" &&
          operation_id.starts_with("transaction.")) ||
         operation_id.starts_with("extensibility.register_udr_package") ||
         operation_id.starts_with("extensibility.load_udr_package") ||
         operation_id.starts_with("extensibility.unload_udr_package") ||
         operation_id.starts_with("extensibility.inspect_udr_packages") ||
         operation_id.starts_with("extensibility.invoke_udr_package") ||
         operation_id == "security.privilege.grant" ||
         operation_id == "security.privilege.revoke" ||
         operation_id == "security.principal.create" ||
         operation_id == "security.principal.alter" ||
         operation_id == "security.policy.create" ||
         operation_id == "security.policy.alter" ||
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
  if (statement_succeeded) {
    engine_api::EngineCommitTransactionRequest commit;
    commit.context = ActiveTransactionContext(*session, *database, request_uuid);
    const auto committed = engine_api::EngineCommitTransaction(commit);
    if (!committed.ok) {
      result.diagnostic_code = committed.diagnostics.empty() || committed.diagnostics.front().code.empty()
                                   ? "PARSER_SERVER_IPC.AUTOCOMMIT_FINALITY_FAILED"
                                   : committed.diagnostics.front().code;
      result.diagnostic_detail = committed.diagnostics.empty() || committed.diagnostics.front().detail.empty()
                                     ? "autocommit_commit_failed"
                                     : committed.diagnostics.front().detail;
      return result;
    }
    result.evidence += "evidence=autocommit_statement_succeeded:committed\n";
  } else {
    engine_api::EngineRollbackTransactionRequest rollback;
    rollback.context = ActiveTransactionContext(*session, *database, request_uuid);
    const auto rolled_back = engine_api::EngineRollbackTransaction(rollback);
    if (!rolled_back.ok) {
      result.diagnostic_code = rolled_back.diagnostics.empty() || rolled_back.diagnostics.front().code.empty()
                                   ? "PARSER_SERVER_IPC.AUTOCOMMIT_FINALITY_FAILED"
                                   : rolled_back.diagnostics.front().code;
      result.diagnostic_detail = rolled_back.diagnostics.empty() || rolled_back.diagnostics.front().detail.empty()
                                     ? "autocommit_rollback_failed"
                                     : rolled_back.diagnostics.front().detail;
      return result;
    }
    result.evidence += "evidence=autocommit_statement_failed:rolled_back\n";
  }

  std::string replacement_code;
  std::string replacement_detail;
  if (!BeginReplacementTransactionForSession(session,
                                             engine_state,
                                             request_uuid,
                                             &replacement_code,
                                             &replacement_detail)) {
    result.diagnostic_code = replacement_code.empty()
                                 ? "PARSER_SERVER_IPC.TRANSACTION_REPLACEMENT_FAILED"
                                 : replacement_code;
    result.diagnostic_detail = replacement_detail.empty()
                                   ? "replacement_transaction_begin_failed"
                                   : replacement_detail;
    return result;
  }

  result.ok = true;
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
  if (LooksLikeBinarySblrEnvelope(encoded)) return std::string(encoded);
  std::string_view dispatch_operation_id = operation_id;
  std::string_view dispatch_operation_family = operation_family;
  if (operation_id == "dml.select_rows" && TargetsServerVirtualSysVersion(encoded)) {
    dispatch_operation_id = "observability.show_version";
    dispatch_operation_family = "sblr.management.report.v3";
  }
  const char* opcode = PublicAbiOpcodeForOperation(dispatch_operation_id);
  if (opcode == nullptr) return std::string(encoded);

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
  if (const auto savepoint_name = SavepointOperandForDispatch(encoded, dispatch_operation_id)) {
    operation_envelope += "savepoint_name=";
    operation_envelope += EscapeOperationOperandField(*savepoint_name);
    operation_envelope += "\n";
    operation_envelope += "operand=text\tsavepoint_name\t";
    operation_envelope += EscapeOperationOperandField(*savepoint_name);
    operation_envelope += "\n";
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
  if (dispatch_operation_id == "security.principal.create" ||
      dispatch_operation_id == "security.principal.alter") {
    constexpr std::string_view kSecurityPrincipalFields[] = {
        "principal_uuid", "principal_name", "principal_kind", "lifecycle_state",
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
      dispatch_operation_id == "security.policy.activate" ||
      dispatch_operation_id == "security.policy.deactivate" ||
      dispatch_operation_id == "security.policy.validate" ||
      dispatch_operation_id == "security.policy.show") {
    constexpr std::string_view kSecurityPolicyFields[] = {
        "policy_uuid", "target_object_uuid", "target_object_kind",
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
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
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
  if (dispatch_operation_id == "ddl.create_table") {
    constexpr std::string_view kTableFields[] = {
        "target_object_uuid", "target_object_kind",
        "table_object_uuid", "table_name",
        "target_schema_uuid", "schema_uuid",
        "column_count", "column_definition_count",
        "canonical_type_name",
        "column_0_name", "column_0_type", "column_0_descriptor",
        "column_0_nullable"};
    for (const auto field : kTableFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          JsonPrimitiveField(encoded, field).value_or(
              TextLineValue(encoded, field).value_or("")));
      if (value.empty()) continue;
      AppendOperationOperand(&operation_envelope, field, value);
    }
    const std::string table_uuid = JsonTextField(encoded, "table_object_uuid").value_or(
        TextLineValue(encoded, "table_object_uuid").value_or(""));
    if (!table_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      AppendOperationOperand(&operation_envelope, "target_object_uuid", table_uuid);
      AppendOperationOperand(&operation_envelope, "target_object_kind", "table");
    }
    const std::string table_name = JsonTextField(encoded, "table_name").value_or(
        TextLineValue(encoded, "table_name").value_or(""));
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
        "target_schema_uuid", "schema_uuid"};
    for (const auto field : kAlterObjectFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
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
        "sequence_object_uuid", "sequence_name",
        "target_schema_uuid", "schema_uuid"};
    for (const auto field : kSequenceFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
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
        "base_descriptor_uuid", "base_descriptor_kind",
        "base_canonical_type_name", "base_encoded_descriptor"};
    for (const auto field : kDomainFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
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
    if (!domain_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(domain_name);
      operation_envelope += "\n";
    }
  }
  if (dispatch_operation_id == "ddl.create_view") {
    constexpr std::string_view kViewFields[] = {
        "target_object_uuid", "target_object_kind",
        "view_object_uuid", "view_name",
        "target_schema_uuid", "schema_uuid",
        "view_projection_count", "view_query_shape"};
    for (const auto field : kViewFields) {
      const auto value = JsonTextField(encoded, field).value_or(
          TextLineValue(encoded, field).value_or(""));
      if (value.empty()) continue;
      operation_envelope += "operand=text\t";
      operation_envelope += field;
      operation_envelope += "\t";
      operation_envelope += EscapeOperationOperandField(value);
      operation_envelope += "\n";
    }
    const std::string view_uuid = JsonTextField(encoded, "view_object_uuid").value_or(
        TextLineValue(encoded, "view_object_uuid").value_or(""));
    if (!view_uuid.empty() &&
        JsonTextField(encoded, "target_object_uuid").value_or(
            TextLineValue(encoded, "target_object_uuid").value_or("")).empty()) {
      operation_envelope += "operand=text\ttarget_object_uuid\t";
      operation_envelope += EscapeOperationOperandField(view_uuid);
      operation_envelope += "\n";
      operation_envelope += "operand=text\ttarget_object_kind\tview\n";
    }
    const std::string view_name = JsonTextField(encoded, "view_name").value_or(
        TextLineValue(encoded, "view_name").value_or(""));
    if (!view_name.empty()) {
      operation_envelope += "operand=text\tname\t";
      operation_envelope += EscapeOperationOperandField(view_name);
      operation_envelope += "\n";
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
        "executable_object_kind", "descriptor_kind",
        "signature_descriptor_kind", "signature_descriptor_uuid",
        "signature_descriptor", "routine_language",
        "routine_parameter_descriptor_present", "routine_parameter_count",
        "routine_parameter_0_name_descriptor", "routine_parameter_0_type",
        "routine_parameter_0_mode", "routine_parameter_0_descriptor_kind",
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
    const std::string object_uuid = JsonTextField(encoded, object_uuid_field).value_or(
        TextLineValue(encoded, object_uuid_field).value_or(""));
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
    operation_envelope += "operand=text\tquery_operation\tinner_join\n";
    operation_envelope += "operand=text\tjoin_algorithm\t";
    operation_envelope += EscapeOperationOperandField(JsonTextField(encoded, "join_algorithm").value_or(
        TextLineValue(encoded, "join_algorithm").value_or("hash")));
    operation_envelope += "\n";
    constexpr std::string_view kJoinFields[] = {
        "target_object_uuid", "target_object_kind",
        "related_object_0_uuid", "related_object_0_kind",
        "left_key_field", "right_key_field",
        "left_key_column", "right_key_column",
        "limit", "offset", "order_column", "order"};
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
        "set_by_name", "limit", "offset"};
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
    operation_envelope += "operand=text\texecute\ttrue\n";
    operation_envelope += "operand=text\tquery_operation\tcount_all\n";
    constexpr std::string_view kCountFields[] = {
        "target_object_uuid", "target_object_kind",
        "aggregate_function", "aggregate_value_field",
        "count_all", "limit", "offset"};
    for (const auto field : kCountFields) {
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
  if (dispatch_operation_id.starts_with("dml.")) {
    if (dispatch_operation_id == "dml.insert_rows") {
      AppendDmlInsertValueOperands(session, encoded, &operation_envelope);
    }
    constexpr std::string_view kDmlFields[] = {
        "target_object_uuid", "target_object_kind", "dml_surface_variant",
        "source_kind", "source_uuid", "source_fingerprint", "source_position",
        "redacted_source_handle", "source_handle_sensitive", "format_family",
        "encoding", "line_ending", "delimiter", "quote", "escape",
        "header_policy", "estimated_row_count", "order_by", "order_direction",
        "limit", "offset",
        "predicate_kind", "predicate_column", "predicate_value",
        "predicate_value_type", "assignment_column", "assignment_value",
        "assignment_value_type",
        "strict_bulk_load_requested", "reference_relaxed_semantics_requested",
        "duplicate_mode", "insert_mode", "require_generated_row_uuid", "reject_mode",
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
  }
  AppendExistingOperationOperands(encoded, &operation_envelope);

  const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(PublicAbiFamilyForServerFamily(dispatch_operation_family), 1)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(operation_envelope.data()),
                                        operation_envelope.size())
                          .encode();
  return std::string(reinterpret_cast<const char*>(binary.data()), binary.size());
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
  std::string payload;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  sb_engine_result_t result_handle = nullptr;
};

PublicAbiDispatchResult DispatchThroughPublicAbi(const ServerSessionRecord& session,
                                                 std::string_view operation_id,
                                                 std::string_view operation_family,
                                                 const std::string& encoded,
                                                 bool retain_result_handle) {
  PublicAbiDispatchResult dispatch_result;
  dispatch_result.attempted = true;
  const std::string public_abi_envelope =
      PublicAbiEnvelopeForDispatch(session, encoded, operation_id, operation_family);
  sb_engine_open_params_v1_t open_params{};
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.database_path_utf8 = session.database_path.data();
  open_params.database_path_size = static_cast<std::uint64_t>(session.database_path.size());
  open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_handle_t engine = nullptr;
  if (sb_engine_open(&open_params, &engine, nullptr) != SB_ENGINE_STATUS_OK || engine == nullptr) {
    dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
    dispatch_result.diagnostic_detail = "engine_open_failed";
    return dispatch_result;
  }
  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::memcpy(session_params.effective_user_uuid.bytes, session.effective_user_uuid.data(), 16);
  std::memcpy(session_params.session_uuid.bytes, session.session_uuid.data(), 16);
  session_params.trust_mode = session.embedded_in_process ? SB_ENGINE_TRUST_EMBEDDED_TRUSTED
                                                          : SB_ENGINE_TRUST_SERVER_ISOLATED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;
  sb_engine_session_t engine_session = nullptr;
  if (sb_engine_session_begin(engine, &session_params, &engine_session, nullptr) == SB_ENGINE_STATUS_OK &&
      engine_session != nullptr) {
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
    sb_engine_result_t abi_result = nullptr;
    const auto status = sb_engine_dispatch_sblr(engine_session, nullptr, &context, &dispatch, &abi_result);
    dispatch_result.ok = status == SB_ENGINE_STATUS_OK;
    if (abi_result != nullptr) {
      sb_engine_execution_summary_view_v1_t summary{};
      if (sb_engine_result_summary(abi_result, &summary) == SB_ENGINE_STATUS_OK) {
        dispatch_result.row_count = summary.rows_produced;
      }
      if (!dispatch_result.ok || !retain_result_handle) {
        sb_engine_string_view_t payload{};
        if (sb_engine_result_payload(abi_result, &payload) == SB_ENGINE_STATUS_OK) {
          dispatch_result.payload = StringViewToString(payload);
        }
      }
      if (!dispatch_result.ok) {
        dispatch_result.diagnostic_code = FirstEngineDiagnosticCode(abi_result);
        dispatch_result.diagnostic_detail = FirstEngineDiagnosticDetail(abi_result);
      }
      if (dispatch_result.ok && retain_result_handle) {
        dispatch_result.result_handle = abi_result;
      } else {
        (void)sb_engine_result_release(abi_result);
      }
    }
    if (!dispatch_result.ok && dispatch_result.diagnostic_code.empty()) {
      dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
    }
    sb_engine_session_end_params_v1_t end_params{};
    end_params.struct_size = sizeof(end_params);
    end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end_params.rollback_active_transactions = 1;
    end_params.cancel_open_results = 1;
    (void)sb_engine_session_end(engine_session, &end_params, nullptr);
  } else {
    dispatch_result.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
    dispatch_result.diagnostic_detail = "engine_session_begin_failed";
  }
  (void)sb_engine_close(engine, nullptr);
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
    bool cursor_requested) {
  std::vector<std::uint8_t> out;
  out.reserve(16 + 16 + 1 + 2 + encoded_sblr_envelope.size());
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
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
  prepared.session_uuid = decoded->session_uuid;
  prepared.encoded_sblr_envelope = decoded->encoded_sblr_envelope;
  prepared.operation_id = admission.operation_id;
  prepared.catalog_generation = decoded->catalog_generation;
  prepared.security_epoch = decoded->security_epoch;
  prepared.descriptor_epoch = session->descriptor_epoch;
  prepared.grant_epoch = session->grant_epoch;
  prepared.policy_generation = decoded->policy_generation;
  prepared.role_set_hash = session->role_set_hash;
  prepared.group_set_hash = session->group_set_hash;
  prepared.search_path_hash = session->search_path_hash;
  CapturePreparedLanguageContext(&prepared, *session);
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
                                         const sbps::Frame& request) {
  auto decoded = DecodeExecutePayload(request.payload);
  if (!decoded) {
    return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                   kSchemaExecuteResultTestV1,
                   request.header.session_uuid,
                   "PARSER_SERVER_IPC.EXECUTE_INVALID",
                   "The SBLR execute payload is invalid.",
                   "execute_invalid");
  }
  ServerSessionRecord* session = FindMutableSession(registry, decoded->session_uuid);
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
  std::string encoded = decoded->encoded_sblr_envelope;
  bool cursor_requested = decoded->cursor_requested;
  const auto prepared_it = registry->prepared_by_uuid.find(UuidBytesToText(decoded->prepared_statement_uuid));
  if (encoded.empty() && prepared_it != registry->prepared_by_uuid.end()) {
    const auto& prepared = prepared_it->second;
    if (prepared.closed || prepared.session_uuid != decoded->session_uuid) {
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
    if (!PreparedStatementEpochMatches(prepared, *session)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "prepared_statement_epoch_stale");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                     "The prepared SBLR statement was invalidated by a server authority epoch change.",
                     "prepared_statement_epoch_stale");
    }
    encoded = prepared.encoded_sblr_envelope;
    LinkServerRequestPreparedStatement(registry,
                                       request_record.request_uuid,
                                       decoded->prepared_statement_uuid);
  }
  auto admission = AdmitServerSblrEnvelope(ServerSblrAdmissionRequest{encoded, false});
  if (!admission.admitted) {
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
    prepared.session_uuid = decoded->session_uuid;
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
    prepared.operation_id = inner_admission.operation_id;
    prepared.catalog_generation = session->catalog_generation;
    prepared.security_epoch = session->security_epoch;
    prepared.descriptor_epoch = session->descriptor_epoch;
    prepared.grant_epoch = session->grant_epoch;
    prepared.policy_generation = session->policy_generation;
    prepared.role_set_hash = session->role_set_hash;
    prepared.group_set_hash = session->group_set_hash;
    prepared.search_path_hash = session->search_path_hash;
    CapturePreparedLanguageContext(&prepared, *session);
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
    if (prepared == nullptr || !PreparedStatementEpochMatches(*prepared, *session)) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     prepared == nullptr ? "prepared_statement_not_found"
                                                         : "prepared_statement_epoch_stale");
      return Failure(static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
                     kSchemaExecuteResultTestV1,
                     decoded->session_uuid,
                     prepared == nullptr
                         ? "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND"
                         : "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                     "The prepared SBLR statement is not available for this session.",
                     prepared == nullptr ? "prepared_statement_not_found"
                                         : "prepared_statement_epoch_stale");
    }
    encoded = prepared->encoded_sblr_envelope;
    cursor_requested = cursor_requested || JsonBoolField(decoded->encoded_sblr_envelope,
                                                        "prepared_cursor_requested");
    LinkServerRequestPreparedStatement(registry,
                                       request_record.request_uuid,
                                       prepared->prepared_statement_uuid);
    admission = AdmitServerSblrEnvelope(ServerSblrAdmissionRequest{encoded, false});
    if (!admission.admitted) {
      CompleteServerRequestLifecycle(registry,
                                     request_record.request_uuid,
                                     ServerRequestLifecycleState::kFailed,
                                     "prepared_inner_sblr_rejected");
      return FailureWithDiagnostics(
          static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult),
          kSchemaExecuteResultTestV1,
          decoded->session_uuid,
          admission.diagnostics,
          "prepared_inner_sblr_rejected");
    }
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
      AddArchiveReplicationRestoreOptions(&api_request, manifest_uri);
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
      auto public_abi = DispatchThroughPublicAbi(*session,
                                                 admission.operation_id,
                                                 admission.operation_family,
                                                 encoded,
                                                 retain_engine_result);
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
      }
    }
    ApplyTransactionResultToSession(admission.operation_id, row_packet, session);
  } else {
    row_packet = ResultPacket(admission.operation_id, true, row_count);
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
  } else {
    CompleteServerRequestLifecycle(registry,
                                   request_record.request_uuid,
                                   ServerRequestLifecycleState::kCompleted,
                                   "execute_completed");
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
