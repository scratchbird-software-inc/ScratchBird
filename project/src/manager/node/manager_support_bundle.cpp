// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_SUPPORT_BUNDLE

#include "manager_support_bundle.hpp"

#include "manager_protocol.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace scratchbird::manager::node {
namespace {

namespace proto = scratchbird::manager::protocol;

std::string BoolText(bool value) { return value ? "true" : "false"; }

bool WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

bool SensitiveKeyAt(const std::string& text, std::size_t pos, const char* key) {
  std::size_t i = 0;
  while (key[i] != '\0') {
    if (pos + i >= text.size()) return false;
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(text[pos + i]))) != key[i]) {
      return false;
    }
    ++i;
  }
  if (pos > 0) {
    const unsigned char previous = static_cast<unsigned char>(text[pos - 1]);
    if (std::isalnum(previous) || text[pos - 1] == '_' || text[pos - 1] == '-') return false;
  }
  const std::size_t after = pos + i;
  return after < text.size() && (text[after] == '=' || text[after] == ':' || text[after] == '"');
}

bool SensitiveDelimiter(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == ',' || ch == ';' || ch == '&' || ch == '"' || ch == '\'';
}

std::string RedactManagerSupportBundleText(std::string text) {
  static constexpr const char* kSensitiveKeys[] = {
      "password",
      "passwd",
      "secret",
      "token",
      "private_key",
      "credential",
      "verifier",
      "encryption_key",
      "decryption_key",
      "key_handle"};
  for (std::size_t pos = 0; pos < text.size(); ++pos) {
    for (const char* key : kSensitiveKeys) {
      if (!SensitiveKeyAt(text, pos, key)) continue;
      std::size_t value_start = pos;
      while (value_start < text.size() && text[value_start] != '=' &&
             text[value_start] != ':' && text[value_start] != '"') {
        ++value_start;
      }
      if (value_start >= text.size()) continue;
      ++value_start;
      std::size_t value_end = value_start;
      while (value_end < text.size() && !SensitiveDelimiter(text[value_end])) {
        ++value_end;
      }
      text.replace(value_start, value_end - value_start, "[redacted]");
      pos = value_start + 9;
      break;
    }
  }
  for (std::size_t pos = 0; pos < text.size(); ++pos) {
    if (text[pos] != '/') { continue; }
    std::size_t value_end = pos;
    while (value_end < text.size() && !SensitiveDelimiter(text[value_end])) {
      ++value_end;
    }
    text.replace(pos, value_end - pos, "[path-redacted]");
    pos += 14;
  }
  return text;
}

bool CopyRedactedIfExists(const std::filesystem::path& source, const std::filesystem::path& target) {
  if (!std::filesystem::exists(source)) return true;
  std::ifstream in(source);
  std::ofstream out(target, std::ios::trunc);
  if (!in || !out) return false;
  std::string line;
  while (std::getline(in, line)) {
    out << RedactManagerSupportBundleText(std::move(line)) << '\n';
  }
  return static_cast<bool>(out);
}

}  // namespace

bool GenerateManagerSupportBundle(const ManagerConfig& config,
                                  const SupportBundleInputs& inputs,
                                  std::string* error_code) {
  std::error_code ec;
  std::filesystem::create_directories(inputs.bundle_dir, ec);
  if (ec) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_FAILED";
    return false;
  }

  std::ostringstream manifest;
  manifest << "format=SBMN_MANAGER_SUPPORT_BUNDLE_V1\n";
  manifest << "created_ms=" << proto::CurrentEpochMilliseconds() << "\n";
  manifest << "scope=" << inputs.scope << "\n";
  manifest << "redaction_profile=" << inputs.redaction_profile << "\n";
  manifest << "control_dir=[path-redacted]\n";
  manifest << "runtime_dir=[path-redacted]\n";
  manifest << "audit_file=[path-redacted]\n";
  manifest << "metrics_file=[path-redacted]\n";
  manifest << "authority_path=engine.authorization.management.SUPPORT_EXPORT\n";
  manifest << "excluded_protected_material=password,secret,token,private_key,credential,verifier,encryption_key,decryption_key,key_handle\n";
  manifest << "local_path_policy=redacted\n";
  if (!WriteText(inputs.bundle_dir / "manifest.txt", manifest.str())) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_FAILED";
    return false;
  }
  if (!WriteText(inputs.bundle_dir / "status.json", inputs.status_json)) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_FAILED";
    return false;
  }
  if (!WriteText(inputs.bundle_dir / "metrics.json", inputs.metrics_json)) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_FAILED";
    return false;
  }
  if (!inputs.agent_observability_json.empty() &&
      !WriteText(inputs.bundle_dir / "agent-observability.json",
                 RedactManagerSupportBundleText(inputs.agent_observability_json))) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_FAILED";
    return false;
  }

  std::ostringstream config_summary;
  config_summary << "proxy_enabled=" << BoolText(config.proxy_enabled) << "\n";
  config_summary << "bind_address=" << config.bind_address << "\n";
  config_summary << "proxy_port=" << config.proxy_port << "\n";
  config_summary << "native_bind=" << (config.native_bind.empty() ? "" : "[path-redacted]") << "\n";
  config_summary << "native_port=" << config.native_port << "\n";
  config_summary << "owner_database_name=" << config.owner_database_name << "\n";
  config_summary << "owner_database_uuid_set=" << BoolText(config.owner_database_uuid_set) << "\n";
  config_summary << "listener_id=" << config.listener_id << "\n";
  config_summary << "listener_control_socket_dir=[path-redacted]\n";
  config_summary << "dbbt_keyring_path=" << (config.dbbt_keyring_path.empty() ? "" : "<redacted-path-present>") << "\n";
  config_summary << "mcp_secret_ref=" << (config.mcp_secret_ref.empty() ? "" : "<redacted-secret-ref-present>") << "\n";
  config_summary << "mcp_secret_rights_configured=" << BoolText(!config.mcp_secret_rights.empty()) << "\n";
  config_summary << "restart_enabled=" << BoolText(config.restart_enabled) << "\n";
  config_summary << "restart_executable=" << (config.restart_executable.empty() ? "" : "<redacted-path-present>") << "\n";
  if (!WriteText(inputs.bundle_dir / "config-redacted.txt", config_summary.str())) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_FAILED";
    return false;
  }

  if (!CopyRedactedIfExists(inputs.lifecycle_state_file, inputs.bundle_dir / "lifecycle.state") ||
      !CopyRedactedIfExists(inputs.lifecycle_journal_file, inputs.bundle_dir / "lifecycle.journal") ||
      !CopyRedactedIfExists(inputs.audit_file, inputs.bundle_dir / "audit.jsonl")) {
    if (error_code) *error_code = "MANAGER.SUPPORT_BUNDLE_REDACTION_FAILED";
    return false;
  }
  return true;
}

}  // namespace scratchbird::manager::node
