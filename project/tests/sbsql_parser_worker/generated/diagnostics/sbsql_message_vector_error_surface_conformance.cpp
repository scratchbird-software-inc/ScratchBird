// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "diagnostics.hpp"
#include "parser_ipc_common.hpp"
#include "rendering/rendering.hpp"
#include "sbps.hpp"
#include "scratchbird/engine/engine.h"
#include "sbu_sbsql_parser_support.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Harness {
  bool ok{true};

  void Require(bool condition, std::string message) {
    if (!condition) {
      ok = false;
      std::cerr << message << '\n';
    }
  }
};

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string LowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

std::string TrimCsvCell(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : value) {
    if (ch == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> cells;
  std::string current;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
        current.push_back('"');
        ++i;
      } else {
        quoted = !quoted;
      }
      continue;
    }
    if (ch == ',' && !quoted) {
      cells.push_back(TrimCsvCell(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  cells.push_back(TrimCsvCell(current));
  return cells;
}

std::uint16_t U16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint32_t U32(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int byte = 3; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

void ZeroU32(std::vector<std::uint8_t>* data, std::size_t offset) {
  for (std::size_t i = 0; i < 4; ++i) {
    (*data)[offset + i] = 0;
  }
}

std::string BytesString(const std::vector<std::uint8_t>& data,
                        std::size_t offset,
                        std::size_t size) {
  return std::string(reinterpret_cast<const char*>(data.data() + offset), size);
}

std::string EngineString(sb_engine_string_view_t view) {
  if (view.data == nullptr || view.size_bytes == 0) return {};
  return std::string(view.data, static_cast<std::size_t>(view.size_bytes));
}

bool ValidateBacklog(const std::filesystem::path& artifacts, Harness* harness) {
  const auto path = artifacts / "MESSAGE_VECTOR_COVERAGE_BACKLOG.csv";
  std::ifstream in(path);
  harness->Require(in.good(), "MESSAGE_VECTOR_COVERAGE_BACKLOG.csv is not readable");
  if (!in.good()) return false;

  std::string header_line;
  std::getline(in, header_line);
  const auto headers = SplitCsvLine(header_line);
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < headers.size(); ++i) {
    index[headers[i]] = i;
  }
  for (const auto& required :
       {"backlog_id", "origin", "subsystem", "error_condition", "diagnostic_code",
        "message_vector_fields", "parser_rendering_template", "redaction_policy",
        "conformance_fixture", "status"}) {
    harness->Require(index.contains(required), std::string("backlog missing column ") + required);
  }
  if (!index.contains("backlog_id") || !index.contains("origin") ||
      !index.contains("subsystem") || !index.contains("diagnostic_code") ||
      !index.contains("message_vector_fields") ||
      !index.contains("parser_rendering_template") ||
      !index.contains("redaction_policy") || !index.contains("conformance_fixture") ||
      !index.contains("status")) {
    return false;
  }

  std::set<std::string> diagnostic_codes;
  std::set<std::string> origins;
  std::set<std::string> subsystems;
  std::set<std::string> prefixes;
  std::size_t row_count = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    ++row_count;
    const auto cells = SplitCsvLine(line);
    harness->Require(cells.size() == headers.size(), "backlog row has unexpected column count: " + line);
    if (cells.size() != headers.size()) continue;

    const auto get = [&](std::string_view key) -> const std::string& {
      return cells[index.at(std::string(key))];
    };
    const auto& id = get("backlog_id");
    const auto& origin = get("origin");
    const auto& subsystem = get("subsystem");
    const auto& code = get("diagnostic_code");
    const auto& fields = get("message_vector_fields");
    const auto& rendering = get("parser_rendering_template");
    const auto& redaction = get("redaction_policy");
    const auto& fixture = get("conformance_fixture");
    const auto& status = get("status");

    harness->Require(Contains(id, "MV-"), "backlog id is not MV-prefixed: " + id);
    harness->Require(!origin.empty(), "backlog origin is empty for " + id);
    harness->Require(!subsystem.empty(), "backlog subsystem is empty for " + id);
    harness->Require(!code.empty() && Contains(code, "."), "diagnostic code is invalid for " + id);
    harness->Require(diagnostic_codes.insert(code).second, "duplicate diagnostic code: " + code);
    harness->Require(!fields.empty() && fields != "TBD", "message vector fields missing for " + id);
    harness->Require(Split(fields, ';').size() >= 3, "message vector fields too narrow for " + id);
    harness->Require(!rendering.empty() && rendering != "TBD", "parser rendering template missing for " + id);
    harness->Require(!redaction.empty() && redaction != "TBD", "redaction policy missing for " + id);
    harness->Require(Contains(fixture, "MSGV-"), "conformance fixture is invalid for " + id);
    harness->Require(!status.empty(), "status missing for " + id);

    const auto lowered = LowerAscii(code + fields + rendering + redaction + fixture);
    harness->Require(!Contains(lowered, "todo") && !Contains(lowered, "stub") &&
                         !Contains(lowered, " tbd"),
                     "placeholder text remains in backlog row " + id);

    origins.insert(origin);
    subsystems.insert(subsystem);
    const auto dot = code.find('.');
    if (dot != std::string::npos) prefixes.insert(code.substr(0, dot));
  }

  harness->Require(row_count >= 30, "message-vector backlog has fewer than 30 seed rows");
  for (const auto& required_origin : {"agent", "server", "engine", "parser", "udr", "listener", "manager"}) {
    harness->Require(origins.contains(required_origin),
                     std::string("message-vector backlog missing origin ") + required_origin);
  }
  for (const auto& required_prefix : {"AGENT", "SERVER", "ENGINE", "SBSQL", "UDR", "LISTENER", "MANAGER"}) {
    harness->Require(prefixes.contains(required_prefix),
                     std::string("message-vector backlog missing diagnostic prefix ") + required_prefix);
  }
  for (const auto& required_subsystem :
       {"physical_device", "filespace", "page_manager", "parser_ipc", "security", "syntax",
        "literal", "resource", "encoding", "binding", "context", "authority", "resolver",
        "admission", "runtime", "streaming", "catalog", "transaction", "datatype",
        "function", "optimizer", "metrics", "endpoint", "control", "cleanup"}) {
    harness->Require(subsystems.contains(required_subsystem),
                     std::string("message-vector backlog missing subsystem ") + required_subsystem);
  }
  return harness->ok;
}

bool ValidateParserRendering(Harness* harness) {
  using scratchbird::parser::sbsql::MakeDiagnostic;
  using scratchbird::parser::sbsql::MessageVectorSet;
  using scratchbird::parser::sbsql::MessageVectorToJson;
  using scratchbird::parser::sbsql::RenderMessageVectorSet;

  MessageVectorSet messages;
  messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.SYNTAX.INVALID_TOKEN", "ERROR", "Unexpected token in SBSQL input.",
      "sbp_sbsql.lexer",
      {{"token_text", "?"}, {"line", "1"}, {"column", "7"},
       {"expected_token_set", "expression"}, {"profile_uuid", "profile-public"}}));

  harness->Require(messages.has_errors(), "parser message vector did not flag ERROR");
  const auto json = MessageVectorToJson(messages);
  harness->Require(Contains(json, "\"code\":\"SBSQL.SYNTAX.INVALID_TOKEN\""),
                   "parser JSON missing diagnostic code");
  harness->Require(Contains(json, "\"component\":\"sbp_sbsql.lexer\""),
                   "parser JSON missing component");
  harness->Require(Contains(json, "\"line\":\"1\"") && Contains(json, "\"expected_token_set\":\"expression\""),
                   "parser JSON missing structured fields");

  const auto rendered = RenderMessageVectorSet(messages);
  harness->Require(Contains(rendered, "MESSAGE ERROR SBSQL.SYNTAX.INVALID_TOKEN"),
                   "parser rendering missing severity/code");
  harness->Require(Contains(rendered, "component=sbp_sbsql.lexer") &&
                       Contains(rendered, "expected_token_set=expression"),
                   "parser rendering missing component/fields");
  return harness->ok;
}

