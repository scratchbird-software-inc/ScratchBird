// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SB_DBLC_REPO_ROOT
#define SB_DBLC_REPO_ROOT ""
#endif

#ifndef SB_DBLC_BUILD_ROOT
#define SB_DBLC_BUILD_ROOT ""
#endif

#ifndef SB_DBLC_VALIDATION_PLAN
#define SB_DBLC_VALIDATION_PLAN ""
#endif

#ifndef SB_DBLC_REGRESSION_REPORT
#define SB_DBLC_REGRESSION_REPORT ""
#endif

#ifndef SB_DBLC_REGRESSION_MATRIX
#define SB_DBLC_REGRESSION_MATRIX ""
#endif

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "DBLC-016 exhaustive regression failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    Fail("missing required file: " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool Contains(std::string_view text, std::string_view token) {
  return text.find(token) != std::string_view::npos;
}

std::filesystem::path RepoRoot() {
  if (std::string_view(SB_DBLC_REPO_ROOT).size() != 0) {
    return std::filesystem::path(SB_DBLC_REPO_ROOT);
  }
  auto current = std::filesystem::current_path();
  for (;;) {
    if (std::filesystem::exists(
            current /
            "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
            "VALIDATION_PLAN.md")) {
      return current;
    }
    if (!current.has_parent_path() || current == current.parent_path()) {
      break;
    }
    current = current.parent_path();
  }
  Fail("repository root could not be located");
}

std::filesystem::path BuildRoot(const std::filesystem::path& repo_root) {
  if (std::string_view(SB_DBLC_BUILD_ROOT).size() != 0) {
    return std::filesystem::path(SB_DBLC_BUILD_ROOT);
  }
  return repo_root / "build";
}

std::filesystem::path ValidationPlan(const std::filesystem::path& repo_root) {
  if (std::string_view(SB_DBLC_VALIDATION_PLAN).size() != 0) {
    return std::filesystem::path(SB_DBLC_VALIDATION_PLAN);
  }
  return repo_root /
         "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
         "VALIDATION_PLAN.md";
}

std::filesystem::path RegressionReport(const std::filesystem::path& repo_root) {
  if (std::string_view(SB_DBLC_REGRESSION_REPORT).size() != 0) {
    return std::filesystem::path(SB_DBLC_REGRESSION_REPORT);
  }
  return repo_root /
         "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
         "artifacts/DATABASE_LIFECYCLE_REGRESSION_REPORT.md";
}

std::filesystem::path RegressionMatrix(const std::filesystem::path& repo_root) {
  if (std::string_view(SB_DBLC_REGRESSION_MATRIX).size() != 0) {
    return std::filesystem::path(SB_DBLC_REGRESSION_MATRIX);
  }
  return repo_root /
         "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
         "artifacts/DATABASE_LIFECYCLE_REGRESSION_MATRIX.csv";
}

std::vector<std::string> RequiredLifecycleLabels(std::string_view validation_plan) {
  const auto gate_pos = validation_plan.find("## Gate Commands");
  const std::string required_table =
      std::string(validation_plan.substr(0, gate_pos == std::string_view::npos
                                                ? validation_plan.size()
                                                : gate_pos));
  std::regex row_re(R"(\| `([^`]+)` \|)");
  std::vector<std::string> labels;
  for (std::sregex_iterator it(required_table.begin(), required_table.end(), row_re), end;
       it != end;
       ++it) {
    labels.push_back((*it)[1].str());
  }
  if (labels.empty()) {
    Fail("VALIDATION_PLAN required CTest label table was not parsed");
  }
  return labels;
}

std::set<std::string> ScanCtestLabels(const std::filesystem::path& build_root) {
  if (!std::filesystem::exists(build_root)) {
    Fail("build root does not exist: " + build_root.string());
  }
  std::set<std::string> labels;
  std::regex labels_re("LABELS \"([^\"]*)\"");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(build_root)) {
    if (!entry.is_regular_file() || entry.path().filename() != "CTestTestfile.cmake") {
      continue;
    }
    const auto text = ReadText(entry.path());
    for (std::sregex_iterator it(text.begin(), text.end(), labels_re), end; it != end; ++it) {
      std::string packed = (*it)[1].str();
      std::string item;
      std::istringstream input(packed);
      while (std::getline(input, item, ';')) {
        if (!item.empty()) {
          labels.insert(item);
        }
      }
    }
  }
  if (labels.empty()) {
    Fail("no generated CTest labels were found under: " + build_root.string());
  }
  return labels;
}

