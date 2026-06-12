// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compatibility_worker_session.hpp"

#include <cerrno>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::compatibility {
namespace {

std::string FailJson(std::string_view code, const DialectProfile& profile) {
  return "{\"ok\":false,\"dialect\":\"" + EscapeJson(profile.dialect_id) +
         "\",\"diagnostic_code\":\"" + std::string(code) +
         "\",\"runtime_policy\":\"fail_closed\"}";
}

std::string ProjectTestsRoot(const DialectProfile& profile) {
  return "project/tests/reference_regression/" + std::string(profile.dialect_id);
}

bool KnownWireTranscriptFamily(std::string_view family) {
  return family == "connect_auth_startup" ||
         family == "prepare_bind_describe_execute_fetch_cancel_close" ||
         family == "error_warning_notice_status";
}

std::string WireTranscriptOracleJson(std::string_view family,
                                     const DialectProfile& profile) {
  return "{\"ok\":true,\"dialect\":\"" + EscapeJson(profile.dialect_id) +
         "\",\"transcript_family\":\"" + EscapeJson(family) +
         "\",\"parser_wire_owner\":true,"
         "\"listener_boundary\":\"handoff_only\","
         "\"auth_authority\":\"scratchbird_engine\","
         "\"authorization_authority\":\"scratchbird_engine\","
         "\"normalizes_secrets\":true,"
         "\"normalizes_nondeterminism\":true,"
         "\"negative_cases_registered\":true,"
         "\"compatibility_sql_executed\":false,"
         "\"compatibility_storage_authority\":false,"
         "\"compatibility_recovery_authority\":false,"
         "\"reference_engine_sql_executed\":false,"
         "\"reference_storage_authority\":false,"
         "\"reference_recovery_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"proof_path\":\"" +
         ProjectTestsRoot(profile) + "/wire_transcripts/" + std::string(family) +
         "/\"}";
}

std::string ResourceLimitJson(const DialectProfile& profile) {
  return "{\"ok\":true,\"dialect\":\"" + EscapeJson(profile.dialect_id) +
         "\",\"max_statement_bytes\":1048576,"
         "\"max_message_bytes\":1048576,"
         "\"parse_timeout_ms\":30000,"
         "\"worker_idle_timeout_ms\":300000,"
         "\"cancellation_token_authority\":\"scratchbird_engine\","
         "\"backpressure_policy\":\"fail_closed\","
         "\"malformed_input_policy\":\"diagnostic_no_reference_effect\","
         "\"drain_behavior\":\"finish_current_or_abort_before_handoff\","
         "\"mga_transaction_authority\":\"scratchbird_engine\","
         "\"parser_transaction_finality_authority\":false,"
         "\"compatibility_storage_authority\":false,"
         "\"reference_storage_authority\":false}";
}

std::string RegressionManifestJson(const DialectProfile& profile) {
  const auto root = ProjectTestsRoot(profile);
  return "{\"ok\":true,\"dialect\":\"" + EscapeJson(profile.dialect_id) +
         "\",\"project_tests_root\":\"" + root + "\","
         "\"proof_location\":\"project/tests\","
         "\"file_presence_is_completion\":false,"
         "\"generated_only_completion\":false,"
         "\"runtime_behavior_required\":true,"
         "\"required_manifests\":["
         "\"" + root + "/upstream_manifest.csv\","
         "\"" + root + "/fixtures/fixture_manifest.csv\","
         "\"" + root + "/goldens/golden_manifest.csv\","
         "\"" + root + "/management_package_abi/management_package_abi_manifest.csv\","
         "\"" + root + "/wire_transcripts/wire_transcript_manifest.csv\","
         "\"" + root + "/resource_limits/resource_limit_manifest.csv\","
         "\"" + root + "/release_evidence/release_evidence_manifest.csv\","
         "\"" + root + "/enterprise_completion/enterprise_completion_manifest.csv\"]}";
}

#ifndef _WIN32
bool WriteAll(int fd, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const auto rc = ::write(fd, text.data() + written, text.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool ReadLine(int fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
    const auto rc = ::read(fd, &ch, 1);
    if (rc == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') line->push_back(ch);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return !line->empty();
  }
}
#endif

std::string AfterCommand(std::string_view line, std::string_view command) {
  const auto trimmed = TrimAscii(line);
  if (trimmed.size() <= command.size()) return {};
  return TrimAscii(std::string_view(trimmed).substr(command.size()));
}

} // namespace

std::string HandleWorkerCommand(std::string_view line,
                                const DialectProfile& profile,
                                bool* close) {
  const auto trimmed = TrimAscii(line);
  const auto upper = ToUpperAscii(trimmed);
  if (upper.empty()) return "OK EMPTY\n";
  if (upper == "QUIT" || upper == "EXIT") {
    *close = true;
    return "OK BYE\n";
  }
  if (upper == "PING") return "OK PONG " + std::string(profile.dialect_id) + "\n";
  if (upper == "PACKAGE_IDENTITY") {
    return "PACKAGE " + PackageIdentityJson(profile) + "\n";
  }
  if (upper == "SURFACE_REPORT") {
    return "SURFACE " + SurfaceReportJson(profile) + "\n";
  }
  if (upper == "CONNECTION_SANDBOX_REPORT") {
    return "SANDBOX " + ConnectionSandboxReportJson(profile) + "\n";
  }
  if (upper == "DIALECT_VARIANT_REPORT") {
    return "VARIANTS " + DialectVariantReportJson(profile) + "\n";
  }
  if (upper.starts_with("WIRE_TRANSCRIPT ")) {
    const auto family = AfterCommand(trimmed, "WIRE_TRANSCRIPT");
    if (!KnownWireTranscriptFamily(family)) {
      return "ERROR " + FailJson(std::string(profile.diagnostic_prefix) +
                                     ".WORKER.WIRE_TRANSCRIPT_UNKNOWN",
                                 profile) + "\n";
    }
    return "WIRE " + WireTranscriptOracleJson(family, profile) + "\n";
  }
  if (upper == "RESOURCE_LIMIT_REPORT") {
    return "RESOURCE " + ResourceLimitJson(profile) + "\n";
  }
  if (upper == "REGRESSION_MANIFEST_REPORT") {
    return "REGRESSION " + RegressionManifestJson(profile) + "\n";
  }
  if (upper.starts_with("PARSE ")) {
    const auto result = ParseStatement(AfterCommand(trimmed, "PARSE"), profile);
    if (!result.ok) return "ERROR " + result.message_vector_json + "\n";
    return "SBLR " + result.sblr_envelope + "\n";
  }
  return "ERROR " + FailJson(std::string(profile.diagnostic_prefix) +
                                 ".WORKER.COMMAND_UNKNOWN",
                             profile) + "\n";
}

int ServeTextWorkerSession(int fd, const DialectProfile& profile) {
#ifdef _WIN32
  (void)fd;
  (void)profile;
  return 1;
#else
  std::string line;
  bool close = false;
  while (!close && ReadLine(fd, &line)) {
    if (!WriteAll(fd, HandleWorkerCommand(line, profile, &close))) return 1;
  }
  return 0;
#endif
}

} // namespace scratchbird::parser::compatibility