bool ValidateUdrDiagnostic(Harness* harness) {
  const auto refusal =
      scratchbird::udr::sbsql_parser_support::sbu_sbsql_parse_to_sblr("select 1", "");
  harness->Require(!refusal.ok, "UDR missing-context path did not fail closed");
  harness->Require(Contains(refusal.message_vector_json, "\"code\":\"UDR.SBSQL.CONTEXT_MISSING\""),
                   "UDR refusal missing canonical context diagnostic");
  harness->Require(Contains(refusal.message_vector_json, "\"component\":\"sbu_sbsql_parser_support\""),
                   "UDR refusal missing component");
  harness->Require(Contains(refusal.message_vector_json, "\"udr_function\":\"sbu_sbsql_parse_to_sblr\"") &&
                       Contains(refusal.message_vector_json, "\"operation_uuid\":\"not_assigned\""),
                   "UDR refusal missing structured fields");

  const auto normalized =
      scratchbird::udr::sbsql_parser_support::sbu_sbsql_normalize("  select 1  ", "sbsql");
  harness->Require(normalized.ok, "UDR normalize success failed");
  harness->Require(normalized.message_vector_json == "[]",
                   "UDR success path did not return canonical empty message vector");
  return harness->ok;
}

bool ValidateSbpsPayload(const scratchbird::server::ServerDiagnostic& diagnostic,
                         const std::vector<std::uint8_t>& payload,
                         Harness* harness) {
  constexpr std::uint32_t kSbmvMagic = 0x564d4253;
  harness->Require(payload.size() >= 64, "SBMV payload shorter than header");
  if (payload.size() < 64) return false;
  harness->Require(U32(payload, 0) == kSbmvMagic, "SBMV magic missing");
  harness->Require(U16(payload, 4) == 64, "SBMV header size mismatch");
  harness->Require(U16(payload, 6) == 1, "SBMV version mismatch");
  harness->Require(U32(payload, 12) == 1, "SBMV vector count mismatch");
  harness->Require(U32(payload, 16) == payload.size(), "SBMV total length mismatch");

  auto header = std::vector<std::uint8_t>(payload.begin(), payload.begin() + 64);
  const auto header_crc = U32(payload, 52);
  ZeroU32(&header, 52);
  harness->Require(scratchbird::server::sbps::Crc32c(header.data(), header.size()) == header_crc,
                   "SBMV header CRC mismatch");

  const auto records_crc = U32(payload, 20);
  const auto records_size = payload.size() - 64;
  harness->Require(scratchbird::server::sbps::Crc32c(payload.data() + 64, records_size) == records_crc,
                   "SBMV records CRC mismatch");

  const std::size_t record_offset = 64;
  const auto record_bytes = U32(payload, record_offset);
  harness->Require(record_bytes >= 112 && record_offset + record_bytes <= payload.size(),
                   "SBMV record size invalid");
  auto record = std::vector<std::uint8_t>(payload.begin() + static_cast<std::ptrdiff_t>(record_offset),
                                          payload.begin() + static_cast<std::ptrdiff_t>(record_offset + record_bytes));
  const auto record_crc = U32(record, 4);
  ZeroU32(&record, 4);
  harness->Require(scratchbird::server::sbps::Crc32c(record.data(), record.size()) == record_crc,
                   "SBMV record CRC mismatch");

  harness->Require(payload[record_offset + 11] == 2, "SBMV severity byte is not ERROR");
  const auto language_len = U16(payload, record_offset + 92);
  const auto code_len = U16(payload, record_offset + 94);
  const auto key_len = U16(payload, record_offset + 96);
  const auto admin_key_len = U16(payload, record_offset + 98);
  const auto safe_len = U16(payload, record_offset + 100);
  const auto field_count = U16(payload, record_offset + 102);
  std::size_t cursor = record_offset + 112;
  const auto language = BytesString(payload, cursor, language_len);
  cursor += language_len;
  const auto code = BytesString(payload, cursor, code_len);
  cursor += code_len;
  const auto message_key = BytesString(payload, cursor, key_len);
  cursor += key_len + admin_key_len;
  const auto safe_message = BytesString(payload, cursor, safe_len);
  cursor += safe_len;

  harness->Require(language == "en", "SBMV language is not en");
  harness->Require(code == diagnostic.code, "SBMV record code mismatch");
  harness->Require(message_key == diagnostic.message_key, "SBMV record message key mismatch");
  harness->Require(safe_message == diagnostic.safe_message, "SBMV record safe message mismatch");
  std::size_t public_field_count = 0;
  for (const auto& field : diagnostic.fields) {
    if (scratchbird::server::IsPublicDiagnosticFieldAllowed(field.key, field.value)) {
      ++public_field_count;
    }
  }
  harness->Require(field_count == public_field_count, "SBMV field count mismatch");

  std::map<std::string, std::string> fields;
  for (std::uint16_t i = 0; i < field_count; ++i) {
    harness->Require(cursor + 8 <= record_offset + record_bytes, "SBMV TLV header exceeds record");
    if (cursor + 8 > record_offset + record_bytes) return false;
    const auto tlv_key_len = U16(payload, cursor);
    const auto tlv_type = U16(payload, cursor + 2);
    const auto tlv_value_len = U32(payload, cursor + 4);
    cursor += 8;
    harness->Require(tlv_type == 1, "SBMV TLV type is not string");
    harness->Require(cursor + tlv_key_len + tlv_value_len <= record_offset + record_bytes,
                     "SBMV TLV payload exceeds record");
    if (cursor + tlv_key_len + tlv_value_len > record_offset + record_bytes) return false;
    const auto tlv_key = BytesString(payload, cursor, tlv_key_len);
    cursor += tlv_key_len;
    const auto tlv_value = BytesString(payload, cursor, tlv_value_len);
    cursor += tlv_value_len;
    while (((cursor - record_offset) % 4u) != 0u) {
      ++cursor;
    }
    fields[tlv_key] = tlv_value;
  }
  harness->Require(fields["reason_code"] == "raw_sql_forbidden", "SBMV reason_code field mismatch");
  harness->Require(!fields.contains("operation_uuid"),
                   "SBMV public field set leaked operation_uuid");
  return harness->ok;
}

