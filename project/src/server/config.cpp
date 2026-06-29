// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_CONFIG

#include "config.hpp"

#include "memory.hpp"
#include "security/auth_provider_model.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::server {

namespace {

namespace memory = scratchbird::core::memory;
namespace engine_api = scratchbird::engine::internal_api;

struct ParsedConfig {
  std::map<std::string, std::string> values;
};

std::string Trim(std::string value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
  });
  return value;
}

std::string StripInlineComment(std::string line) {
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quoted && ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && (ch == '#' || ch == ';')) {
      if (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t') {
        return line.substr(0, i);
      }
    }
  }
  return line;
}

std::string Unquote(std::string value, const std::string& key, std::vector<ServerDiagnostic>* diagnostics) {
  value = Trim(value);
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return value;
  }
  std::string out;
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    const char ch = value[i];
    if (ch != '\\') {
      out += ch;
      continue;
    }
    if (i + 2 >= value.size()) {
      diagnostics->push_back({"CONFIG.MALFORMED",
                              "config.malformed",
                              ServerDiagnosticSeverity::kError,
                              "A quoted configuration string has an invalid escape sequence.",
                              {{"canonical_key", key}}});
      return {};
    }
    const char escaped = value[++i];
    switch (escaped) {
      case '\\':
        out += '\\';
        break;
      case '"':
        out += '"';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      default:
        diagnostics->push_back({"CONFIG.MALFORMED",
                                "config.malformed",
                                ServerDiagnosticSeverity::kError,
                                "A quoted configuration string has an unsupported escape sequence.",
                                {{"canonical_key", key}, {"escape", std::string(1, escaped)}}});
        return {};
    }
  }
  return out;
}

ServerDiagnostic ConfigDiagnostic(std::string code,
                                  std::string key,
                                  std::string safe,
                                  std::vector<ServerDiagnosticField> fields = {}) {
  return {std::move(code),
          std::move(key),
          ServerDiagnosticSeverity::kError,
          std::move(safe),
          std::move(fields)};
}

const std::set<std::string>& KnownKeys() {
  static const std::set<std::string> keys{
      "config.format",
      "server.mode",
      "server.config.allow_current_directory",
      "server.runtime.data_dir",
      "server.runtime.control_dir",
      "server.runtime.pid_file",
      "server.runtime.lifecycle_state_file",
      "server.runtime.lifecycle_journal_file",
      "server.logging.log_file",
      "server.logging.log_level",
      "server.config.source_epoch",
      "server.config.reload_generation",
      "server.runtime.cache_invalidation_epoch",
      "server.capability.policy_generation",
      "server.security.authority_mode",
      "server.security.database_path",
      "server.security.policy_generation",
      "server.security.epoch",
      "server.security.provider_family",
      "server.security.provider_generation",
      "server.security.provider_state",
      "server.security.default_policy_installed",
      "server.database.default_path",
      "server.database.resource_seed_pack_root",
      "server.database.policy_seed_pack_root",
      "server.database.auto_create",
      "server.database.create_page_size_bytes",
      "server.database.open_mode",
      "server.database.daemon_scope",
      "server.listener.native.enabled",
      "server.listener.native.bind_host",
      "server.listener.native.port",
      "server.listener.native.executable_path",
      "server.listener.native.parser_executable_path",
      "server.listener.native.control_dir",
      "server.listener.native.runtime_dir",
      "server.listener.native.tls_required",
      "server.listener.native.tls_cert_file",
      "server.listener.native.tls_key_file",
      "server.listener.native.tls_ca_file",
      "server.listener.native.ready_timeout_ms",
      "server.metrics.enabled",
      "server.metrics.flush_interval_ms",
      "server.memory.policy_name",
      "server.memory.hard_limit_bytes",
      "server.memory.soft_limit_bytes",
      "server.memory.per_context_limit_bytes",
      "server.memory.page_buffer_pool_limit_bytes",
      "server.memory.min_startup_available_bytes",
      "server.memory.failure_mode",
      "server.memory.track_allocations",
      "server.memory.zero_memory_on_allocate",
      "server.memory.zero_memory_on_release",
      "server.memory.reject_over_soft_limit",
      "server.memory.adaptive_page_cache_enabled",
      "server.memory.index_read_cache_enabled",
      "server.memory.trim_heap_on_disconnect",
      "server.memory.policy_provenance",
      "server.memory.policy_generation",
      "server.memory.enable_platform_memory_probe",
      "server.memory.require_platform_memory_ceiling",
      "server.parser.registry_path",
      "server.parser.worker_restart_max",
      "server.parser.worker_restart_window_ms",
      "server.parser.sbps_enabled",
      "server.parser.sbps_endpoint",
      "server.parser.sbps_max_frame_bytes",
      "server.parser.sbps_max_streams",
      "server.parser.sbps_hello_timeout_ms",
  };
  return keys;
}

