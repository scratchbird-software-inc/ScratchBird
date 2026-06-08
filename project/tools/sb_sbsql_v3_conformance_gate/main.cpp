// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
  std::filesystem::path execution_plan_root;
};

struct Fixture {
  std::string id;
  int stage = -1;
  std::string polarity;
  std::string command;
  std::string expected_gate;
  std::string expected_result;
};

std::string Trim(const std::string& value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n\"'");
  if (first == std::string::npos) { return {}; }
  const std::size_t last = value.find_last_not_of(" \t\r\n\"'");
  return value.substr(first, last - first + 1);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

bool ReadFile(const std::filesystem::path& path, std::string* content) {
  std::ifstream in(path);
  if (!in) { return false; }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *content = buffer.str();
  return true;
}

std::string ValueAfterColon(const std::string& line) {
  const auto pos = line.find(':');
  if (pos == std::string::npos) { return {}; }
  return Trim(line.substr(pos + 1));
}

int ParseInt(const std::string& value) {
  try {
    return std::stoi(value);
  } catch (...) {
    return -1;
  }
}

std::vector<Fixture> ParseFixtures(const std::string& text) {
  std::vector<Fixture> fixtures;
  Fixture current;
  bool active = false;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = Trim(line);
    if (StartsWith(trimmed, "- id:")) {
      if (active) { fixtures.push_back(current); }
      current = Fixture{};
      current.id = Trim(trimmed.substr(5));
      active = true;
      continue;
    }
    if (!active) { continue; }
    if (StartsWith(trimmed, "stage:")) { current.stage = ParseInt(ValueAfterColon(trimmed)); }
    else if (StartsWith(trimmed, "polarity:")) { current.polarity = ValueAfterColon(trimmed); }
    else if (StartsWith(trimmed, "command:")) { current.command = ValueAfterColon(trimmed); }
    else if (StartsWith(trimmed, "expected_gate:")) { current.expected_gate = ValueAfterColon(trimmed); }
    else if (StartsWith(trimmed, "expected_result:")) { current.expected_result = ValueAfterColon(trimmed); }
  }
  if (active) { fixtures.push_back(current); }
  return fixtures;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--execution_plan-root") {
      args->execution_plan_root = value;
    } else {
      return false;
    }
  }
  return !args->execution_plan_root.empty();
}

