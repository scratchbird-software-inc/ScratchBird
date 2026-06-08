// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
  std::filesystem::path execution_plan_root;
};

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

bool ReadFile(const std::filesystem::path& path, std::string* content) {
  std::ifstream in(path);
  if (!in) { return false; }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *content = buffer.str();
  return true;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
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

void AddFailure(std::vector<std::string>* failures, std::string failure) {
  failures->push_back(std::move(failure));
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_full_gate --execution_plan-root PATH\n";
    return 2;
  }

  std::vector<std::string> failures;
  const std::vector<std::string> required_files = {
      "README.md",
      "IMPLEMENTATION_TRACE.md",
      "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
      "SBSQL_V3_EXECUTION_PLAN_CLOSURE_REPORT.md",
      "SBSQL_V3_FINAL_IMPLEMENTATION_STATUS.yaml",
      "SBSQL_V3_CONFORMANCE_SUITE.md",
      "SBSQL_V3_CONFORMANCE_MATRIX.yaml",
      "conformance/SBSQL_V3_CONFORMANCE_FIXTURES.yaml"};

  for (const auto& file : required_files) {
    if (!std::filesystem::exists(args.execution_plan_root / file)) {
      AddFailure(&failures, "missing_required_file:" + file);
    }
  }

  std::string readme;
  if (ReadFile(args.execution_plan_root / "README.md", &readme)) {
    for (int stage = 0; stage <= 16; ++stage) {
      const std::string prefix = "| Stage " + std::to_string(stage) + ":";
      const auto pos = readme.find(prefix);
      if (pos == std::string::npos) {
        AddFailure(&failures, "missing_stage_tracking:" + std::to_string(stage));
        continue;
      }
      const auto line_end = readme.find('\n', pos);
      const std::string line = readme.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
      if (!Contains(line, "| validated |")) { AddFailure(&failures, "stage_not_validated:" + std::to_string(stage)); }
    }
    const auto stage17_pos = readme.find("| Stage 17:");
    if (stage17_pos == std::string::npos) {
      AddFailure(&failures, "missing_stage_tracking:17");
    } else {
      const auto line_end = readme.find('\n', stage17_pos);
      const std::string line = readme.substr(stage17_pos, line_end == std::string::npos ? std::string::npos : line_end - stage17_pos);
      if (Contains(line, "| pending |")) { AddFailure(&failures, "stage17_still_pending"); }
    }
  } else {
    AddFailure(&failures, "cannot_read_readme");
  }

  std::string matrix;
  if (ReadFile(args.execution_plan_root / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv", &matrix)) {
    if (!Contains(matrix, "All matrices and evidence are updated before completion,17,SBSQL_V3_EXECUTION_PLAN_CLOSURE_REPORT.md,implemented_pending_validation,sb_sbsql_v3_full_gate") &&
        !Contains(matrix, "All matrices and evidence are updated before completion,17,SBSQL_V3_EXECUTION_PLAN_CLOSURE_REPORT.md,validated,sb_sbsql_v3_full_gate")) {
      AddFailure(&failures, "stage17_audit_matrix_row_missing_or_wrong");
    }
  } else {
    AddFailure(&failures, "cannot_read_audit_matrix");
  }

  std::string trace;
  if (ReadFile(args.execution_plan_root / "IMPLEMENTATION_TRACE.md", &trace)) {
    if (!Contains(trace, "## Stage 16 validation evidence")) {
      AddFailure(&failures, "missing_stage16_validation_evidence");
    }
    if (!Contains(trace, "## Stage 17 implementation")) {
      AddFailure(&failures, "missing_stage17_implementation_trace");
    }
  } else {
    AddFailure(&failures, "cannot_read_implementation_trace");
  }

  std::string status;
  if (ReadFile(args.execution_plan_root / "SBSQL_V3_FINAL_IMPLEMENTATION_STATUS.yaml", &status)) {
    if (!Contains(status, "status: implemented_pending_validation") &&
        !Contains(status, "status: validated")) {
      AddFailure(&failures, "final_status_not_closed");
    }
    if (!Contains(status, "validation_gate: sb_sbsql_v3_full_gate")) {
      AddFailure(&failures, "final_status_missing_gate");
    }
  } else {
    AddFailure(&failures, "cannot_read_final_status");
  }

  const bool ok = failures.empty();
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
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