bool ParseBool(const std::string& value, bool* out) {
  const auto lower = LowerAscii(value);
  if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") {
    *out = true;
    return true;
  }
  if (lower == "false" || lower == "no" || lower == "off" || lower == "0") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseUint64(const std::string& value, std::uint64_t* out) {
  const auto trimmed = Trim(value);
  if (trimmed.empty() || trimmed.front() == '-') {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto* begin = trimmed.data();
  const auto* end = begin + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseServerConfigFormatVersion(std::string_view format, std::uint32_t* out) {
  if (format.size() != 5 || format.substr(0, 4) != "SBCD") {
    return false;
  }
  const char digit = format[4];
  if (digit < '0' || digit > '9') {
    return false;
  }
  *out = static_cast<std::uint32_t>(digit - '0');
  return true;
}

bool ParseDurationMs(const std::string& value, std::uint64_t* out) {
  auto trimmed = Trim(value);
  std::uint64_t multiplier = 1;
  if (trimmed.ends_with("ms")) {
    trimmed.resize(trimmed.size() - 2);
  } else if (trimmed.ends_with("s")) {
    trimmed.resize(trimmed.size() - 1);
    multiplier = 1000;
  } else if (trimmed.ends_with("m")) {
    trimmed.resize(trimmed.size() - 1);
    multiplier = 60000;
  } else if (trimmed.ends_with("h")) {
    trimmed.resize(trimmed.size() - 1);
    multiplier = 3600000;
  }
  std::uint64_t base = 0;
  if (!ParseUint64(trimmed, &base)) {
    return false;
  }
  *out = base * multiplier;
  return true;
}

std::filesystem::path NormalizePath(const std::string& raw) {
  std::filesystem::path path(raw);
  if (path.empty()) {
    return {};
  }
  if (path.is_relative()) {
    path = std::filesystem::current_path() / path;
  }
  return path.lexically_normal();
}

std::filesystem::path NormalizePath(const std::filesystem::path& raw) {
  if (raw.empty()) {
    return {};
  }
  std::filesystem::path path = raw;
  if (path.is_relative()) {
    path = std::filesystem::current_path() / path;
  }
  return path.lexically_normal();
}

std::filesystem::path ExistingDirectoryOrEmpty(const std::filesystem::path& raw) {
  if (raw.empty()) {
    return {};
  }
  const auto candidate = NormalizePath(raw);
  std::error_code ec;
  if (std::filesystem::is_directory(candidate, ec)) {
    return candidate;
  }
  return {};
}

std::filesystem::path EnvironmentPathOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return NormalizePath(std::string(value));
}

std::filesystem::path DefaultResourceSeedPackRoot() {
  if (auto path = ExistingDirectoryOrEmpty(EnvironmentPathOrEmpty(
          "SCRATCHBIRD_RESOURCE_SEED_PACK_ROOT"));
      !path.empty()) {
    return path;
  }

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    if (auto path = ExistingDirectoryOrEmpty(
            cwd / "project" / "resources" / "seed-packs" /
            "initial-resource-pack");
        !path.empty()) {
      return path;
    }
  }

  if (auto path = ExistingDirectoryOrEmpty(
          "/usr/share/scratchbird/resources/seed-packs/initial-resource-pack");
      !path.empty()) {
    return path;
  }
  if (auto path = ExistingDirectoryOrEmpty(
          "/opt/scratchbird/resources/seed-packs/initial-resource-pack");
      !path.empty()) {
    return path;
  }
  return {};
}

std::filesystem::path DefaultPolicySeedPackRoot() {
  if (auto path = ExistingDirectoryOrEmpty(EnvironmentPathOrEmpty(
          "SCRATCHBIRD_POLICY_SEED_PACK_ROOT"));
      !path.empty()) {
    return path;
  }
  if (auto path = ExistingDirectoryOrEmpty(EnvironmentPathOrEmpty(
          "SCRATCHBIRD_POLICY_PACK_ROOT"));
      !path.empty()) {
    return path;
  }

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    if (auto path = ExistingDirectoryOrEmpty(
            cwd / "project" / "resources" / "policy-packs" /
            "default-local-password");
        !path.empty()) {
      return path;
    }
  }

  if (auto path = ExistingDirectoryOrEmpty(
          "/usr/share/scratchbird/resources/policy-packs/default-local-password");
      !path.empty()) {
    return path;
  }
  if (auto path = ExistingDirectoryOrEmpty(
          "/opt/scratchbird/resources/policy-packs/default-local-password");
      !path.empty()) {
    return path;
  }
  return {};
}

