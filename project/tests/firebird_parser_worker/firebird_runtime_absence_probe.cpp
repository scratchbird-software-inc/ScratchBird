// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_package_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

struct Case {
  std::string_view package_state;
  std::string_view operation_class;
  bool ok;
  std::string_view expected_behavior;
  std::string_view diagnostic_code;
};

bool Probe(const Case& test) {
  const auto result =
      scratchbird::parser::firebird::EvaluateFirebirdPackageAdmission(
          test.package_state, test.operation_class);
  if (result.ok != test.ok) {
    std::cerr << "admission ok mismatch for " << test.package_state
              << ": " << result.json << '\n';
    return false;
  }
  if (!Contains(result.json, test.expected_behavior)) {
    std::cerr << "admission behavior mismatch for " << test.package_state
              << ": " << result.json << '\n';
    return false;
  }
  if (!test.ok && !Contains(result.message_vector_json, test.diagnostic_code)) {
    std::cerr << "admission diagnostic mismatch for " << test.package_state
              << ": " << result.message_vector_json << '\n';
    return false;
  }
  if (test.ok && !result.diagnostic_code.empty()) {
    std::cerr << "successful admission carried diagnostic code\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  const Case cases[] = {
      {"parser_not_installed", "client_attach", false,
       "deterministic_parser_missing_diagnostic",
       "FIREBIRD.PACKAGE.PARSER_MISSING"},
      {"parser_profile_disabled", "client_attach", false,
       "deterministic_parser_disabled_diagnostic",
       "FIREBIRD.PACKAGE.PARSER_DISABLED"},
      {"udr_not_installed", "dynamic_sql_or_environment_install", false,
       "deterministic_parser_support_udr_required_diagnostic",
       "FIREBIRD.PACKAGE.UDR_MISSING"},
      {"udr_version_mismatch", "dynamic_sql_or_environment_install", false,
       "deterministic_parser_support_udr_rejected_diagnostic",
       "FIREBIRD.PACKAGE.UDR_VERSION_MISMATCH"},
      {"udr_signature_or_policy_invalid", "dynamic_sql_or_environment_install", false,
       "deterministic_parser_support_udr_rejected_diagnostic",
       "FIREBIRD.PACKAGE.UDR_POLICY_REJECTED"},
      {"package_policy_denied", "any_firebird_operation", false,
       "deterministic_policy_diagnostic",
       "FIREBIRD.PACKAGE.POLICY_DENIED"},
      {"no_parser_or_udr_installed", "core_startup_and_other_dialects", true,
       "core_and_other_dialects_continue", ""},
  };

  for (const auto& test : cases) {
    if (!Probe(test)) return EXIT_FAILURE;
  }

  const auto unknown =
      scratchbird::parser::firebird::EvaluateFirebirdPackageAdmission(
          "unknown_state", "client_attach");
  if (unknown.ok || !Contains(unknown.message_vector_json,
                              "FIREBIRD.PACKAGE.STATE_UNKNOWN")) {
    std::cerr << "unknown package state did not fail closed: "
              << unknown.json << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
