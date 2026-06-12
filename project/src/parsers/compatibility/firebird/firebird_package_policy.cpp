// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_package_policy.hpp"

#include "firebird_dialect.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {
namespace {

std::string EscapeJsonLocal(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

PackageAdmissionResult AdmissionDiagnostic(std::string_view package_state,
                                           std::string_view operation_class,
                                           std::string_view behavior,
                                           std::string_view diagnostic_code,
                                           std::string_view message) {
  PackageAdmissionResult result;
  result.ok = false;
  result.package_state = std::string(package_state);
  result.operation_class = std::string(operation_class);
  result.behavior = std::string(behavior);
  result.diagnostic_code = std::string(diagnostic_code);
  result.message_vector_json = MessageVectorToJson({
      {std::string(diagnostic_code), "ERROR", std::string(message),
       "sbl_firebird_package_policy",
       {{"package_state", std::string(package_state)},
        {"operation_class", std::string(operation_class)},
        {"expected_behavior", std::string(behavior)}}},
  });
  result.json = "{\"ok\":false,\"package_state\":\"" +
                EscapeJsonLocal(result.package_state) +
                "\",\"operation_class\":\"" +
                EscapeJsonLocal(result.operation_class) +
                "\",\"expected_behavior\":\"" +
                EscapeJsonLocal(result.behavior) +
                "\",\"diagnostic_code\":\"" +
                EscapeJsonLocal(result.diagnostic_code) +
                "\",\"runtime_policy\":\"fail_closed_package_admission\"}";
  return result;
}

PackageAdmissionResult AdmissionOk(std::string_view package_state,
                                   std::string_view operation_class,
                                   std::string_view behavior) {
  PackageAdmissionResult result;
  result.ok = true;
  result.package_state = std::string(package_state);
  result.operation_class = std::string(operation_class);
  result.behavior = std::string(behavior);
  result.message_vector_json = MessageVectorToJson({});
  result.json = "{\"ok\":true,\"package_state\":\"" +
                EscapeJsonLocal(result.package_state) +
                "\",\"operation_class\":\"" +
                EscapeJsonLocal(result.operation_class) +
                "\",\"expected_behavior\":\"" +
                EscapeJsonLocal(result.behavior) +
                "\",\"runtime_policy\":\"core_continues_without_firebird_package\"}";
  return result;
}

} // namespace

PackageAdmissionResult EvaluateFirebirdPackageAdmission(
    std::string_view package_state,
    std::string_view operation_class) {
  if (package_state == "parser_not_installed") {
    return AdmissionDiagnostic(
        package_state, operation_class,
        "deterministic_parser_missing_diagnostic",
        "FIREBIRD.PACKAGE.PARSER_MISSING",
        "Firebird parser package is not installed for this listener profile.");
  }
  if (package_state == "parser_profile_disabled") {
    return AdmissionDiagnostic(
        package_state, operation_class,
        "deterministic_parser_disabled_diagnostic",
        "FIREBIRD.PACKAGE.PARSER_DISABLED",
        "Firebird parser profile is disabled by package policy.");
  }
  if (package_state == "udr_not_installed") {
    return AdmissionDiagnostic(
        package_state, operation_class,
        "deterministic_parser_support_udr_required_diagnostic",
        "FIREBIRD.PACKAGE.UDR_MISSING",
        "Firebird parser-support UDR package is required for this operation.");
  }
  if (package_state == "udr_version_mismatch") {
    return AdmissionDiagnostic(
        package_state, operation_class,
        "deterministic_parser_support_udr_rejected_diagnostic",
        "FIREBIRD.PACKAGE.UDR_VERSION_MISMATCH",
        "Firebird parser-support UDR package version does not match the parser profile.");
  }
  if (package_state == "udr_signature_or_policy_invalid") {
    return AdmissionDiagnostic(
        package_state, operation_class,
        "deterministic_parser_support_udr_rejected_diagnostic",
        "FIREBIRD.PACKAGE.UDR_POLICY_REJECTED",
        "Firebird parser-support UDR package failed signature or policy admission.");
  }
  if (package_state == "package_policy_denied") {
    return AdmissionDiagnostic(
        package_state, operation_class,
        "deterministic_policy_diagnostic",
        "FIREBIRD.PACKAGE.POLICY_DENIED",
        "Firebird package policy denied the requested operation.");
  }
  if (package_state == "no_parser_or_udr_installed" &&
      operation_class == "core_startup_and_other_dialects") {
    return AdmissionOk(package_state, operation_class,
                       "core_and_other_dialects_continue");
  }
  return AdmissionDiagnostic(
      package_state, operation_class,
      "deterministic_policy_diagnostic",
      "FIREBIRD.PACKAGE.STATE_UNKNOWN",
      "Firebird package state is not recognized by the admission policy.");
}

} // namespace scratchbird::parser::firebird