std::string StablePathScopeId(const std::filesystem::path& path) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto text = path.lexically_normal().string();
  for (unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "db-" << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::optional<ParsedConfig> ParseConfigFile(const std::filesystem::path& path,
                                            std::vector<ServerDiagnostic>* diagnostics) {
  std::ifstream input(path);
  if (!input) {
    diagnostics->push_back(ConfigDiagnostic("CONFIG.FILE_UNREADABLE",
                                            "config.file_unreadable",
                                            "The configuration file could not be read.",
                                            {{"path_redacted", path.string()}}));
    return std::nullopt;
  }
  ParsedConfig parsed;
  std::string section;
  std::string line;
  std::uint64_t line_number = 0;
  std::set<std::string> seen;
  while (std::getline(input, line)) {
    ++line_number;
    line = Trim(StripInlineComment(line));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      section = LowerAscii(Trim(line.substr(1, line.size() - 2)));
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos || section.empty()) {
      diagnostics->push_back(ConfigDiagnostic("CONFIG.MALFORMED",
                                              "config.malformed",
                                              "A configuration line is malformed.",
                                              {{"path_redacted", path.string()},
                                               {"line", std::to_string(line_number)}}));
      return std::nullopt;
    }
    const auto key = LowerAscii(section + "." + Trim(line.substr(0, eq)));
    if (!KnownKeys().contains(key)) {
      diagnostics->push_back(ConfigDiagnostic("CONFIG.KEY_UNKNOWN",
                                              "config.key_unknown",
                                              "The configuration file contains an unknown key.",
                                              {{"canonical_key", key}, {"source_id", path.string()}}));
      return std::nullopt;
    }
    if (seen.contains(key)) {
      diagnostics->push_back(ConfigDiagnostic("CONFIG.KEY_DUPLICATE",
                                              "config.key_duplicate",
                                              "The configuration file contains a duplicate key.",
                                              {{"canonical_key", key}, {"source_id", path.string()}}));
      return std::nullopt;
    }
    seen.insert(key);
    auto value = Unquote(line.substr(eq + 1), key, diagnostics);
    if (!diagnostics->empty()) {
      return std::nullopt;
    }
    parsed.values[key] = value;
  }
  const auto format = parsed.values.find("config.format");
  if (format == parsed.values.end()) {
    diagnostics->push_back(ConfigDiagnostic("CONFIG.VERSION_MISSING",
                                            "config.version_missing",
                                            "The configuration file is missing config.format.",
                                            {{"path_redacted", path.string()}}));
    return std::nullopt;
  }
  const auto compatibility = ClassifyServerConfigFormat(format->second);
  if (!compatibility.accepted) {
    diagnostics->push_back(compatibility.diagnostic);
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::filesystem::path> DiscoverConfigFile(const ServerCliOptions& cli) {
  if (!cli.config_path.empty()) {
    return NormalizePath(cli.config_path);
  }
  if (const char* env = std::getenv("SCRATCHBIRD_CONFIG"); env != nullptr && *env != '\0') {
    return NormalizePath(std::string(env));
  }
  const auto current = std::filesystem::current_path() / "sb_server.conf";
  if (std::filesystem::is_regular_file(current)) {
    return current.lexically_normal();
  }
#ifndef _WIN32
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    const auto path = std::filesystem::path(xdg) / "scratchbird" / "sb_server.conf";
    if (std::filesystem::is_regular_file(path)) {
      return path.lexically_normal();
    }
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    const auto path = std::filesystem::path(home) / ".config" / "scratchbird" / "sb_server.conf";
    if (std::filesystem::is_regular_file(path)) {
      return path.lexically_normal();
    }
  }
  const auto system = std::filesystem::path("/etc/scratchbird/sb_server.conf");
  if (std::filesystem::is_regular_file(system)) {
    return system;
  }
#endif
  return std::nullopt;
}

bool EnumAllowed(const std::string& value, std::initializer_list<const char*> allowed) {
  for (const char* item : allowed) {
    if (value == item) {
      return true;
    }
  }
  return false;
}

// PUBLIC_DEFAULT_CONFIG_CHECK
// Public startup configuration refuses fixture, unknown, or non-authentication
// provider families before the server publishes lifecycle artifacts.
bool PublicStartupAuthProviderFamilyAllowed(const std::string& value,
                                            std::string* canonical) {
  auto family = engine_api::CanonicalAuthProviderFamily(value);
  if (family.empty()) {
    family = "local_password";
  }
  if (!engine_api::IsKnownAuthProviderFamily(family) ||
      !engine_api::AuthProviderFamilySupportsAuthn(family)) {
    return false;
  }
  if (family.find("fixture") != std::string::npos ||
      family.find("test") != std::string::npos) {
    return false;
  }
  *canonical = std::move(family);
  return true;
}

std::optional<std::size_t> JsonValueOffset(const std::string& text,
                                           const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  const auto key_pos = text.find(marker);
  if (key_pos == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = text.find(':', key_pos + marker.size());
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  auto value_pos = colon + 1;
  while (value_pos < text.size() &&
         (text[value_pos] == ' ' || text[value_pos] == '\t' ||
          text[value_pos] == '\r' || text[value_pos] == '\n')) {
    ++value_pos;
  }
  return value_pos;
}

bool ExtractJsonStringField(const std::string& text,
                            const std::string& key,
                            std::string* out) {
  const auto value_pos = JsonValueOffset(text, key);
  if (!value_pos || *value_pos >= text.size() || text[*value_pos] != '"') {
    return false;
  }
  std::string value;
  bool escaped = false;
  for (std::size_t i = *value_pos + 1; i < text.size(); ++i) {
    const char ch = text[i];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      *out = std::move(value);
      return true;
    }
    value.push_back(ch);
  }
  return false;
}

bool ExtractJsonUint64Field(const std::string& text,
                            const std::string& key,
                            std::uint64_t* out) {
  const auto value_pos = JsonValueOffset(text, key);
  if (!value_pos) {
    return false;
  }
  std::size_t end = *value_pos;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
    ++end;
  }
  if (end == *value_pos) {
    return false;
  }
  return ParseUint64(text.substr(*value_pos, end - *value_pos), out);
}