void AddFailure(std::vector<std::string>* failures, std::string failure) {
  failures->push_back(std::move(failure));
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_conformance_gate --execution_plan-root PATH\n";
    return 2;
  }

  std::vector<std::string> failures;
  const std::vector<std::string> required_files = {
      "README.md",
      "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
      "SBSQL_V3_CONFORMANCE_SUITE.md",
      "SBSQL_V3_CONFORMANCE_MATRIX.yaml",
      "conformance/SBSQL_V3_CONFORMANCE_FIXTURES.yaml",
      "SBSQL_V3_NATIVE_PARSER_PACKAGE_INTEGRATION.md",
      "SBSQL_V3_DIAGNOSTIC_RENDERING.md",
      "SBSQL_V3_CLUSTER_PLACEHOLDER_COMMANDS.md",
      "SBSQL_V3_EXTENSION_COMMANDS_CONTRACT.md",
      "SBSQL_V3_NOSQL_SPECIALIZED_SURFACES_CONTRACT.md"};

  for (const auto& file : required_files) {
    if (!std::filesystem::exists(args.execution_plan_root / file)) {
      AddFailure(&failures, "missing_required_file:" + file);
    }
  }

  std::string readme;
  if (ReadFile(args.execution_plan_root / "README.md", &readme)) {
    for (int stage = 0; stage <= 15; ++stage) {
      const std::string prefix = "| Stage " + std::to_string(stage) + ":";
      const std::string validated = "| validated |";
      const auto pos = readme.find(prefix);
      if (pos == std::string::npos) {
        AddFailure(&failures, "missing_stage_tracking:" + std::to_string(stage));
        continue;
      }
      const auto line_end = readme.find('\n', pos);
      const std::string line = readme.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
      if (!Contains(line, validated)) { AddFailure(&failures, "stage_not_validated:" + std::to_string(stage)); }
    }
  } else {
    AddFailure(&failures, "cannot_read_readme");
  }

  std::string matrix;
  if (ReadFile(args.execution_plan_root / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv", &matrix)) {
    if (!Contains(matrix, ",15,SBSQL_V3_NATIVE_PARSER_PACKAGE_INTEGRATION.md,validated,")) {
      AddFailure(&failures, "stage15_audit_matrix_not_validated");
    }
    if (!Contains(matrix, "Canonical diagnostics and parser rendering both exist,14,SBSQL_V3_DIAGNOSTIC_RENDERING.md,validated,")) {
      AddFailure(&failures, "stage14_audit_matrix_not_validated");
    }
  } else {
    AddFailure(&failures, "cannot_read_audit_matrix");
  }

  std::string fixture_text;
  std::vector<Fixture> fixtures;
  if (ReadFile(args.execution_plan_root / "conformance/SBSQL_V3_CONFORMANCE_FIXTURES.yaml", &fixture_text)) {
    fixtures = ParseFixtures(fixture_text);
  } else {
    AddFailure(&failures, "cannot_read_fixture_manifest");
  }

  std::set<std::string> ids;
  int positive = 0;
  int negative = 0;
  for (const auto& fixture : fixtures) {
    if (fixture.id.empty()) { AddFailure(&failures, "fixture_missing_id"); }
    if (!ids.insert(fixture.id).second) { AddFailure(&failures, "duplicate_fixture_id:" + fixture.id); }
    if (fixture.stage < 0 || fixture.stage > 16) { AddFailure(&failures, "fixture_invalid_stage:" + fixture.id); }
    if (fixture.polarity == "positive") { ++positive; }
    else if (fixture.polarity == "negative") { ++negative; }
    else { AddFailure(&failures, "fixture_invalid_polarity:" + fixture.id); }
    if (fixture.command.empty()) { AddFailure(&failures, "fixture_missing_command:" + fixture.id); }
    if (fixture.expected_gate.empty()) { AddFailure(&failures, "fixture_missing_expected_gate:" + fixture.id); }
    if (fixture.expected_result.empty()) { AddFailure(&failures, "fixture_missing_expected_result:" + fixture.id); }
  }

  if (positive < 8) { AddFailure(&failures, "insufficient_positive_fixtures"); }
  if (negative < 7) { AddFailure(&failures, "insufficient_negative_fixtures"); }

  const std::vector<std::string> required_ids = {
      "sbv3.show.version.positive",
      "sbv3.show.version.negative.missing_parser_context",
      "sbv3.show.database.positive",
      "sbv3.show.database.negative.unsupported_context",
      "sbv3.unsupported_command.negative.no_dispatch",
      "diagnostics.render.success.positive",
      "diagnostics.render.negative.missing_parser_package",
      "cluster.inspect_state.negative.fail_closed"};

  for (const auto& id : required_ids) {
    if (!ids.contains(id)) { AddFailure(&failures, "missing_required_fixture:" + id); }
  }

  const bool ok = failures.empty();
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"fixture_count\": " << fixtures.size() << ",\n";
  std::cout << "  \"positive_fixture_count\": " << positive << ",\n";
  std::cout << "  \"negative_fixture_count\": " << negative << ",\n";
  std::cout << "  \"failure_count\": " << failures.size() << ",\n";
  std::cout << "  \"failures\": [\n";
  for (std::size_t i = 0; i < failures.size(); ++i) {
    std::cout << "    \"" << JsonEscape(failures[i]) << "\"";
    if (i + 1 != failures.size()) { std::cout << ","; }
    std::cout << "\n";
  }
  std::cout << "  ]\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}

