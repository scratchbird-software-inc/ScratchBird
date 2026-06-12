// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"
#include "metrics/parser_metrics.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

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

std::string LowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool IsSourceFile(const std::filesystem::path& path) {
  const auto ext = path.extension().string();
  return ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".hh";
}

std::vector<std::filesystem::path> SourceFilesUnder(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_regular_file(root)) {
    if (IsSourceFile(root)) files.push_back(root);
    return files;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || !IsSourceFile(entry.path())) continue;
    files.push_back(entry.path());
  }
  return files;
}

std::string GenericPath(const std::filesystem::path& path) {
  return path.generic_string();
}

bool IsAllowedWalOccurrencePath(const std::filesystem::path& path) {
  const auto generic = GenericPath(path);
  return Contains(generic, "parsers/sbsql_worker/lexer/lexer.cpp") ||
         Contains(generic, "parsers/sbsql_worker/registry/generated/"
                           "sbsql_generated_registry.cpp");
}

bool HasWalAuthorityToken(std::string_view lower) {
  return Contains(lower, "authoritative_wal") ||
         Contains(lower, "wal_authority") ||
         Contains(lower, "wal_required") ||
         Contains(lower, "no_wal") ||
         Contains(lower, "wal_recovery") ||
         Contains(lower, "\"wal\"") ||
         Contains(lower, "write-ahead") ||
         Contains(lower, "write_ahead") ||
         Contains(lower, "redo log") ||
         Contains(lower, "redo_log");
}

bool IsAllowedAntiWalEvidenceLine(std::string_view lower) {
  return Contains(lower, "no_wal") ||
         Contains(lower, "wal_recovery_forbidden") ||
         Contains(lower, "wal_recovery_authority") ||
         Contains(lower, "refusal_wal") ||
         Contains(lower, "authoritative_wal\", \"false") ||
         Contains(lower, "\"authoritative_wal\", \"false") ||
         Contains(lower, "authoritative_wal:false") ||
         Contains(lower, "authoritative_wal_allowed") ||
         Contains(lower, "optionbool(request, \"authoritative_wal:\"") ||
         Contains(lower, "optionenabled(request, \"authoritative_wal:\"") ||
         Contains(lower, "authoritative_wal:\") == \"true") ||
         Contains(lower, "backup_authoritative_wal_forbidden") ||
         Contains(lower, "reference_sqlite_wal_shortcut_forbidden") ||
         Contains(lower, "std::string(\"authoritative_\") + \"wal\"") ||
         Contains(lower, "wal_authority\")") ||
         Contains(lower, "wal_required") ||
         Contains(lower, "wal-not-authority");
}

void ValidateResourcePolicy(const std::filesystem::path& artifact_root,
                            Harness* harness) {
  const auto text = ReadText(artifact_root / "RESOURCE_BUDGET_POLICY.md");
  for (const auto token :
       {"Status: complete", "SQL statement bytes | 1 MiB",
        "Identifier bytes | 256 bytes", "AST depth | 256 nodes deep",
        "Parameter count | 65,535", "SBLR envelope bytes | 16 MiB",
        "Diagnostic payload bytes | 64 KiB",
        "Message-vector count per operation | 1,024",
        "Result metadata columns | 4,096",
        "Parser cache entry count | 10,000",
        "No resource budget may introduce WAL recovery semantics",
        "Budget checks must avoid spinlocks and busy wait loops"}) {
    harness->Check(Contains(text, token),
                   std::string("RESOURCE_BUDGET_POLICY.md missing token ") +
                       token);
  }
}

