// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_CORE_DIAGNOSTIC_PROBE_MAIN

#include "runtime_platform.hpp"

#include <iostream>
#include <string>

namespace {

using namespace scratchbird::core::platform;

const char* Bool(bool value) { return value ? "true" : "false"; }

bool FailsWith(const DiagnosticValidationResult& result, const std::string& key) {
  if (result.ok()) { return false; }
  for (const auto& failure : result.failures) {
    if (failure.key == key) { return true; }
  }
  return false;
}

}  // namespace

int main() {
  const auto good = MakeDiagnostic(StatusCode::memory_invalid_request,
                                   Severity::error,
                                   Subsystem::memory,
                                   "SB-MEMORY-INVALID-REQUEST",
                                   "memory.request.invalid",
                                   {{"operation", "probe"}},
                                   "trace-1",
                                   "core.memory.probe",
                                   "Correct the memory request and retry.");
  const auto ok = MakeDiagnostic(StatusCode::ok,
                                 Severity::info,
                                 Subsystem::diagnostics,
                                 "SB-DIAGNOSTIC-OK",
                                 "diagnostics.record.ok",
                                 {},
                                 {},
                                 "core.diagnostics.probe");
  const auto missing_remediation = MakeDiagnostic(StatusCode::memory_invalid_request,
                                                  Severity::error,
                                                  Subsystem::memory,
                                                  "SB-MEMORY-INVALID-REQUEST",
                                                  "memory.request.invalid",
                                                  {},
                                                  {},
                                                  "core.memory.probe");
  const auto placeholder = MakeDiagnostic(StatusCode::memory_invalid_request,
                                          Severity::error,
                                          Subsystem::memory,
                                          "SB-MEMORY-PLACEHOLDER",
                                          "memory.placeholder.invalid",
                                          {},
                                          {},
                                          "core.memory.probe",
                                          "placeholder remediation");
  const auto bad_code = MakeDiagnostic(StatusCode::memory_invalid_request,
                                       Severity::error,
                                       Subsystem::memory,
                                       "BAD CODE!",
                                       "memory.request.invalid",
                                       {},
                                       {},
                                       "core.memory.probe",
                                       "Correct the memory request and retry.");
  const auto bad_key = MakeDiagnostic(StatusCode::memory_invalid_request,
                                      Severity::error,
                                      Subsystem::memory,
                                      "SB-MEMORY-INVALID-REQUEST",
                                      "memory_request_invalid",
                                      {},
                                      {},
                                      "core.memory.probe",
                                      "Correct the memory request and retry.");
  const auto bad_source = MakeDiagnostic(StatusCode::memory_invalid_request,
                                         Severity::error,
                                         Subsystem::memory,
                                         "SB-MEMORY-INVALID-REQUEST",
                                         "memory.request.invalid",
                                         {},
                                         {},
                                         {},
                                         "Correct the memory request and retry.");

  const auto good_validation = ValidateDiagnosticRecord(good);
  const auto ok_validation = ValidateDiagnosticRecord(ok);
  const auto remediation_validation = ValidateDiagnosticRecord(missing_remediation);
  const auto placeholder_validation = ValidateDiagnosticRecord(placeholder);
  const auto code_validation = ValidateDiagnosticRecord(bad_code);
  const auto key_validation = ValidateDiagnosticRecord(bad_key);
  const auto source_validation = ValidateDiagnosticRecord(bad_source);

  const bool good_ok = good_validation.ok() && ok_validation.ok();
  const bool remediation_required = FailsWith(remediation_validation, "remediation_hint");
  const bool placeholder_rejected = FailsWith(placeholder_validation, "placeholder_text");
  const bool bad_code_rejected = FailsWith(code_validation, "diagnostic_code");
  const bool bad_key_rejected = FailsWith(key_validation, "message_key");
  const bool source_required = FailsWith(source_validation, "source_component");
  const bool helpers_ok = DiagnosticCodeLooksCanonical("SB-TEST-CODE") &&
                          DiagnosticCodeLooksCanonical("CAPABILITY.LLVM_MISSING") &&
                          DiagnosticMessageKeyLooksCanonical("test.message.key") &&
                          !DiagnosticRecordHasPlaceholderText(good);
  const bool all_ok = good_ok && remediation_required && placeholder_rejected && bad_code_rejected &&
                      bad_key_rejected && source_required && helpers_ok;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << Bool(all_ok) << ",\n";
  std::cout << "  \"good_ok\": " << Bool(good_ok) << ",\n";
  std::cout << "  \"remediation_required\": " << Bool(remediation_required) << ",\n";
  std::cout << "  \"placeholder_rejected\": " << Bool(placeholder_rejected) << ",\n";
  std::cout << "  \"bad_code_rejected\": " << Bool(bad_code_rejected) << ",\n";
  std::cout << "  \"bad_key_rejected\": " << Bool(bad_key_rejected) << ",\n";
  std::cout << "  \"source_required\": " << Bool(source_required) << ",\n";
  std::cout << "  \"helpers_ok\": " << Bool(helpers_ok) << "\n";
  std::cout << "}\n";
  return all_ok ? 0 : 1;
}