bool ValidateServerDiagnostic(Harness* harness) {
  scratchbird::server::ServerDiagnostic diagnostic{
      "SERVER.ADMISSION.REFUSED",
      "server.admission.refused",
      scratchbird::server::ServerDiagnosticSeverity::kError,
      "Server refused SBSQL operation admission.",
      {{"operation_family", "sblr.query.relational.v3"},
       {"reason_code", "raw_sql_forbidden"},
       {"required_context", "server_admission_envelope"},
       {"operation_uuid", "00000000-0000-7000-8000-000000000010"}}};

  const auto json_line = scratchbird::server::ToMessageVectorJsonLine(diagnostic);
  harness->Require(Contains(json_line, "\"code\":\"SERVER.ADMISSION.REFUSED\""),
                   "server JSON line missing diagnostic code");
  harness->Require(Contains(json_line, "\"message_key\":\"server.admission.refused\""),
                   "server JSON line missing message key");
  harness->Require(Contains(json_line, "\"severity\":\"error\""),
                   "server JSON line missing severity");
  harness->Require(Contains(json_line, "\"operation_family\":\"sblr.query.relational.v3\""),
                   "server JSON line missing structured fields");
  harness->Require(!Contains(json_line, "00000000-0000-7000-8000-000000000010"),
                   "server JSON line leaked operation UUID");

  std::array<std::uint8_t, 16> request_uuid{};
  request_uuid[0] = 0x01;
  request_uuid[6] = 0x70;
  request_uuid[8] = 0x80;
  request_uuid[15] = 0x10;
  const auto payload = scratchbird::server::sbps::EncodeMessageVectorSet({diagnostic}, request_uuid);
  const auto codes = scratchbird::server::sbps::DecodeMessageVectorDiagnosticCodes(payload);
  harness->Require(codes.size() == 1 && codes[0] == diagnostic.code,
                   "SBPS code decoder did not preserve diagnostic code");
  return ValidateSbpsPayload(diagnostic, payload, harness);
}