bool ExtractJsonBoolField(const std::string& text,
                          const std::string& key,
                          bool* out) {
  const auto value_pos = JsonValueOffset(text, key);
  if (!value_pos) {
    return false;
  }
  if (text.compare(*value_pos, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (text.compare(*value_pos, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

std::set<std::string> ExplicitServerMemoryKeys(const ParsedConfig* parsed) {
  std::set<std::string> keys;
  if (parsed == nullptr) {
    return keys;
  }
  for (const auto& [key, value] : parsed->values) {
    (void)value;
    if (key.rfind("server.memory.", 0) == 0) {
      keys.insert(key);
    }
  }
  return keys;
}

bool ExplicitlyConfigured(const std::set<std::string>& keys,
                          const std::string& key) {
  return keys.find(key) != keys.end();
}

bool ApplyDefaultMemoryPolicyFromPolicyPack(
    ServerBootstrapConfig* config,
    const std::set<std::string>& explicit_memory_keys,
    std::vector<ServerDiagnostic>* diagnostics) {
  if (config == nullptr || diagnostics == nullptr ||
      config->database_policy_seed_pack_root.empty()) {
    return true;
  }

  const auto path = config->database_policy_seed_pack_root /
                    "policies" / "server_memory_cache_policy.json";
  std::ifstream input(path);
  if (!input) {
    diagnostics->push_back(ConfigDiagnostic(
        "CONFIG.DEFAULT_MEMORY_POLICY_UNAVAILABLE",
        "config.default_memory_policy_unavailable",
        "The default server memory/cache policy could not be read.",
        {{"path_redacted", path.string()}}));
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string text = buffer.str();

  std::string profile_area;
  std::string policy_name;
  std::string provenance;
  std::string failure_mode;
  std::uint64_t schema_version = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t hard_limit_bytes = 0;
  std::uint64_t soft_limit_bytes = 0;
  std::uint64_t per_context_limit_bytes = 0;
  std::uint64_t page_buffer_pool_limit_bytes = 0;
  std::uint64_t min_startup_available_bytes = 0;
  bool track_allocations = false;
  bool zero_memory_on_allocate = false;
  bool zero_memory_on_release = false;
  bool reject_over_soft_limit = false;
  bool enable_platform_memory_probe = false;
  bool require_platform_memory_ceiling = false;
  bool adaptive_page_cache_enabled = false;
  bool index_read_cache_enabled = false;
  bool trim_heap_on_disconnect = true;
  bool cache_finality_authority = true;
  bool cache_visibility_authority = true;
  bool wal_or_redo_authority = true;
  bool dirty_writeback_required = false;

  if (!ExtractJsonUint64Field(text, "schema_version", &schema_version) ||
      schema_version != 1 ||
      !ExtractJsonUint64Field(text, "policy_generation", &policy_generation) ||
      policy_generation == 0 ||
      !ExtractJsonStringField(text, "profile_area", &profile_area) ||
      profile_area != "memory_resource_governance" ||
      !ExtractJsonStringField(text, "policy_name", &policy_name) ||
      !ExtractJsonStringField(text, "provenance", &provenance) ||
      !ExtractJsonUint64Field(text, "hard_limit_bytes", &hard_limit_bytes) ||
      !ExtractJsonUint64Field(text, "soft_limit_bytes", &soft_limit_bytes) ||
      !ExtractJsonUint64Field(text, "per_context_limit_bytes", &per_context_limit_bytes) ||
      !ExtractJsonUint64Field(text, "page_buffer_pool_limit_bytes",
                              &page_buffer_pool_limit_bytes) ||
      !ExtractJsonUint64Field(text, "min_startup_available_bytes",
                              &min_startup_available_bytes) ||
      !ExtractJsonStringField(text, "failure_mode", &failure_mode) ||
      !ExtractJsonBoolField(text, "track_allocations", &track_allocations) ||
      !ExtractJsonBoolField(text, "zero_memory_on_allocate", &zero_memory_on_allocate) ||
      !ExtractJsonBoolField(text, "zero_memory_on_release", &zero_memory_on_release) ||
      !ExtractJsonBoolField(text, "reject_over_soft_limit", &reject_over_soft_limit) ||
      !ExtractJsonBoolField(text, "enable_platform_memory_probe",
                            &enable_platform_memory_probe) ||
      !ExtractJsonBoolField(text, "require_platform_memory_ceiling",
                            &require_platform_memory_ceiling) ||
      !ExtractJsonBoolField(text, "enabled", &adaptive_page_cache_enabled) ||
      !ExtractJsonBoolField(text, "index_read_optimization", &index_read_cache_enabled) ||
      !ExtractJsonBoolField(text, "ordinary_disconnect_heap_trim",
                            &trim_heap_on_disconnect) ||
      !ExtractJsonBoolField(text, "dirty_writeback_required", &dirty_writeback_required) ||
      !ExtractJsonBoolField(text, "cache_finality_authority", &cache_finality_authority) ||
      !ExtractJsonBoolField(text, "cache_visibility_authority", &cache_visibility_authority) ||
      !ExtractJsonBoolField(text, "wal_or_redo_authority", &wal_or_redo_authority)) {
    diagnostics->push_back(ConfigDiagnostic(
        "CONFIG.DEFAULT_MEMORY_POLICY_MALFORMED",
        "config.default_memory_policy_malformed",
        "The default server memory/cache policy is malformed.",
        {{"path_redacted", path.string()}}));
    return false;
  }

  memory::AllocationFailureMode parsed_failure_mode{};
  if (!memory::ParseAllocationFailureMode(failure_mode, &parsed_failure_mode) ||
      hard_limit_bytes < min_startup_available_bytes ||
      soft_limit_bytes > hard_limit_bytes ||
      per_context_limit_bytes > hard_limit_bytes ||
      page_buffer_pool_limit_bytes > hard_limit_bytes ||
      !dirty_writeback_required ||
      cache_finality_authority ||
      cache_visibility_authority ||
      wal_or_redo_authority) {
    diagnostics->push_back(ConfigDiagnostic(
        "CONFIG.DEFAULT_MEMORY_POLICY_INVALID",
        "config.default_memory_policy_invalid",
        "The default server memory/cache policy violates the ScratchBird authority or limit contract.",
        {{"path_redacted", path.string()},
         {"policy_name", policy_name},
         {"provenance", provenance}}));
    return false;
  }

  auto apply_string = [&](const std::string& key, std::string value, std::string* target) {
    if (!ExplicitlyConfigured(explicit_memory_keys, key)) {
      *target = std::move(value);
    }
  };
  auto apply_u64 = [&](const std::string& key, std::uint64_t value, std::uint64_t* target) {
    if (!ExplicitlyConfigured(explicit_memory_keys, key)) {
      *target = value;
    }
  };
  auto apply_bool = [&](const std::string& key, bool value, bool* target) {
    if (!ExplicitlyConfigured(explicit_memory_keys, key)) {
      *target = value;
    }
  };

  apply_string("server.memory.policy_name", policy_name, &config->memory_policy_name);
  apply_u64("server.memory.hard_limit_bytes", hard_limit_bytes,
            &config->memory_hard_limit_bytes);
  apply_u64("server.memory.soft_limit_bytes", soft_limit_bytes,
            &config->memory_soft_limit_bytes);
  apply_u64("server.memory.per_context_limit_bytes", per_context_limit_bytes,
            &config->memory_per_context_limit_bytes);
  apply_u64("server.memory.page_buffer_pool_limit_bytes", page_buffer_pool_limit_bytes,
            &config->memory_page_buffer_pool_limit_bytes);
  apply_u64("server.memory.min_startup_available_bytes", min_startup_available_bytes,
            &config->memory_min_startup_available_bytes);
  if (!ExplicitlyConfigured(explicit_memory_keys, "server.memory.failure_mode")) {
    config->memory_failure_mode = parsed_failure_mode;
  }
  apply_bool("server.memory.track_allocations", track_allocations,
             &config->memory_track_allocations);
  apply_bool("server.memory.zero_memory_on_allocate", zero_memory_on_allocate,
             &config->memory_zero_memory_on_allocate);
  apply_bool("server.memory.zero_memory_on_release", zero_memory_on_release,
             &config->memory_zero_memory_on_release);
  apply_bool("server.memory.reject_over_soft_limit", reject_over_soft_limit,
             &config->memory_reject_over_soft_limit);
  apply_bool("server.memory.adaptive_page_cache_enabled", adaptive_page_cache_enabled,
             &config->memory_adaptive_page_cache_enabled);
  apply_bool("server.memory.index_read_cache_enabled", index_read_cache_enabled,
             &config->memory_index_read_cache_enabled);
  apply_bool("server.memory.trim_heap_on_disconnect", trim_heap_on_disconnect,
             &config->memory_trim_heap_on_disconnect);
  apply_string("server.memory.policy_provenance", provenance,
               &config->memory_policy_provenance);
  apply_u64("server.memory.policy_generation", policy_generation,
            &config->memory_policy_generation);
  apply_bool("server.memory.enable_platform_memory_probe", enable_platform_memory_probe,
             &config->memory_enable_platform_memory_probe);
  apply_bool("server.memory.require_platform_memory_ceiling",
             require_platform_memory_ceiling,
             &config->memory_require_platform_memory_ceiling);
  return true;
}

memory::MemoryPolicyConfig BuildMemoryPolicyConfig(const ServerBootstrapConfig& config) {
  memory::MemoryPolicyConfig memory_config;
  memory_config.policy_name = config.memory_policy_name;
  memory_config.hard_limit_bytes = config.memory_hard_limit_bytes;
  memory_config.soft_limit_bytes = config.memory_soft_limit_bytes;
  memory_config.per_context_limit_bytes = config.memory_per_context_limit_bytes;
  memory_config.page_buffer_pool_limit_bytes = config.memory_page_buffer_pool_limit_bytes;
  memory_config.failure_mode = config.memory_failure_mode;
  memory_config.track_allocations = config.memory_track_allocations;
  memory_config.zero_memory_on_allocate = config.memory_zero_memory_on_allocate;
  memory_config.zero_memory_on_release = config.memory_zero_memory_on_release;
  memory_config.reject_over_soft_limit = config.memory_reject_over_soft_limit;
  memory_config.provenance = config.memory_policy_provenance;
  memory_config.source_epoch = config.config_source_epoch;
  memory_config.reload_generation = config.config_reload_generation;
  memory_config.policy_generation = config.memory_policy_generation;
  memory_config.enable_platform_memory_probe = config.memory_enable_platform_memory_probe;
  memory_config.require_platform_memory_ceiling = config.memory_require_platform_memory_ceiling;
  return memory_config;
}

bool ValidateServerMemoryPolicy(const ServerBootstrapConfig& config,
                                std::vector<ServerDiagnostic>* diagnostics) {
  const auto resolved = memory::ResolveMemoryPolicyConfig(BuildMemoryPolicyConfig(config));
  if (!resolved.ok()) {
    for (const auto& diagnostic : resolved.diagnostics) {
      std::vector<ServerDiagnosticField> fields;
      fields.push_back({"source_component", diagnostic.source_component});
      fields.push_back({"provenance", config.memory_policy_provenance});
      fields.push_back({"policy_generation", std::to_string(config.memory_policy_generation)});
      for (const auto& argument : diagnostic.arguments) {
        fields.push_back({argument.key, argument.value});
      }
      diagnostics->push_back(ConfigDiagnostic(diagnostic.diagnostic_code,
                                              diagnostic.message_key,
                                              "The configured production memory policy is invalid.",
                                              std::move(fields)));
    }
    return false;
  }
  if (config.memory_hard_limit_bytes < config.memory_min_startup_available_bytes) {
    diagnostics->push_back(ConfigDiagnostic(
        "CONFIG.MEMORY_POLICY_MIN_STARTUP_UNAVAILABLE",
        "config.memory_policy_min_startup_unavailable",
        "The configured memory hard limit is below the startup memory availability floor.",
        {{"hard_limit_bytes", std::to_string(config.memory_hard_limit_bytes)},
         {"min_startup_available_bytes",
          std::to_string(config.memory_min_startup_available_bytes)},
         {"provenance", config.memory_policy_provenance}}));
    return false;
  }
  return true;
}

bool ApplyParsedConfig(const ParsedConfig& parsed,
                       ServerBootstrapConfig* config,
                       std::vector<ServerDiagnostic>* diagnostics) {
  auto invalid = [&](const std::string& code, const std::string& key, const std::string& value) {
    diagnostics->push_back(ConfigDiagnostic(code,
                                            LowerAscii(code),
                                            "A configuration value is invalid.",
                                            {{"canonical_key", key}, {"value_redacted", value}}));
    return false;
  };

  for (const auto& [key, value] : parsed.values) {
    if (key == "config.format") {
      continue;
    }
    const auto lower = LowerAscii(value);
    if (key == "server.mode") {
      if (lower == "foreground") config->mode = ServerMode::kForeground;
      else if (lower == "service") config->mode = ServerMode::kService;
      else if (lower == "validation_only") config->mode = ServerMode::kValidationOnly;
      else if (lower == "maintenance") config->mode = ServerMode::kMaintenance;
      else if (lower == "read_only") config->mode = ServerMode::kReadOnly;
      else return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
    } else if (key == "server.config.allow_current_directory") {
      if (!ParseBool(value, &config->allow_current_directory)) return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
    } else if (key == "server.runtime.data_dir") {
      config->data_dir = NormalizePath(value);
    } else if (key == "server.runtime.control_dir") {
      config->control_dir = NormalizePath(value);
    } else if (key == "server.runtime.pid_file") {
      config->pid_file = NormalizePath(value);
    } else if (key == "server.runtime.lifecycle_state_file") {
      config->lifecycle_state_file = NormalizePath(value);
    } else if (key == "server.runtime.lifecycle_journal_file") {
      config->lifecycle_journal_file = NormalizePath(value);
    } else if (key == "server.logging.log_file") {
      config->log_file = value;
    } else if (key == "server.logging.log_level") {
      if (!EnumAllowed(lower, {"trace","debug","info","warning","warn","error","critical","fatal"})) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
      config->log_level = lower == "warning" ? "warn" : (lower == "critical" ? "fatal" : lower);
    } else if (key == "server.config.source_epoch") {
      if (!ParseUint64(value, &config->config_source_epoch) || config->config_source_epoch == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.config.reload_generation") {
      if (!ParseUint64(value, &config->config_reload_generation) ||
          config->config_reload_generation == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.runtime.cache_invalidation_epoch") {
      if (!ParseUint64(value, &config->cache_invalidation_epoch) ||
          config->cache_invalidation_epoch == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.capability.policy_generation") {
      if (!ParseUint64(value, &config->capability_policy_generation) ||
          config->capability_policy_generation == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.security.authority_mode") {
      if (!EnumAllowed(lower, {"database_local","security_database","external_provider","cluster"})) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
      config->security_authority_mode = lower;
    } else if (key == "server.security.database_path") {
      config->security_database_path = NormalizePath(value);
    } else if (key == "server.security.policy_generation") {
      if (!ParseUint64(value, &config->security_policy_generation) ||
          config->security_policy_generation == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.security.epoch") {
      if (!ParseUint64(value, &config->security_epoch) || config->security_epoch == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.security.provider_family") {
      std::string provider_family;
      if (!PublicStartupAuthProviderFamilyAllowed(value, &provider_family)) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
      config->security_provider_family = std::move(provider_family);
    } else if (key == "server.security.provider_generation") {
      if (!ParseUint64(value, &config->security_provider_generation) ||
          config->security_provider_generation == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.security.provider_state") {
      if (!EnumAllowed(lower, {"loaded","started","healthy","degraded","quiescing",
                               "quiesced","disabled","quarantined"})) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
      config->security_provider_state = lower;
    } else if (key == "server.security.default_policy_installed") {
      if (!ParseBool(value, &config->security_default_policy_installed)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.database.default_path") {
      config->database_default_path = NormalizePath(value);
    } else if (key == "server.database.resource_seed_pack_root") {
      config->database_resource_seed_pack_root = NormalizePath(value);
    } else if (key == "server.database.policy_seed_pack_root") {
      config->database_policy_seed_pack_root = NormalizePath(value);
    } else if (key == "server.database.auto_create") {
      if (!ParseBool(value, &config->database_auto_create)) return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
    } else if (key == "server.database.create_page_size_bytes") {
      if (!ParseUint64(value, &config->database_create_page_size_bytes) ||
          (config->database_create_page_size_bytes != 4096 &&
           config->database_create_page_size_bytes != 8192 &&
           config->database_create_page_size_bytes != 16384 &&
           config->database_create_page_size_bytes != 32768 &&
           config->database_create_page_size_bytes != 65536 &&
           config->database_create_page_size_bytes != 131072)) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.database.open_mode") {
      if (!EnumAllowed(lower, {"normal","read_only","maintenance","restricted","restricted_open"})) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
      config->database_open_mode = lower;
    } else if (key == "server.database.daemon_scope") {
      if (!EnumAllowed(lower, {"shared","dedicated"})) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
      config->database_daemon_scope = lower;
    } else if (key == "server.listener.native.enabled") {
      if (!ParseBool(value, &config->listener_native_enabled)) return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
    } else if (key == "server.listener.native.bind_host") {
      config->listener_native_bind_host = value;
    } else if (key == "server.listener.native.port") {
      if (!ParseUint64(value, &config->listener_native_port)) return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
    } else if (key == "server.listener.native.executable_path") {
      config->listener_native_executable_path = NormalizePath(value);
    } else if (key == "server.listener.native.parser_executable_path") {
      config->listener_native_parser_executable_path = NormalizePath(value);
    } else if (key == "server.listener.native.control_dir") {
      config->listener_native_control_dir = NormalizePath(value);
    } else if (key == "server.listener.native.runtime_dir") {
      config->listener_native_runtime_dir = NormalizePath(value);
    } else if (key == "server.listener.native.tls_required") {
      if (!ParseBool(value, &config->listener_native_tls_required)) return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
    } else if (key == "server.listener.native.tls_cert_file") {
      config->listener_native_tls_cert_file = NormalizePath(value);
    } else if (key == "server.listener.native.tls_key_file") {
      config->listener_native_tls_key_file = NormalizePath(value);
    } else if (key == "server.listener.native.tls_ca_file") {
      config->listener_native_tls_ca_file = NormalizePath(value);
    } else if (key == "server.listener.native.ready_timeout_ms") {
      if (!ParseDurationMs(value, &config->listener_native_ready_timeout_ms)) return invalid("CONFIG.VALUE_INVALID_DURATION", key, value);
    } else if (key == "server.metrics.enabled") {
      if (!ParseBool(value, &config->metrics_enabled)) return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
    } else if (key == "server.metrics.flush_interval_ms") {
      if (!ParseDurationMs(value, &config->metrics_flush_interval_ms)) return invalid("CONFIG.VALUE_INVALID_DURATION", key, value);
    } else if (key == "server.memory.policy_name") {
      config->memory_policy_name = value;
    } else if (key == "server.memory.hard_limit_bytes") {
      if (!ParseUint64(value, &config->memory_hard_limit_bytes)) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.memory.soft_limit_bytes") {
      if (!ParseUint64(value, &config->memory_soft_limit_bytes)) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.memory.per_context_limit_bytes") {
      if (!ParseUint64(value, &config->memory_per_context_limit_bytes)) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.memory.page_buffer_pool_limit_bytes") {
      if (!ParseUint64(value, &config->memory_page_buffer_pool_limit_bytes)) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.memory.min_startup_available_bytes") {
      if (!ParseUint64(value, &config->memory_min_startup_available_bytes)) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.memory.failure_mode") {
      if (!memory::ParseAllocationFailureMode(value, &config->memory_failure_mode)) {
        return invalid("CONFIG.VALUE_INVALID_ENUM", key, value);
      }
    } else if (key == "server.memory.track_allocations") {
      if (!ParseBool(value, &config->memory_track_allocations)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.zero_memory_on_allocate") {
      if (!ParseBool(value, &config->memory_zero_memory_on_allocate)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.zero_memory_on_release") {
      if (!ParseBool(value, &config->memory_zero_memory_on_release)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.reject_over_soft_limit") {
      if (!ParseBool(value, &config->memory_reject_over_soft_limit)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.adaptive_page_cache_enabled") {
      if (!ParseBool(value, &config->memory_adaptive_page_cache_enabled)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.index_read_cache_enabled") {
      if (!ParseBool(value, &config->memory_index_read_cache_enabled)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.trim_heap_on_disconnect") {
      if (!ParseBool(value, &config->memory_trim_heap_on_disconnect)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.policy_provenance") {
      config->memory_policy_provenance = value;
    } else if (key == "server.memory.policy_generation") {
      if (!ParseUint64(value, &config->memory_policy_generation) ||
          config->memory_policy_generation == 0) {
        return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
      }
    } else if (key == "server.memory.enable_platform_memory_probe") {
      if (!ParseBool(value, &config->memory_enable_platform_memory_probe)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.memory.require_platform_memory_ceiling") {
      if (!ParseBool(value, &config->memory_require_platform_memory_ceiling)) {
        return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
      }
    } else if (key == "server.parser.registry_path") {
      config->parser_registry_path = NormalizePath(value);
    } else if (key == "server.parser.worker_restart_max") {
      if (!ParseUint64(value, &config->parser_worker_restart_max)) return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
    } else if (key == "server.parser.worker_restart_window_ms") {
      if (!ParseDurationMs(value, &config->parser_worker_restart_window_ms)) return invalid("CONFIG.VALUE_INVALID_DURATION", key, value);
    } else if (key == "server.parser.sbps_enabled") {
      if (!ParseBool(value, &config->sbps_enabled)) return invalid("CONFIG.VALUE_INVALID_BOOL", key, value);
    } else if (key == "server.parser.sbps_endpoint") {
      config->sbps_endpoint = NormalizePath(value);
    } else if (key == "server.parser.sbps_max_frame_bytes") {
      if (!ParseUint64(value, &config->sbps_max_frame_bytes)) return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
    } else if (key == "server.parser.sbps_max_streams") {
      if (!ParseUint64(value, &config->sbps_max_streams)) return invalid("CONFIG.VALUE_INVALID_UINT", key, value);
    } else if (key == "server.parser.sbps_hello_timeout_ms") {
      if (!ParseDurationMs(value, &config->sbps_hello_timeout_ms)) return invalid("CONFIG.VALUE_INVALID_DURATION", key, value);
    }
  }
  return true;
}

void ApplyCliOverrides(const ServerCliOptions& cli, ServerBootstrapConfig* config) {
  if (cli.foreground) config->mode = ServerMode::kForeground;
  if (cli.service) config->mode = ServerMode::kService;
  if (cli.validate_config || cli.validate_endpoints) config->mode = ServerMode::kValidationOnly;
  if (cli.maintenance) {
    config->mode = ServerMode::kMaintenance;
    config->database_open_mode = "maintenance";
  }
  if (cli.read_only) {
    config->mode = ServerMode::kReadOnly;
    config->database_open_mode = "read_only";
  }
  if (cli.restricted_open) {
    config->database_open_mode = "restricted";
  }
  if (cli.create_if_missing) config->database_auto_create = true;
  if (cli.create_page_size_bytes != 0) {
    config->database_create_page_size_bytes = cli.create_page_size_bytes;
  }
  if (!cli.control_dir.empty()) config->control_dir = NormalizePath(cli.control_dir);
  if (!cli.runtime_dir.empty()) config->data_dir = NormalizePath(cli.runtime_dir);
  if (!cli.database_ref.empty()) config->database_default_path = NormalizePath(cli.database_ref);
  if (!cli.sbps_endpoint.empty()) config->sbps_endpoint = NormalizePath(cli.sbps_endpoint);
  if (!cli.log_path.empty()) config->log_file = cli.log_path;
  if (!cli.log_level.empty()) config->log_level = cli.log_level;
}

void FinalizeDerivedPaths(ServerBootstrapConfig* config) {
  if (config->mode == ServerMode::kForeground || config->mode == ServerMode::kValidationOnly) {
    if (config->log_file.empty()) {
      config->log_file = "stderr";
    }
  }
  if (!config->database_default_path.empty() && config->database_runtime_scope_id.empty()) {
    config->database_runtime_scope_id = StablePathScopeId(config->database_default_path);
  }
  if (config->database_daemon_scope == "dedicated" && !config->database_runtime_scope_id.empty()) {
    const auto scope = std::filesystem::path("databases") / config->database_runtime_scope_id;
    config->control_dir = (config->control_dir / scope).lexically_normal();
    config->data_dir = (config->data_dir / scope).lexically_normal();
  }
  if (config->pid_file.empty()) {
    config->pid_file = config->control_dir / "sb_server.pid";
  }
  if (config->lifecycle_state_file.empty()) {
    config->lifecycle_state_file = config->control_dir / "sb_server.lifecycle.state";
  }
  if (config->lifecycle_journal_file.empty()) {
    config->lifecycle_journal_file = config->control_dir / "sb_server.lifecycle.journal";
  }
  if (config->sbps_endpoint.empty()) {
    config->sbps_endpoint = config->control_dir / "sb_server.sbps.sock";
  }
  if (config->database_resource_seed_pack_root.empty()) {
    config->database_resource_seed_pack_root = DefaultResourceSeedPackRoot();
  }
  if (config->database_policy_seed_pack_root.empty()) {
    config->database_policy_seed_pack_root = DefaultPolicySeedPackRoot();
  }
  if (config->listener_native_control_dir.empty()) {
    config->listener_native_control_dir = config->control_dir / "listeners";
  }
  if (config->listener_native_runtime_dir.empty()) {
    config->listener_native_runtime_dir = config->data_dir / "listeners";
  }
  if (config->mode == ServerMode::kService && config->log_file == "stderr") {
    config->log_file = "/var/log/scratchbird/sb_server.log";
  }
  if (config->mode == ServerMode::kService) {
    config->allow_current_directory = false;
  }
  const auto min_cache_epoch = std::max({config->config_source_epoch,
                                         config->config_reload_generation,
                                         config->capability_policy_generation,
                                         config->security_policy_generation,
                                         config->security_epoch,
                                         config->security_provider_generation});
  if (config->cache_invalidation_epoch < min_cache_epoch) {
    config->cache_invalidation_epoch = min_cache_epoch;
  }
}

}  // namespace

const char* ServerModeName(ServerMode mode) {
  switch (mode) {
    case ServerMode::kForeground:
      return "foreground";
    case ServerMode::kService:
      return "service";
    case ServerMode::kValidationOnly:
      return "validation_only";
    case ServerMode::kMaintenance:
      return "maintenance";
    case ServerMode::kReadOnly:
      return "read_only";
  }
  return "foreground";
}

const char* ServerConfigCompatibilityClassName(ServerConfigCompatibilityClass compatibility_class) {
  switch (compatibility_class) {
    case ServerConfigCompatibilityClass::kSupportedCurrent:
      return "supported-current";
    case ServerConfigCompatibilityClass::kUnsupportedOld:
      return "unsupported-old";
    case ServerConfigCompatibilityClass::kUnsupportedNew:
      return "unsupported-new";
    case ServerConfigCompatibilityClass::kDowngradeRefused:
      return "downgrade-refused";
    case ServerConfigCompatibilityClass::kNewerThanSupportedRefused:
      return "newer-than-supported-refused";
    case ServerConfigCompatibilityClass::kMissingMigrationPlanRefused:
      return "missing-migration-plan-refused";
    case ServerConfigCompatibilityClass::kMigrationRequiredWithoutPlanRefused:
      return "migration-required-without-plan-refused";
  }
  return "unsupported-new";
}

ServerConfigCompatibilityResult ClassifyServerConfigFormat(
    std::string_view format,
    std::string_view migration_plan_id,
    bool downgrade_requested,
    bool migration_plan_required) {
  auto failure = [&](ServerConfigCompatibilityClass compatibility_class,
                     std::string code,
                     std::string message,
                     std::string detail) {
    ServerConfigCompatibilityResult result;
    result.accepted = false;
    result.migration_required =
        compatibility_class == ServerConfigCompatibilityClass::kMissingMigrationPlanRefused ||
        compatibility_class ==
            ServerConfigCompatibilityClass::kMigrationRequiredWithoutPlanRefused;
    result.compatibility_class = compatibility_class;
    result.diagnostic = ConfigDiagnostic(std::move(code),
                                         LowerAscii(code),
                                         std::move(message),
                                         {{"format", std::string(format)},
                                          {"detail", std::move(detail)}});
    return result;
  };

  if (downgrade_requested) {
    return failure(ServerConfigCompatibilityClass::kDowngradeRefused,
                   "CONFIG.DOWNGRADE_REFUSED",
                   "The configuration format would require an unsafe downgrade.",
                   "downgrade_requested");
  }
  if (migration_plan_required && migration_plan_id.empty()) {
    return failure(ServerConfigCompatibilityClass::kMissingMigrationPlanRefused,
                   "CONFIG.MIGRATION_PLAN_MISSING",
                   "The configuration migration requires an explicit plan.",
                   "plan_missing");
  }

  std::uint32_t version = 0;
  if (!ParseServerConfigFormatVersion(format, &version)) {
    return failure(ServerConfigCompatibilityClass::kUnsupportedNew,
                   "CONFIG.VERSION_UNSUPPORTED",
                   "The configuration format is not supported.",
                   "format_parse_failed");
  }
  if (version < kServerConfigFormatVersionMinSupported) {
    return failure(ServerConfigCompatibilityClass::kMigrationRequiredWithoutPlanRefused,
                   "CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN",
                   "The configuration format requires a migration plan.",
                   "supported_migration_requires_plan");
  }
  if (version > kServerConfigFormatVersionMaxSupported) {
    return failure(ServerConfigCompatibilityClass::kNewerThanSupportedRefused,
                   "CONFIG.VERSION_NEWER_THAN_SUPPORTED",
                   "The configuration format is newer than this server supports.",
                   "newer_than_supported");
  }

  ServerConfigCompatibilityResult result;
  result.accepted = true;
  result.compatibility_class = ServerConfigCompatibilityClass::kSupportedCurrent;
  return result;
}

std::string ServerDatabaseRuntimeScopeId(const std::filesystem::path& database_path) {
  if (database_path.empty()) return {};
  return StablePathScopeId(database_path);
}

memory::MemoryPolicyConfigResolveResult ResolveServerMemoryAllocationPolicy(
    const ServerBootstrapConfig& config) {
  return memory::ResolveMemoryPolicyConfig(BuildMemoryPolicyConfig(config));
}

ServerConfigLoadResult ResolveServerBootstrapConfig(const ServerCliOptions& cli) {
  ServerConfigLoadResult result;
  auto selected = DiscoverConfigFile(cli);
  std::optional<ParsedConfig> parsed_config;
  if (selected) {
    result.config.selected_config_path = *selected;
    result.config.selected_config_source = cli.config_path.empty() ? "discovered_file" : "explicit_file";
    auto parsed = ParseConfigFile(*selected, &result.diagnostics);
    if (!parsed) {
      return result;
    }
    if (!ApplyParsedConfig(*parsed, &result.config, &result.diagnostics)) {
      return result;
    }
    parsed_config = std::move(*parsed);
  }
  ApplyCliOverrides(cli, &result.config);
  FinalizeDerivedPaths(&result.config);
  const auto explicit_memory_keys =
      ExplicitServerMemoryKeys(parsed_config ? &*parsed_config : nullptr);
  if (!ApplyDefaultMemoryPolicyFromPolicyPack(&result.config,
                                              explicit_memory_keys,
                                              &result.diagnostics)) {
    return result;
  }
  if (!ValidateServerMemoryPolicy(result.config, &result.diagnostics)) {
    return result;
  }
  return result;
}

}  // namespace scratchbird::server
