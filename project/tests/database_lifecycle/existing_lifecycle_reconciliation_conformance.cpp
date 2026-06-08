// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifndef SB_DBLC_RECONCILIATION_AUDIT
#define SB_DBLC_RECONCILIATION_AUDIT ""
#endif

#ifndef SB_DBLC_RECONCILIATION_REPORT
#define SB_DBLC_RECONCILIATION_REPORT ""
#endif

#ifndef SB_DBLC_REPO_ROOT
#define SB_DBLC_REPO_ROOT ""
#endif

#ifndef SB_DBLC_PYTHON_EXECUTABLE
#define SB_DBLC_PYTHON_EXECUTABLE "python3"
#endif

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::string ShellQuote(const std::filesystem::path& path) {
  std::string input = path.string();
  std::string out = "'";
  for (char c : input) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

std::filesystem::path FindRepoRoot() {
  if (std::string_view(SB_DBLC_REPO_ROOT).size() != 0) {
    return std::filesystem::path(SB_DBLC_REPO_ROOT);
  }

  std::filesystem::path current = std::filesystem::current_path();
  for (;;) {
    const auto candidate =
        current / "project/tools/database_lifecycle/lifecycle_reconciliation_audit.py";
    if (std::filesystem::exists(candidate)) {
      return current;
    }
    if (!current.has_parent_path() || current == current.parent_path()) {
      break;
    }
    current = current.parent_path();
  }
  Fail("DBLC-013N repository root could not be located");
}

std::filesystem::path AuditScript(const std::filesystem::path& repo_root) {
  if (std::string_view(SB_DBLC_RECONCILIATION_AUDIT).size() != 0) {
    return std::filesystem::path(SB_DBLC_RECONCILIATION_AUDIT);
  }
  return repo_root / "project/tools/database_lifecycle/lifecycle_reconciliation_audit.py";
}

}  // namespace

int main() {
  const auto repo_root = FindRepoRoot();
  const auto audit = AuditScript(repo_root);
  if (!std::filesystem::is_regular_file(audit)) {
    Fail("DBLC-013N reconciliation audit script is missing");
  }

  const auto report = std::string_view(SB_DBLC_RECONCILIATION_REPORT).size() != 0
                          ? std::filesystem::path(SB_DBLC_RECONCILIATION_REPORT)
                          : repo_root /
                                "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
                                "artifacts/DATABASE_LIFECYCLE_RECONCILIATION_REPORT.md";

  const std::string command = ShellQuote(std::filesystem::path(SB_DBLC_PYTHON_EXECUTABLE)) +
                              " " + ShellQuote(audit) + " --repo-root " +
                              ShellQuote(repo_root) + " --report " + ShellQuote(report);
  const int rc = std::system(command.c_str());
  if (rc != 0) {
    std::cerr << "DBLC-013N reconciliation audit failed with status " << rc << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "database_lifecycle_existing_reconciliation=passed\n";
  return EXIT_SUCCESS;
}
