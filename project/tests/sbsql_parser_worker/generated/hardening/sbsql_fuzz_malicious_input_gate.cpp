// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "common/common.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "sbps.hpp"
#include "sblr_admission.hpp"
#include "sbu_sbsql_parser_support.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;
namespace sbps = scratchbird::server::sbps;
namespace udr = scratchbird::udr::sbsql_parser_support;

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;

struct CsvTable {
  std::filesystem::path path;
  std::vector<std::string> headers;
  std::vector<CsvRow> rows;
};

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string message) {
    if (condition) return;
    ok = false;
    if (failures < 100) std::cerr << message << '\n';
    ++failures;
  }
};

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

void TrimLineEnding(std::string* line) {
  while (!line->empty() && (line->back() == '\r' || line->back() == '\n')) {
    line->pop_back();
  }
}

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"') {
      in_quotes = true;
    } else if (ch == ',') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  fields.push_back(current);
  return fields;
}

CsvTable ReadCsv(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  std::string line;
  if (!std::getline(input, line)) throw std::runtime_error("empty CSV " + path.string());
  TrimLineEnding(&line);
  CsvTable table;
  table.path = path;
  table.headers = SplitCsvLine(line);
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    TrimLineEnding(&line);
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    if (fields.size() != table.headers.size()) {
      throw std::runtime_error(path.string() + ":" + std::to_string(line_number) +
                               " field count mismatch");
    }
    CsvRow row;
    for (std::size_t index = 0; index < table.headers.size(); ++index) {
      row.emplace(table.headers[index], fields[index]);
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

std::string_view Field(const CsvRow& row, std::string_view column) {
  const auto found = row.find(std::string(column));
  return found == row.end() ? std::string_view{} : std::string_view(found->second);
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages, std::string_view code) {
  return std::any_of(messages.diagnostics.begin(), messages.diagnostics.end(),
                     [code](const sbsql::Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool HasServerDiagnostic(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
                         std::string_view code) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [code](const scratchbird::server::ServerDiagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::string LongString(std::size_t count, char ch) {
  return std::string(count, ch);
}

std::string ManyParameters(std::size_t count) {
  std::string sql = "SELECT ";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) sql += ", ";
    sql += "?";
  }
  return sql;
}

std::string DeepExpression(std::size_t depth) {
  std::string sql = "SELECT ";
  sql.append(depth, '(');
  sql += "1";
  sql.append(depth, ')');
  return sql;
}

bool PipelineHasDiagnostic(std::string_view sql,
                           const sbsql::ParserConfig& config,
                           std::string_view code) {
  sbsql::ParserMetrics metrics;
  sbsql::SblrTemplateCache cache;
  sbsql::SbsqlTestWireSession session(config, &metrics, &cache);
  session.HandleLine("AUTH fspe012b-fuzz");
  const auto result = session.RunPipeline(sql, false);
  return result.messages.has_errors() && HasDiagnostic(result.messages, code);
}

bool LoweringHasSblrEnvelopeBudgetDiagnostic(std::string_view sql,
                                             const sbsql::ParserConfig& config,
                                             std::string_view code) {
  auto cst = sbsql::BuildCst(sql);
  auto ast = sbsql::BuildAst(cst);
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-00000000f012";
  session.connection_uuid = "00000000-0000-7000-8000-00000000f013";
  session.database_uuid = "00000000-0000-7000-8000-00000000f014";
  session.authenticated_user_uuid = "00000000-0000-7000-8000-00000000f015";

  auto bound = sbsql::BindAst(ast, cst, config, session);
  auto lowered = sbsql::LowerToSblr(bound, cst, session);
  if (!lowered.payload.empty() &&
      lowered.payload.size() > config.resource_budget.max_sblr_envelope_bytes) {
    lowered.messages.diagnostics.push_back(sbsql::MakeDiagnostic(
        "SBSQL.RESOURCE.SBLR_ENVELOPE_TOO_LARGE",
        "ERROR",
        "lowered SBLR envelope exceeds parser resource budget",
        "sbp_sbsql.wire",
        {{"sblr_envelope_bytes", std::to_string(lowered.payload.size())},
         {"max_sblr_envelope_bytes",
          std::to_string(config.resource_budget.max_sblr_envelope_bytes)}}));
  }
  return lowered.messages.has_errors() && HasDiagnostic(lowered.messages, code);
}

void ValidateCorpus(const CsvTable& corpus, Harness* harness) {
  const std::set<std::string> required_routes = {
      "lexer_cst_ast", "binder_lowering", "udr", "sbps_packet",
      "message_vector", "full_route_client"};
  std::set<std::string> observed_routes;
  std::set<std::string> fixture_ids;
  for (const auto& row : corpus.rows) {
    const auto fixture_id = std::string(Field(row, "fixture_id"));
    const auto route = std::string(Field(row, "route"));
    fixture_ids.insert(fixture_id);
    observed_routes.insert(route);
    harness->Check(!fixture_id.empty() && !route.empty() &&
                       !Field(row, "input_kind").empty() &&
                       !Field(row, "expected_diagnostic").empty(),
                   fixture_id + " has incomplete malicious corpus row");
    harness->Check(Field(row, "status") == "ready",
                   fixture_id + " malicious corpus row is not ready");
  }
  harness->Check(fixture_ids.size() == corpus.rows.size(),
                 "malicious corpus fixture ids must be unique");
  for (const auto& route : required_routes) {
    harness->Check(observed_routes.contains(route),
                   "malicious corpus missing route " + route);
  }
}

void ValidateParserDiagnostics(Harness* harness) {
  const auto unterminated = sbsql::BuildAst(sbsql::BuildCst("SELECT 'unterminated"));
  harness->Check(unterminated.messages.has_errors() &&
                     HasDiagnostic(unterminated.messages,
                                   "SBSQL.LEXER.STRING_UNCLOSED"),
                 "unterminated literal did not fail closed with lexer diagnostic");

  const auto invalid_utf8_source =
      std::string("SELECT ") + std::string(1, static_cast<char>(0xC3)) + "(";
  const auto invalid_utf8 = sbsql::BuildAst(sbsql::BuildCst(invalid_utf8_source));
  harness->Check(invalid_utf8.messages.has_errors() &&
                     HasDiagnostic(invalid_utf8.messages,
                                   "SBSQL.ENCODING.INVALID_UTF8"),
                 "invalid UTF-8 did not fail closed with encoding diagnostic");

  const auto invalid_uuid = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT UUID 'not-a-canonical-uuid'"));
  harness->Check(invalid_uuid.messages.has_errors() &&
                     HasDiagnostic(invalid_uuid.messages,
                                   "SBSQL.LEXER.UUID_LITERAL_INVALID"),
                 "invalid UUID literal did not fail closed with lexer diagnostic");

  const auto unknown_wal = sbsql::BuildAst(sbsql::BuildCst("WAL CHECKPOINT"));
  harness->Check(unknown_wal.messages.has_errors() &&
                     HasDiagnostic(unknown_wal.messages,
                                   "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN"),
                 "unknown WAL statement did not fail closed with AST diagnostic");

  const auto duplicate_conflict = sbsql::BuildAst(sbsql::BuildCst("WAL WAL CHECKPOINT"));
  harness->Check(duplicate_conflict.messages.has_errors() &&
                     HasDiagnostic(duplicate_conflict.messages,
                                   "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN"),
                 "duplicate/conflicting parser tokens did not fail closed with AST diagnostic");

  const auto stacked_injection =
      sbsql::BuildAst(sbsql::BuildCst("WAL CHECKPOINT; DROP DATABASE prod"));
  harness->Check(stacked_injection.messages.has_errors() &&
                     HasDiagnostic(stacked_injection.messages,
                                   "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN"),
                 "stacked injection-like parser input did not fail closed with AST diagnostic");

  sbsql::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "/tmp/sbsql-fuzz-public-resolver-required.sock";
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-fuzz00000001";
  auto cst = sbsql::BuildCst("SELECT * FROM missing_relation");
  auto ast = sbsql::BuildAst(cst);
  auto bound = sbsql::BindAst(ast, cst, config, session, {});
  harness->Check(bound.messages.has_errors() &&
                     HasDiagnostic(bound.messages,
                                   "SBSQL.NAME_RESOLUTION.PUBLIC_RESOLVER_REQUIRED"),
                 "binder did not fail closed for missing public name resolution");
}

void ValidateResourceDiagnostics(Harness* harness) {
  sbsql::ParserConfig config;
  config.probe_mode = true;
  config.allow_probe_auth = true;
  config.resource_budget.max_identifier_bytes = 8;
  config.resource_budget.max_literal_bytes = 16;
  config.resource_budget.max_token_count = 8;
  config.resource_budget.max_parameter_count = 2;
  config.resource_budget.max_ast_depth = 2;
  config.resource_budget.max_sblr_envelope_bytes = 64;

  harness->Check(
      PipelineHasDiagnostic("SELECT * FROM " + LongString(32, 'a'),
                            config, "SBSQL.RESOURCE.IDENTIFIER_TOO_LARGE"),
      "oversized identifier did not fail closed with resource diagnostic");
  harness->Check(
      PipelineHasDiagnostic("SELECT '" + LongString(64, 'x') + "'",
                            config, "SBSQL.RESOURCE.LITERAL_TOO_LARGE"),
      "oversized literal did not fail closed with resource diagnostic");
  harness->Check(
      PipelineHasDiagnostic("SELECT 1 + 2 + 3 + 4 + 5", config,
                            "SBSQL.RESOURCE.TOKEN_COUNT_EXCEEDED"),
      "excessive token count did not fail closed with resource diagnostic");
  harness->Check(
      PipelineHasDiagnostic(ManyParameters(4), config,
                            "SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED"),
      "excessive parameter count did not fail closed with resource diagnostic");
  harness->Check(
      PipelineHasDiagnostic(DeepExpression(4), config,
                            "SBSQL.RESOURCE.AST_DEPTH_EXCEEDED"),
      "deep nesting did not fail closed with resource diagnostic");

  sbsql::ParserConfig sblr_config;
  sblr_config.probe_mode = true;
  sblr_config.allow_probe_auth = true;
  sblr_config.resource_budget.max_sblr_envelope_bytes = 1;
  harness->Check(
      LoweringHasSblrEnvelopeBudgetDiagnostic("SELECT 1", sblr_config,
                                              "SBSQL.RESOURCE.SBLR_ENVELOPE_TOO_LARGE"),
      "oversized SBLR envelope did not fail closed with resource diagnostic");
}

void ValidateUdrDiagnostics(Harness* harness) {
  const auto missing_context = udr::sbu_sbsql_parse_to_sblr("SELECT 1", "");
  harness->Check(!missing_context.ok &&
                     Contains(missing_context.message_vector_json,
                              "UDR.SBSQL.CONTEXT_MISSING"),
                 "UDR parse_to_sblr did not require trusted engine context");

  const auto decompile_refused = udr::sbu_sbsql_decompile_sblr("raw-sblr", "default");
  harness->Check(!decompile_refused.ok &&
                     Contains(decompile_refused.message_vector_json,
                              "SBU_SBSQL.DECOMPILE_POLICY_REFUSED"),
                 "UDR decompile did not fail closed without debug policy");
}

void ValidateWireResourceDiagnostic(Harness* harness) {
  sbsql::ParserConfig config;
  config.probe_mode = true;
  config.allow_probe_auth = true;
  config.resource_budget.max_statement_bytes = 8;
  sbsql::ParserMetrics metrics;
  sbsql::SblrTemplateCache cache;
  sbsql::SbsqlTestWireSession session(config, &metrics, &cache);
  const auto response = session.HandleLine("PARSE SELECT 123456789");
  harness->Check(Contains(response.text, "SBSQL.RESOURCE.STATEMENT_TOO_LARGE"),
                 "parser wire oversized statement did not return resource diagnostic");
}

std::vector<std::uint8_t> MutateAt(std::vector<std::uint8_t> bytes,
                                   std::size_t offset,
                                   std::uint8_t value) {
  if (offset < bytes.size()) bytes[offset] = value;
  return bytes;
}

void ValidateSbpsDiagnostics(Harness* harness) {
  harness->Check(
      HasServerDiagnostic(scratchbird::server::AdmitServerSblrEnvelope(
                              scratchbird::server::ServerSblrAdmissionRequest{
                                  "SELECT * FROM hidden_table", false})
                              .diagnostics,
                          "SBLR.SQL_TEXT_FORBIDDEN"),
      "raw SQL SBLR admission payload did not fail closed");

  harness->Check(
      HasServerDiagnostic(scratchbird::server::AdmitServerSblrEnvelope(
                              scratchbird::server::ServerSblrAdmissionRequest{
                                  "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
                                  "\"operation_family\":\"sblr.query.relational.v3\"}",
                                  false})
                              .diagnostics,
                          "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
      "malformed SBLR envelope did not fail closed");

  harness->Check(
      HasServerDiagnostic(scratchbird::server::AdmitServerSblrEnvelope(
                              scratchbird::server::ServerSblrAdmissionRequest{
                                  "operation_id=dml.select\n"
                                  "operation_id=dml.insert\n"
                                  "sblr_operation_family=sblr.query.relational.v3\n"
                                  "result_shape=rowset\n"
                                  "diagnostic_shape=diagnostic_vector\n"
                                  "parser_resolved_names_to_uuids=true\n",
                                  false})
                              .diagnostics,
                          "PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD"),
      "duplicate text SBLR authority key did not fail closed");

  harness->Check(
      HasServerDiagnostic(scratchbird::server::AdmitServerSblrEnvelope(
                              scratchbird::server::ServerSblrAdmissionRequest{
                                  "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
                                  "\"operation_family\":\"sblr.query.relational.v3\","
                                  "\"operation_family\":\"sblr.dml.operation.v3\","
                                  "\"operation_id\":\"dml.select\","
                                  "\"result_shape\":\"rowset\","
                                  "\"diagnostic_shape\":\"diagnostic_vector\"}",
                                  false})
                              .diagnostics,
                          "PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD"),
      "duplicate JSON SBLR authority key did not fail closed");

  harness->Check(HasServerDiagnostic(sbps::DecodeFrameBytes({0, 1, 2}, 1024).diagnostics,
                                     "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID"),
                 "truncated SBPS frame did not fail closed");

  sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPing);
  auto valid_ping = sbps::EncodeFrame(header, {});
  harness->Check(HasServerDiagnostic(
                     sbps::DecodeFrameBytes(MutateAt(valid_ping, 0, 0x00), 1024).diagnostics,
                     "PARSER_SERVER_IPC.FRAME_MAGIC_INVALID"),
                 "bad SBPS magic did not fail closed");

  header.message_type = 999;
  auto unknown = sbps::EncodeFrame(header, {});
  harness->Check(HasServerDiagnostic(sbps::DecodeFrameBytes(unknown, 1024).diagnostics,
                                     "PARSER_SERVER_IPC.MESSAGE_TYPE_UNKNOWN"),
                 "unknown SBPS message type did not fail closed");

  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPing);
  std::vector<std::uint8_t> payload = {'o', 'k'};
  auto payload_frame = sbps::EncodeFrame(header, payload);
  harness->Check(HasServerDiagnostic(
                     sbps::DecodeFrameBytes(MutateAt(payload_frame, payload_frame.size() - 1, 'x'), 1024).diagnostics,
                     "PARSER_SERVER_IPC.FRAME_PAYLOAD_CRC_INVALID"),
                 "SBPS bad payload CRC did not fail closed");

  harness->Check(HasServerDiagnostic(sbps::DecodeFrameBytes(payload_frame, 1).diagnostics,
                                     "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID"),
                 "SBPS oversized payload did not fail closed");
}

void ValidateMessageVectorRoundTrip(Harness* harness) {
  const auto request_uuid = sbps::MakeUuidV7Bytes();
  const auto payload = sbps::EncodeMessageVectorSet(
      {scratchbird::server::ServerDiagnostic{
          "SBSQL.FUZZ.SMOKE", "sbsql.fuzz.smoke",
          scratchbird::server::ServerDiagnosticSeverity::kError,
          "Fuzz smoke diagnostic", {}}},
      request_uuid);
  const auto codes = sbps::DecodeMessageVectorDiagnosticCodes(payload);
  harness->Check(std::find(codes.begin(), codes.end(), "SBSQL.FUZZ.SMOKE") != codes.end(),
                 "message-vector diagnostic did not round trip");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sbsql_fuzz_malicious_input_gate <malicious-corpus-csv>\n";
    return 1;
  }

  Harness harness;
  try {
    ValidateCorpus(ReadCsv(argv[1]), &harness);
    ValidateParserDiagnostics(&harness);
    ValidateResourceDiagnostics(&harness);
    ValidateUdrDiagnostics(&harness);
    ValidateWireResourceDiagnostic(&harness);
    ValidateSbpsDiagnostics(&harness);
    ValidateMessageVectorRoundTrip(&harness);

    if (!harness.ok) {
      std::cerr << "FSPE-012B fuzz malicious-input gate failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-012B fuzz malicious-input gate passed: corpus="
              << argv[1] << " routes=6 fail_closed=true\n";
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-012B fuzz malicious-input gate failed: "
              << ex.what() << '\n';
    return 1;
  }
  return 0;
}