bool ValidateEngineDiagnostic(Harness* harness) {
  sb_engine_open_params_v1_t bad_open{};
  bad_open.struct_size = sizeof(bad_open) - 1;
  bad_open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  sb_engine_result_t result = nullptr;
  sb_engine_handle_t engine = nullptr;
  const auto status = sb_engine_open(&bad_open, &engine, &result);
  harness->Require(status == SB_ENGINE_STATUS_INVALID_ARGUMENT, "engine bad-open status mismatch");
  harness->Require(engine == nullptr, "engine bad-open unexpectedly returned a handle");
  harness->Require(result != nullptr, "engine bad-open did not return diagnostic result");
  if (result == nullptr) return false;

  sb_engine_diagnostic_set_view_t diagnostics{};
  harness->Require(sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK,
                   "engine diagnostics view failed");
  harness->Require(diagnostics.struct_size == sizeof(diagnostics), "engine diagnostics set size mismatch");
  harness->Require(diagnostics.abi_version == SB_ENGINE_ABI_VERSION_PACKED,
                   "engine diagnostics set ABI mismatch");
  harness->Require(diagnostics.diagnostic_count > 0 && diagnostics.diagnostics != nullptr,
                   "engine diagnostics set is empty");

  bool saw_struct_size = false;
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    const auto code = EngineString(diagnostic.symbolic_code);
    const auto key = EngineString(diagnostic.message_key);
    harness->Require(diagnostic.struct_size == sizeof(diagnostic), "engine diagnostic size mismatch");
    harness->Require(diagnostic.abi_version == SB_ENGINE_ABI_VERSION_PACKED,
                     "engine diagnostic ABI mismatch");
    harness->Require(diagnostic.severity == SB_ENGINE_DIAGNOSTIC_ERROR,
                     "engine diagnostic severity mismatch");
    harness->Require(!code.empty() && Contains(code, "."), "engine diagnostic code missing");
    harness->Require(!key.empty() && Contains(key, "."), "engine diagnostic message key missing");
    if (code == "ENGINE.ABI.STRUCT_SIZE_INVALID") {
      saw_struct_size = true;
    }
  }
  harness->Require(saw_struct_size, "engine diagnostic set missing ENGINE.ABI.STRUCT_SIZE_INVALID");
  (void)sb_engine_result_release(result);
  return harness->ok;
}

}  // namespace

int main(int argc, char** argv) {
  Harness harness;
  if (argc != 2) {
    std::cerr << "usage: sb_message_vector_error_surface_conformance <execution_plan-artifacts-dir>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path artifacts = argv[1];
  ValidateBacklog(artifacts, &harness);
  ValidateParserRendering(&harness);
  ValidateUdrDiagnostic(&harness);
  ValidateServerDiagnostic(&harness);
  ValidateEngineDiagnostic(&harness);
  return harness.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