void RequireTokens(std::string_view name,
                   std::string_view text,
                   const std::vector<std::string_view>& tokens) {
  for (const auto token : tokens) {
    if (!Contains(text, token)) {
      Fail(std::string(name) + " missing required token: " + std::string(token));
    }
  }
}

void RequireLabels(const std::set<std::string>& actual,
                   const std::vector<std::string>& expected,
                   std::string_view context) {
  for (const auto& label : expected) {
    if (!actual.contains(label)) {
      Fail(std::string(context) + " missing generated CTest label: " + label);
    }
  }
}

}  // namespace

int main() {
  const auto repo_root = RepoRoot();
  const auto validation_plan = ReadText(ValidationPlan(repo_root));
  const auto report = ReadText(RegressionReport(repo_root));
  const auto matrix = ReadText(RegressionMatrix(repo_root));
  const auto ctest_labels = ScanCtestLabels(BuildRoot(repo_root));

  const auto required_lifecycle_labels = RequiredLifecycleLabels(validation_plan);
  RequireLabels(ctest_labels, required_lifecycle_labels, "VALIDATION_PLAN");

  RequireLabels(ctest_labels,
                {
                    "database_lifecycle_exhaustive",
                    "database_lifecycle",
                    "DBLC_P16_REGRESSION_COMPLETE",
                    "DBLC_STATIC_REGRESSION_REPORT_ARTIFACT",
                    "database_lifecycle_fault_injection",
                    "database_lifecycle_release",
                    "mga_transaction_regression",
                    "sbsql_parser_worker",
                    "database_lifecycle_parser_route",
                    "database_lifecycle_donor_mapping",
                    "DBLC_P14_DONOR_MAPPING_COMPLETE",
                    "DBLC_P15_OBSERVABILITY_COMPLETE",
                },
                "DBLC-016 evidence");

  RequireTokens("DATABASE_LIFECYCLE_REGRESSION_REPORT.md",
                report,
                {
                    "DBLC_P16_REGRESSION_COMPLETE",
                    "DATABASE-LIFECYCLE-VALIDATION-PLAN",
                    "database_lifecycle_exhaustive",
                    "DBLC_STATIC_REGRESSION_REPORT_ARTIFACT",
                    "lifecycle_operation_core",
                    "lifecycle_state_transition_core",
                    "lifecycle_invalid_transition_core",
                    "lifecycle_route_core",
                    "policy_override_no_override",
                    "policy_override_create_database_only",
                    "policy_override_security_admin",
                    "policy_override_sysarch",
                    "policy_override_policy_defined",
                    "policy_override_cluster_only",
                    "security_valid_credentials",
                    "security_invalid_credentials",
                    "auth_authority_engine_owned",
                    "resource_seed_epoch_coverage",
                    "diagnostic_message_vector_coverage",
                    "observability_metrics_audit_coverage",
                    "donor_mapping_firebird_sbsql",
                    "sbsql_full_route_coverage",
                    "mga_transaction_regression",
                    "no_authoritative_wal_recovery",
                    "no_parser_finality_authority",
                    "no_donor_sql_execution",
                    "evidence_report_present",
                });

  RequireTokens("DATABASE_LIFECYCLE_REGRESSION_MATRIX.csv",
                matrix,
                {
                    "coverage_id,coverage_class,required_surface,evidence_labels,evidence_artifacts,status",
                    "DBLC-P16-LABEL-database_lifecycle_exhaustive",
                    "DBLC-P16-CORE-lifecycle_operation_core",
                    "DBLC-P16-POLICY-policy_override_cluster_only",
                    "DBLC-P16-SECURITY-auth_authority_engine_owned",
                    "DBLC-P16-OBSERVABILITY-observability_metrics_audit_coverage",
                    "DBLC-P16-ROUTE-sbsql_full_route_coverage",
                    "DBLC-P16-MGA-no_authoritative_wal_recovery",
                    "DBLC-P16-REPORT-evidence_report_present",
                });

  for (const auto& label : required_lifecycle_labels) {
    if (!Contains(report, label)) {
      Fail("regression report does not mention required lifecycle label: " + label);
    }
    if (!Contains(matrix, "DBLC-P16-LABEL-" + label)) {
      Fail("regression matrix lacks required lifecycle label row: " + label);
    }
  }

  std::cout << "DBLC_P16_REGRESSION_COMPLETE=ctest-label-and-artifact-audit-passed\n";
  std::cout << "database_lifecycle_exhaustive=passed\n";
  return EXIT_SUCCESS;
}