void ValidateParserResourceDefaults(Harness* harness) {
  const sbsql::ParserResourceBudget budget;
  harness->Check(budget.max_statement_bytes == 1024 * 1024,
                 "max_statement_bytes default mismatch");
  harness->Check(budget.max_identifier_bytes == 256,
                 "max_identifier_bytes default mismatch");
  harness->Check(budget.max_ast_depth == 256, "max_ast_depth default mismatch");
  harness->Check(budget.max_parameter_count == 65535,
                 "max_parameter_count default mismatch");
  harness->Check(budget.max_sblr_envelope_bytes == 16 * 1024 * 1024,
                 "max_sblr_envelope_bytes default mismatch");
  harness->Check(budget.max_diagnostic_payload_bytes == 64 * 1024,
                 "max_diagnostic_payload_bytes default mismatch");
  harness->Check(budget.max_message_vector_count == 1024,
                 "max_message_vector_count default mismatch");
  harness->Check(budget.max_result_metadata_columns == 4096,
                 "max_result_metadata_columns default mismatch");
  harness->Check(budget.max_parser_cache_entries == 10000,
                 "max_parser_cache_entries default mismatch");

  sbsql::ParserConfig config;
  config.parser_uuid = "00000000-0000-7000-8000-hardening001";
  config.resource_budget = budget;
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-hardening002";
  session.connection_uuid = "00000000-0000-7000-8000-hardening003";
  session.metric_redaction_policy = "redact-sensitive";
  sbsql::SblrTemplateCache cache(static_cast<std::size_t>(budget.max_parser_cache_entries));
  sbsql::ParserMetrics metrics;
  metrics.SetState(sbsql::ParserState::kActive);

  for (const auto metric :
       {"sys.metrics.parsers.parse_pipeline.attempts_total",
        "sys.metrics.parsers.bind.attempts_total",
        "sys.metrics.parsers.name_resolution.calls_total",
        "sys.metrics.parsers.sblr_lowering.attempts_total",
        "sys.metrics.parsers.udr.calls_total",
        "sys.metrics.parsers.server_ipc.calls_total",
        "sys.metrics.parsers.cache.lookup_total",
        "sys.metrics.parsers.rendering.calls_total",
        "sys.metrics.parsers.errors.total",
        "sys.metrics.parsers.resource.limit_exceeded_total",
        "sys.metrics.parsers.control.commands_total"}) {
    metrics.Increment(metric);
  }
  metrics.SetGauge("sys.metrics.parsers.resource.last_statement_bytes",
                   static_cast<double>(budget.max_statement_bytes));

  const auto snapshot = metrics.SnapshotJson(config, session, cache);
  const auto heartbeat = metrics.HeartbeatJson(config, session, cache, "hardening_gate");
  for (const auto token :
       {"\"namespace\":\"sys.metrics.parsers\"",
        "\"resource_budgets\":{", "\"max_statement_bytes\":1048576",
        "\"max_identifier_bytes\":256", "\"max_ast_depth\":256",
        "\"max_parameter_count\":65535", "\"max_sblr_envelope_bytes\":16777216",
        "\"max_diagnostic_payload_bytes\":65536",
        "\"max_message_vector_count\":1024",
        "\"max_result_metadata_columns\":4096",
        "\"max_parser_cache_entries\":10000",
        "sys.metrics.parsers.parse_pipeline.attempts_total",
        "sys.metrics.parsers.bind.attempts_total",
        "sys.metrics.parsers.name_resolution.calls_total",
        "sys.metrics.parsers.sblr_lowering.attempts_total",
        "sys.metrics.parsers.udr.calls_total",
        "sys.metrics.parsers.server_ipc.calls_total",
        "sys.metrics.parsers.cache.lookup_total",
        "sys.metrics.parsers.rendering.calls_total",
        "sys.metrics.parsers.errors.total",
        "sys.metrics.parsers.resource.limit_exceeded_total",
        "sys.metrics.parsers.control.commands_total"}) {
    harness->Check(Contains(snapshot, token),
                   std::string("metrics snapshot missing token ") + token);
  }
  for (const auto token :
       {"\"current_operation\":\"hardening_gate\"",
        "\"cache_counts\":", "\"resource_budgets\":{",
        "\"redaction_state\":\"redact-sensitive\""}) {
    harness->Check(Contains(heartbeat, token),
                   std::string("metrics heartbeat missing token ") + token);
  }
}

