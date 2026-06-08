// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"

#include <algorithm>
#include <cctype>

namespace scratchbird::core::platform {
namespace {

bool ContainsInsensitive(const std::string& value, const std::string& needle) {
  auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
  std::string haystack = value;
  std::string target = needle;
  std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
  std::transform(target.begin(), target.end(), target.begin(), lower);
  return haystack.find(target) != std::string::npos;
}

bool IsCanonicalDiagnosticCodeChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}

void AddDiagnosticValidationFailure(DiagnosticValidationResult* result, std::string key, std::string value) {
  result->failures.push_back({std::move(key), std::move(value)});
}

}  // namespace

CompileFeatureGates CurrentCompileFeatureGates() {
  CompileFeatureGates gates;
  gates.cxx17_or_newer = __cplusplus >= 201703L;
  gates.compiler_int128 = kCompilerHasInt128;
  gates.endian_known = true;

#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
  gates.exceptions_enabled = true;
#else
  gates.exceptions_enabled = false;
#endif

#if defined(__GXX_RTTI) || defined(_CPPRTTI)
  gates.rtti_enabled = true;
#else
  gates.rtti_enabled = false;
#endif

  return gates;
}

Status CheckCompileFeatureGates() {
  const CompileFeatureGates gates = CurrentCompileFeatureGates();
  if (!gates.cxx17_or_newer || !gates.compiler_int128 || !gates.endian_known) {
    return {StatusCode::platform_compile_gate_failed, Severity::fatal, Subsystem::platform};
  }
  return {StatusCode::ok, Severity::info, Subsystem::platform};
}

const char* SeverityName(Severity severity) {
  switch (severity) {
    case Severity::trace: return "trace";
    case Severity::info: return "info";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    case Severity::fatal: return "fatal";
  }
  return "unknown";
}

const char* SubsystemName(Subsystem subsystem) {
  switch (subsystem) {
    case Subsystem::platform: return "platform";
    case Subsystem::diagnostics: return "diagnostics";
    case Subsystem::uuid: return "uuid";
    case Subsystem::time: return "time";
    case Subsystem::memory: return "memory";
    case Subsystem::storage_disk: return "storage_disk";
    case Subsystem::storage_page: return "storage_page";
    case Subsystem::datatypes: return "datatypes";
    case Subsystem::transaction_mga: return "transaction_mga";
    case Subsystem::catalog: return "catalog";
    case Subsystem::engine: return "engine";
    case Subsystem::parser: return "parser";
    case Subsystem::cluster_private: return "cluster_private";
  }
  return "unknown";
}

const char* StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::ok: return "ok";
    case StatusCode::platform_compile_gate_failed: return "platform_compile_gate_failed";
    case StatusCode::platform_unknown_endian: return "platform_unknown_endian";
    case StatusCode::platform_required_feature_missing: return "platform_required_feature_missing";
    case StatusCode::diagnostic_invalid_record: return "diagnostic_invalid_record";
    case StatusCode::uuid_invalid: return "uuid_invalid";
    case StatusCode::time_source_unavailable: return "time_source_unavailable";
    case StatusCode::memory_invalid_request: return "memory_invalid_request";
    case StatusCode::memory_allocation_failed: return "memory_allocation_failed";
    case StatusCode::memory_limit_exceeded: return "memory_limit_exceeded";
    case StatusCode::memory_unknown_pointer: return "memory_unknown_pointer";
  }
  return "unknown";
}

DiagnosticRecord MakeDiagnostic(StatusCode code,
                                Severity severity,
                                Subsystem subsystem,
                                std::string diagnostic_code,
                                std::string message_key,
                                std::vector<DiagnosticArgument> arguments,
                                std::string trace_id,
                                std::string source_component,
                                std::string remediation_hint) {
  DiagnosticRecord record;
  record.status = {code, severity, subsystem};
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  record.arguments = std::move(arguments);
  record.trace_id = std::move(trace_id);
  record.source_component = std::move(source_component);
  record.remediation_hint = std::move(remediation_hint);
  return record;
}

bool DiagnosticCodeLooksCanonical(const std::string& diagnostic_code) {
  if (diagnostic_code.empty()) { return false; }
  if (diagnostic_code == "OK" || diagnostic_code == "SB_OK") { return false; }
  bool has_letter = false;
  bool has_separator = false;
  for (char c : diagnostic_code) {
    if (!IsCanonicalDiagnosticCodeChar(c)) { return false; }
    if (std::isalpha(static_cast<unsigned char>(c))) { has_letter = true; }
    if (c == '-' || c == '_' || c == '.') { has_separator = true; }
  }
  return has_letter && has_separator;
}

bool DiagnosticMessageKeyLooksCanonical(const std::string& message_key) {
  if (message_key.empty()) { return false; }
  bool has_dot = false;
  for (char c : message_key) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) { return false; }
    if (c == '.') { has_dot = true; }
  }
  return has_dot;
}

bool DiagnosticRecordHasPlaceholderText(const DiagnosticRecord& record) {
  const std::string combined = record.diagnostic_code + " " + record.message_key + " " + record.source_component +
                               " " + record.remediation_hint;
  return ContainsInsensitive(combined, "placeholder") ||
         ContainsInsensitive(combined, "stub") ||
         ContainsInsensitive(combined, "todo") ||
         ContainsInsensitive(combined, "fixme") ||
         ContainsInsensitive(combined, "generic_error") ||
         ContainsInsensitive(combined, "unknown_error");
}

DiagnosticValidationResult ValidateDiagnosticRecord(const DiagnosticRecord& record) {
  DiagnosticValidationResult result;
  result.status = {StatusCode::ok, Severity::info, Subsystem::diagnostics};
  result.diagnostic = record;

  if (!DiagnosticCodeLooksCanonical(record.diagnostic_code)) {
    AddDiagnosticValidationFailure(&result, "diagnostic_code", "not_canonical");
  }
  if (!DiagnosticMessageKeyLooksCanonical(record.message_key)) {
    AddDiagnosticValidationFailure(&result, "message_key", "not_canonical");
  }
  if (record.source_component.empty()) {
    AddDiagnosticValidationFailure(&result, "source_component", "required");
  }
  if (record.status.code != StatusCode::ok && record.remediation_hint.empty()) {
    AddDiagnosticValidationFailure(&result, "remediation_hint", "required_for_non_ok_status");
  }
  if (record.status.code == StatusCode::ok && record.status.severity != Severity::info &&
      record.status.severity != Severity::trace) {
    AddDiagnosticValidationFailure(&result, "severity", "ok_status_must_be_info_or_trace");
  }
  if (record.status.code != StatusCode::ok && record.status.severity == Severity::info) {
    AddDiagnosticValidationFailure(&result, "severity", "non_ok_status_must_not_be_info");
  }
  if (DiagnosticRecordHasPlaceholderText(record)) {
    AddDiagnosticValidationFailure(&result, "placeholder_text", "forbidden");
  }

  if (!result.failures.empty()) {
    result.status = {StatusCode::diagnostic_invalid_record, Severity::error, Subsystem::diagnostics};
    result.diagnostic = MakeDiagnostic(StatusCode::diagnostic_invalid_record,
                                       Severity::error,
                                       Subsystem::diagnostics,
                                       "SB-DIAGNOSTIC-INVALID-RECORD",
                                       "diagnostics.record.invalid",
                                       result.failures,
                                       {},
                                       "core.platform.diagnostics",
                                       "Fix the diagnostic record so it has a canonical code, canonical message key, source component, and remediation hint when non-OK.");
  }
  return result;
}

}  // namespace scratchbird::core::platform