void ValidateSourceHardening(const std::filesystem::path& project_root,
                             Harness* harness) {
  const std::vector<std::filesystem::path> scan_roots = {
      project_root / "src/parsers/sbsql_worker",
      project_root / "src/udr/sbu_sbsql_parser_support",
      project_root / "src/server",
      project_root / "src/engine/internal_api",
      project_root / "src/engine/sblr",
      project_root / "src/engine/public_abi.cpp",
  };

  const std::set<std::string> parser_or_udr_direct_db_terms = {
      "database_lifecycle.hpp",
      "OpenDatabaseFile",
      "CreateDatabaseFile",
      "MarkDatabaseCleanShutdown",
      "sb_engine_open",
      "std::ifstream",
      "std::ofstream",
      "std::fstream",
      "fopen(",
  };

  for (const auto& root : scan_roots) {
    for (const auto& path : SourceFilesUnder(root)) {
      const auto text = ReadText(path);
      const auto lower = LowerAscii(text);
      const auto generic = GenericPath(path);

      for (const auto token : {"spinlock", "busy_wait", "busy-wait"}) {
        harness->Check(!Contains(lower, token),
                       generic + " contains forbidden spin/busy-wait token " +
                           token);
      }

      if (!IsAllowedWalOccurrencePath(path)) {
        std::size_t line_number = 0;
        std::string line;
        std::istringstream input(text);
        while (std::getline(input, line)) {
          ++line_number;
          const auto lower_line = LowerAscii(line);
          if (!HasWalAuthorityToken(lower_line)) continue;
          harness->Check(IsAllowedAntiWalEvidenceLine(lower_line),
                         generic + ":" + std::to_string(line_number) +
                             " contains WAL text that is not explicit "
                             "anti-WAL/refusal evidence");
        }
      }

      if (Contains(generic, "src/parsers/sbsql_worker") ||
          Contains(generic, "src/udr/sbu_sbsql_parser_support")) {
        for (const auto& token : parser_or_udr_direct_db_terms) {
          harness->Check(!Contains(text, token),
                         generic +
                             " contains forbidden parser/UDR direct DB or file "
                             "authority token " +
                             token);
        }
      }

      if (Contains(generic, "src/engine/internal_api") ||
          Contains(generic, "src/engine/sblr") ||
          Contains(generic, "src/engine/public_abi.cpp")) {
        for (const auto token : {"BuildCst(", "BuildAst(", "BindAst(",
                                 "LowerToSblr(", "parsers/sbsql_worker"}) {
          harness->Check(!Contains(text, token),
                         generic +
                             " contains parser pipeline authority in engine path");
        }
      }
    }
  }

  const auto wire_text =
      ReadText(project_root / "src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp");
  harness->Check(Contains(wire_text, "SBSQL.RESOURCE.STATEMENT_TOO_LARGE"),
                 "parser wire path does not expose statement resource diagnostic");
  harness->Check(Contains(wire_text, "max_statement_bytes"),
                 "parser wire path does not enforce max_statement_bytes");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sbp_sbsql_no_spin_no_wal_no_direct_db_gate "
                 "<project-root> <artifact-root>\n";
    return 1;
  }

  const std::filesystem::path project_root(argv[1]);
  const std::filesystem::path artifact_root(argv[2]);
  Harness harness;

  try {
    ValidateResourcePolicy(artifact_root, &harness);
    ValidateParserResourceDefaults(&harness);
    ValidateSourceHardening(project_root, &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-012 hardening gate failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-012 hardening gate passed: "
              << "resource_budgets=default metrics=visible no_spin=true "
                 "no_wal_recovery=true no_direct_parser_db=true\n";
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-012 hardening gate failed: " << ex.what() << '\n';
    return 1;
  }

  return 0;
}
