// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_RUNTIME

#include "manager_runtime.hpp"
#include "manager_lifecycle.hpp"
#include "manager_listener_control.hpp"
#include "manager_mcp_payload.hpp"
#include "manager_restart_policy.hpp"
#include "manager_runtime_snapshot.hpp"
#include "manager_support_bundle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

namespace scratchbird::manager::node {
namespace {

std::atomic_bool g_stop_requested{false};

void StopHandler(int) { g_stop_requested.store(true); }

constexpr std::uint16_t kMcpProtocolVersion = 0x0100;
constexpr std::uint16_t kConnectFlagBaseCapabilities = 0x0001;
constexpr std::uint16_t kConnectFlagManagerDbbt = 0x0040;
constexpr std::string_view kMcpDbConnectExtendedMagic = "MCP1";
constexpr std::string_view kCanonicalSbsqlProfile = "SBsql";
constexpr std::string_view kDeprecatedNativeV3ProfileAlias = "native_v3";
constexpr std::string_view kReleaseProfileEnterprise = "enterprise";
constexpr std::string_view kReleaseProfileDeveloper = "developer";
constexpr std::string_view kReleaseProfileTest = "test";
constexpr std::string_view kReleaseProfileNativeOnly = "native_only";
constexpr std::string_view kReleaseProfileDiagnostic = "diagnostic";

bool IsSbsqlProfileAlias(std::string_view value) {
  return value == kCanonicalSbsqlProfile || value == kDeprecatedNativeV3ProfileAlias;
}

bool ManagerReleaseProfileValid(std::string_view value) {
  return value == kReleaseProfileEnterprise ||
         value == kReleaseProfileDeveloper ||
         value == kReleaseProfileTest ||
         value == kReleaseProfileNativeOnly ||
         value == kReleaseProfileDiagnostic;
}

bool ReleaseProfileAllowsDeveloperSecret(std::string_view value) {
  return value == kReleaseProfileDeveloper ||
         value == kReleaseProfileTest ||
         value == kReleaseProfileNativeOnly ||
         value == kReleaseProfileDiagnostic;
}

bool ReleaseProfileAllowsLocalTokenStore(std::string_view value) {
  return ReleaseProfileAllowsDeveloperSecret(value);
}

bool ReleaseProfileAllowsDirectNative(std::string_view value) {
  return value == kReleaseProfileDeveloper ||
         value == kReleaseProfileTest ||
         value == kReleaseProfileNativeOnly;
}

bool McpSecretRefIsDeveloperOnly(std::string_view value) {
  return value.rfind("literal:", 0) == 0;
}

bool LocalTokenStoreExplicitlyConfigured(const ManagerConfig& config) {
  return !config.security_token_store_path.empty();
}

std::optional<std::filesystem::path> McpSecretFileRefPath(std::string_view value) {
  constexpr std::string_view prefix = "file:";
  if (value.rfind(prefix, 0) != 0) return std::nullopt;
  return std::filesystem::path(std::string(value.substr(prefix.size())));
}

constexpr std::string_view kSecurityDatabaseTemporaryTokenProvider = "security_database_temporary_token";

std::string CanonicalizeSbsqlProfile(std::string_view value) {
  return value.empty() || IsSbsqlProfileAlias(value) ? std::string(kCanonicalSbsqlProfile) : std::string(value);
}

proto::Diagnostic Diag(std::string code, std::string message, std::vector<proto::Field> fields = {}) {
  return proto::MakeDiagnostic(std::move(code), std::move(message), std::move(fields));
}


std::string StateFile(const ManagerConfig& config) { return (config.control_dir / "sbmn_manager.lifecycle.state").string(); }
std::string OwnerFile(const ManagerConfig& config) { return (config.control_dir / "sbmn_manager.owner").string(); }
std::string PidFile(const ManagerConfig& config) { return (config.control_dir / "sbmn_manager.pid").string(); }
std::string ControlSocketPath(const ManagerConfig& config) { return (config.control_dir / "sbmn_manager.control.sock").string(); }
std::string AuditFile(const ManagerConfig& config) { return (config.control_dir / "sbmn_manager.audit.jsonl").string(); }
std::string MetricsFile(const ManagerConfig& config) { return (config.control_dir / "sbmn_manager.metrics.json").string(); }
std::filesystem::path SupportBundleRoot(const ManagerConfig& config) { return config.control_dir / "support-bundles"; }

std::string ChecksumText(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::string ChecksumBytes(const proto::Bytes& value) {
  if (value.empty()) return ChecksumText({});
  return ChecksumText(std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

std::string ScopeIdFromText(const std::string& value) {
  return "db-" + ChecksumText(value);
}

std::string ExtractJsonStringField(const std::string& text, const std::string& field) {
  const auto needle = "\"" + field + "\":\"";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) return {};
  std::string out;
  for (std::size_t i = pos + needle.size(); i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '"') return out;
    if (ch == '\\' && i + 1 < text.size()) {
      out.push_back(text[++i]);
      continue;
    }
    out.push_back(ch);
  }
  return {};
}

std::string NormalizeListenerManagementResponseText(std::string text) {
  if (const auto ack_hex = ExtractJsonStringField(text, "ack_hex"); !ack_hex.empty()) {
    return "lpreface_ack:" + ack_hex;
  }
  return text;
}

std::string ManagerOwnerDatabaseRuntimeScopeIdImpl(const ManagerConfig& config) {
  if (config.owner_database_uuid_set) {
    return ScopeIdFromText(proto::Hex(config.owner_database_uuid));
  }
  if (!config.owner_database_path.empty()) {
    return ScopeIdFromText(config.owner_database_path.lexically_normal().string());
  }
  if (!config.owner_database_name.empty()) {
    return ScopeIdFromText(config.owner_database_name);
  }
  return {};
}

std::filesystem::path DefaultManagerControlDir(const std::filesystem::path& runtime_dir,
                                               const std::string& scope_id) {
  auto path = runtime_dir / "manager";
  if (!scope_id.empty()) path = path / "databases" / scope_id;
  return path;
}

#ifndef _WIN32
bool ProcessExists(long pid) {
  if (pid <= 0) return false;
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
}

std::optional<long> ReadPidFromOwner(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("pid=", 0) == 0) {
      try { return std::stol(line.substr(4)); } catch (...) { return std::nullopt; }
    }
  }
  return std::nullopt;
}

std::map<std::string, std::string> ReadOwnerTokenFields(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::map<std::string, std::string> fields;
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq != std::string::npos) fields[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return fields;
}

std::optional<std::string> ReadLifecycleState(const ManagerConfig& config) {
  std::ifstream in(StateFile(config));
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("state=", 0) == 0) return line.substr(6);
  }
  return std::nullopt;
}

bool IsSafeFinalLifecycleState(const std::string& state) {
  return state == "stopped" || state == "startup_failed" || state == "failed_terminal";
}
#endif

#ifndef _WIN32
bool PrivateRegularFile(const std::filesystem::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  if (st.st_uid != ::geteuid()) return false;
  if (!S_ISREG(st.st_mode)) return false;
  return (st.st_mode & 0077u) == 0;
}
#else
bool PrivateRegularFile(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}
#endif

#ifndef _WIN32
bool WriteAll(int fd, const std::string& content) {
  const char* data = content.data();
  std::size_t remaining = content.size();
  while (remaining > 0) {
    const ssize_t written = ::write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (written == 0) return false;
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool SyncRuntimeDirectory(const std::filesystem::path& dir) {
#ifdef O_DIRECTORY
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
#endif
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  return ok;
}
#endif

bool WriteAtomicPrivateText(const std::filesystem::path& path, const std::string& content) {
  const auto dir = path.parent_path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return false;
  const auto tmp = dir / (path.filename().string() + ".tmp." +
#ifndef _WIN32
                          std::to_string(::getpid()) +
#else
                          std::string("win") +
#endif
                          "." + std::to_string(proto::CurrentEpochMilliseconds()));
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = WriteAll(fd, content);
  if (ok && ::fsync(fd) != 0) ok = false;
  if (::close(fd) != 0) ok = false;
  if (!ok) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  ::chmod(tmp.c_str(), 0600);
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  ::chmod(path.c_str(), 0600);
  return SyncRuntimeDirectory(dir);
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    out.flush();
    if (!out) {
      std::filesystem::remove(tmp, ec);
      return false;
    }
  }
  std::filesystem::remove(path, ec);
  ec.clear();
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
#endif
}

bool AppendDurablePrivateText(const std::filesystem::path& path, const std::string& record) {
  const auto dir = path.parent_path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return false;
#ifndef _WIN32
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = WriteAll(fd, record);
  if (ok && ::fsync(fd) != 0) ok = false;
  if (::close(fd) != 0) ok = false;
  if (!ok) return false;
  ::chmod(path.c_str(), 0600);
  return SyncRuntimeDirectory(dir);
#else
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return false;
  out << record;
  out.flush();
  return static_cast<bool>(out);
#endif
}

bool ParseManagerRightsStrict(const std::string& rights_text, std::vector<std::string>* rights);
bool ManagerRightIsWildcardGrant(const std::string& right);

void AddEnterpriseSecretPolicyDiagnostics(const ManagerConfig& config,
                                          std::vector<proto::Diagnostic>* diagnostics) {
  if (!config.mcp_secret_rights.empty()) {
    std::vector<std::string> rights;
    if (!ParseManagerRightsStrict(config.mcp_secret_rights, &rights)) {
      diagnostics->push_back(Diag("MANAGER.CONFIG_FIELD_INVALID",
                                  "Manager MCP secret rights list is invalid.",
                                  {{"key", "manager.auth.mcp_secret_rights"}}));
    }
  }
  if (ReleaseProfileAllowsDeveloperSecret(config.release_profile)) return;
  if (!config.mcp_secret_ref.empty()) {
    std::vector<std::string> rights;
    if (config.mcp_secret_rights.empty()) {
      diagnostics->push_back(Diag("MANAGER.MCP_SECRET_RIGHTS_REQUIRED",
                                  "Enterprise manager MCP secrets require explicit command-scoped rights.",
                                  {{"key", "manager.auth.mcp_secret_rights"}}));
    } else if (ParseManagerRightsStrict(config.mcp_secret_rights, &rights)) {
      for (const auto& right : rights) {
        if (ManagerRightIsWildcardGrant(right)) {
          diagnostics->push_back(Diag("MANAGER.RELEASE_PROFILE_FORBIDS_WILDCARD_SECRET_RIGHT",
                                      "Enterprise manager MCP secrets cannot grant wildcard manager rights.",
                                      {{"right", right}}));
          break;
        }
      }
    }
  }
  if (const auto secret_path = McpSecretFileRefPath(config.mcp_secret_ref);
      secret_path && !PrivateRegularFile(*secret_path)) {
    diagnostics->push_back(Diag("MANAGER.SECRET_FILE_UNSAFE",
                                "Enterprise manager secret file is missing, unsafe, or not private.",
                                {{"secret_ref", "file:[path-redacted]"}}));
  }
  if (config.listener_control_socket_dir.empty()) return;
  if (config.dbbt_keyring_path.empty()) {
    diagnostics->push_back(Diag("MANAGER.SECRET_REQUIRED",
                                "Enterprise listener-control mode requires manager.dbbt.keyring_path."));
  } else if (!PrivateRegularFile(config.dbbt_keyring_path)) {
    diagnostics->push_back(Diag("MANAGER.DBBT_KEYRING_FILE_UNSAFE",
                                "Enterprise DBBT keyring file is missing, unsafe, or not private.",
                                {{"keyring_path", "[path-redacted]"}}));
  }
  if (!config.owner_database_uuid_set) {
    diagnostics->push_back(Diag("MANAGER.SECRET_REQUIRED",
                                "Enterprise listener-control mode requires manager.owner.database_uuid."));
  }
  if (config.listener_id == 0) {
    diagnostics->push_back(Diag("MANAGER.CONFIG_FIELD_INVALID",
                                "Enterprise listener-control mode requires a nonzero listener id."));
  }
}

bool ParseBool(const std::string& value, bool* out) {
  if (value == "true" || value == "1" || value == "yes" || value == "on") { *out = true; return true; }
  if (value == "false" || value == "0" || value == "no" || value == "off") { *out = false; return true; }
  return false;
}

bool ParseUnsignedDecimal(const std::string& value, std::uint64_t max_value, std::uint64_t* out) {
  if (value.empty()) return false;
  std::uint64_t parsed = 0;
  for (unsigned char ch : value) {
    if (!std::isdigit(ch)) return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (max_value - digit) / 10u) return false;
    parsed = parsed * 10u + digit;
  }
  *out = parsed;
  return true;
}

bool ParseU16(const std::string& value, std::uint16_t* out) {
  std::uint64_t n = 0;
  if (!ParseUnsignedDecimal(value, std::numeric_limits<std::uint16_t>::max(), &n) || n == 0) return false;
  *out = static_cast<std::uint16_t>(n);
  return true;
}

bool ParseU32(const std::string& value, std::uint32_t* out) {
  std::uint64_t n = 0;
  if (!ParseUnsignedDecimal(value, std::numeric_limits<std::uint32_t>::max(), &n)) return false;
  *out = static_cast<std::uint32_t>(n);
  return true;
}

bool ParseU64(const std::string& value, std::uint64_t* out) {
  return ParseUnsignedDecimal(value, std::numeric_limits<std::uint64_t>::max(), out);
}

bool IsClusterOnlyManagerOperation(const std::string& operation) {
  return operation == "manager.probe_cluster" ||
         operation == "manager.join_cluster" ||
         operation == "manager.negotiate_admission" ||
         operation == "manager.route_cluster_command" ||
         operation.rfind("cluster.", 0) == 0 ||
         operation.rfind("sbmc.", 0) == 0 ||
         operation.rfind("manager.cluster_", 0) == 0;
}

bool ParseUuidHex(std::string value, proto::UuidBytes* out) {
  value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
  const auto bytes = proto::FromHex(value);
  if (bytes.size() != out->size()) return false;
  std::copy(bytes.begin(), bytes.end(), out->begin());
  return true;
}


std::string Trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

void ApplyDefaultPaths(ManagerConfig* config) {
  const char* home = std::getenv("SCRATCHBIRD_HOME");
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  const bool control_dir_was_empty = config->control_dir.empty();
  if (config->config_path.empty()) {
    if (const char* env_config = std::getenv("SCRATCHBIRD_MANAGER_CONFIG")) config->config_path = env_config;
    else if (home) config->config_path = std::filesystem::path(home) / "conf" / "sbmn_manager.conf";
    else config->config_path = "/etc/scratchbird/sbmn_manager.conf";
  }
  if (config->runtime_dir.empty()) {
    if (xdg) config->runtime_dir = std::filesystem::path(xdg) / "scratchbird";
    else config->runtime_dir = "/run/scratchbird";
  }
  if (config->owner_database_runtime_scope_id.empty()) {
    config->owner_database_runtime_scope_id = ManagerOwnerDatabaseRuntimeScopeIdImpl(*config);
  }
  if (config->owner_database_name.empty() && !config->owner_database_path.empty()) {
    config->owner_database_name = config->owner_database_path.string();
  }
  if (config->control_dir.empty()) {
    config->control_dir = DefaultManagerControlDir(config->runtime_dir,
                                                   config->owner_database_runtime_scope_id);
  }
  if (control_dir_was_empty && !config->owner_database_runtime_scope_id.empty()) {
    config->control_dir = DefaultManagerControlDir(config->runtime_dir,
                                                   config->owner_database_runtime_scope_id);
  }
  if (config->log_path.empty()) config->log_path = config->foreground || config->validate_config ? "stderr" : (config->runtime_dir / "logs" / "sbmn_manager.log");
}

bool ApplyKeyValue(ManagerConfig* config, const std::string& key, const std::string& value, std::vector<proto::Diagnostic>* diagnostics) {
  auto invalid = [&]() {
    diagnostics->push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager configuration field is invalid.", {{"key", key}}));
    return false;
  };
  auto parse_nonzero_u32 = [&](std::uint32_t* out) {
    std::uint32_t parsed = 0;
    if (!ParseU32(value, &parsed) || parsed == 0) return false;
    *out = parsed;
    return true;
  };
  auto parse_nonzero_u64 = [&](std::uint64_t* out) {
    std::uint64_t parsed = 0;
    if (!ParseU64(value, &parsed) || parsed == 0) return false;
    *out = parsed;
    return true;
  };
  if (key == "manager.proxy.enabled") return ParseBool(value, &config->proxy_enabled) || invalid();
  if (key == "manager.proxy.bind") { config->bind_address = value; return true; }
  if (key == "manager.proxy.port") return ParseU16(value, &config->proxy_port) || invalid();
  if (key == "manager.proxy.backlog") return ParseU32(value, &config->proxy_backlog) || invalid();
  if (key == "manager.proxy.max_clients") return ParseU32(value, &config->proxy_max_clients) || invalid();
  if (key == "manager.proxy.client_idle_timeout_ms") return ParseU64(value, &config->proxy_client_idle_timeout_ms) || invalid();
  if (key == "manager.proxy.backend_connect_timeout_ms") return ParseU64(value, &config->proxy_backend_connect_timeout_ms) || invalid();
  if (key == "manager.proxy.io_timeout_ms") return ParseU64(value, &config->proxy_io_timeout_ms) || invalid();
  if (key == "manager.control.backlog") return parse_nonzero_u32(&config->management_backlog) || invalid();
  if (key == "manager.control.max_clients") return parse_nonzero_u32(&config->management_max_clients) || invalid();
  if (key == "manager.control.max_payload_bytes") {
    return parse_nonzero_u32(&config->management_max_payload_bytes) &&
           config->management_max_payload_bytes <= 16u * 1024u * 1024u
               ? true
               : invalid();
  }
  if (key == "manager.control.idle_timeout_ms") return parse_nonzero_u64(&config->management_idle_timeout_ms) || invalid();
  if (key == "manager.backend.native_bind") { config->native_bind = value; return true; }
  if (key == "manager.backend.native_port") return ParseU16(value, &config->native_port) || invalid();
  if (key == "manager.owner.database_name") { config->owner_database_name = value; return !value.empty() || invalid(); }
  if (key == "manager.owner.database_path" || key == "manager.default_database.path") {
    config->owner_database_path = std::filesystem::absolute(value).lexically_normal();
    if (config->owner_database_name.empty() || config->owner_database_name == "main") {
      config->owner_database_name = config->owner_database_path.string();
    }
    return true;
  }
  if (key == "manager.owner.database_uuid") { config->owner_database_uuid_set = ParseUuidHex(value, &config->owner_database_uuid); return config->owner_database_uuid_set || invalid(); }
  if (key == "manager.security.temporary_token_store_path") { config->security_token_store_path = value; return true; }
  if (key == "manager.listener.default_id") return ParseU32(value, &config->listener_id) || invalid();
  if (key == "manager.listener.control_socket_dir") { config->listener_control_socket_dir = value; return true; }
  if (key == "manager.dbbt.ttl_ms") return ParseU64(value, &config->dbbt_ttl_ms) || invalid();
  if (key == "manager.dbbt.clock_skew_ms") return ParseU64(value, &config->dbbt_clock_skew_ms) || invalid();
  if (key == "manager.dbbt.replay_cache_entries") return ParseU32(value, &config->dbbt_replay_cache_entries) || invalid();
  if (key == "manager.dbbt.keyring_path") { config->dbbt_keyring_path = value; return true; }
  if (key == "manager.auth.mcp_secret_ref") { config->mcp_secret_ref = value; return true; }
  if (key == "manager.auth.mcp_secret_rights") {
    std::vector<std::string> rights;
    if (!value.empty() && ParseManagerRightsStrict(value, &rights)) {
      config->mcp_secret_rights = value;
      return true;
    }
    return invalid();
  }
  if (key == "manager.server.heartbeat_interval_ms") return ParseU64(value, &config->heartbeat_interval_ms) || invalid();
  if (key == "manager.server.heartbeat_timeout_ms") return ParseU64(value, &config->heartbeat_timeout_ms) || invalid();
  if (key == "manager.server.missed_heartbeat_threshold") return ParseU32(value, &config->missed_heartbeat_threshold) || invalid();
  if (key == "manager.server.restart.enabled") return ParseBool(value, &config->restart_enabled) || invalid();
  if (key == "manager.server.restart.max_attempts") return ParseU32(value, &config->restart_max_attempts) || invalid();
  if (key == "manager.server.restart.window_ms") return ParseU64(value, &config->restart_window_ms) || invalid();
  if (key == "manager.server.restart.initial_backoff_ms") return ParseU64(value, &config->restart_initial_backoff_ms) || invalid();
  if (key == "manager.server.restart.max_backoff_ms") return ParseU64(value, &config->restart_max_backoff_ms) || invalid();
  if (key == "manager.server.restart.executable") { config->restart_executable = value; return RestartExecutableValid(value) || invalid(); }
  if (key == "manager.server.restart.arguments") { config->restart_arguments = value; return RestartArgumentsValid(value) || invalid(); }
  if (key == "manager.third_party.enabled") return ParseBool(value, &config->third_party_management_enabled) || invalid();
  if (key == "manager.threading.no_spin_required") return ParseBool(value, &config->no_spin_required) || invalid();
  if (key == "manager.release.profile") { config->release_profile = value; return ManagerReleaseProfileValid(value) || invalid(); }
  if (key == "manager.runtime_dir") { config->runtime_dir = value; return true; }
  if (key == "manager.control_dir") { config->control_dir = value; return true; }
  if (key == "manager.log.path") { config->log_path = value; return true; }
  if (key == "manager.log.level") { config->log_level = value; return true; }
  diagnostics->push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Unknown manager configuration key is forbidden.", {{"key", key}}));
  return false;
}

void LoadConfigFile(ManagerConfig* config, std::vector<proto::Diagnostic>* diagnostics, bool explicit_config) {
  if (config->config_path.empty()) return;
  if (!std::filesystem::exists(config->config_path)) {
    if (explicit_config) diagnostics->push_back(Diag("MANAGER.CONFIG_LOAD_FAILED", "Manager configuration file could not be loaded.", {{"path", config->config_path.string()}}));
    return;
  }
  std::ifstream in(config->config_path);
  if (!in) {
    diagnostics->push_back(Diag("MANAGER.CONFIG_LOAD_FAILED", "Manager configuration file could not be opened.", {{"path", config->config_path.string()}}));
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    line = Trim(line);
    if (line.empty()) continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      diagnostics->push_back(Diag("MANAGER.CONFIG_VALIDATION_FAILED", "Manager configuration line is malformed."));
      continue;
    }
    ApplyKeyValue(config, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)), diagnostics);
  }
}

#ifndef _WIN32
bool RecvExact(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t got = 0;
  while (got < size) {
    const auto rc = ::recv(fd, data + got, size - got, 0);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (rc == 0) return false;
    got += static_cast<std::size_t>(rc);
  }
  return true;
}

bool RecvExactWithTimeout(int fd, std::uint8_t* data, std::size_t size, std::uint64_t timeout_ms) {
  std::size_t got = 0;
  const int timeout = timeout_ms > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                          ? std::numeric_limits<int>::max()
                          : static_cast<int>(timeout_ms);
  while (got < size) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (ready == 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
    if ((pfd.revents & POLLIN) == 0) continue;
    const auto rc = ::recv(fd, data + got, size - got, 0);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (rc == 0) return false;
    got += static_cast<std::size_t>(rc);
  }
  return true;
}

int CreateTcpListener(const std::string& bind_address, std::uint16_t port, int backlog, std::vector<proto::Diagnostic>* diagnostics) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const auto port_text = std::to_string(port);
  const int gai = ::getaddrinfo(bind_address.c_str(), port_text.c_str(), &hints, &results);
  if (gai != 0) {
    diagnostics->push_back(Diag("MANAGER.PROXY_BIND_INVALID", "Proxy bind address is invalid.", {{"address", bind_address}, {"port", port_text}}));
    return -1;
  }
  int fd = -1;
  for (auto* ai = results; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, backlog) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);
  if (fd < 0) diagnostics->push_back(Diag("MANAGER.PROXY_BIND_FAILED", "Proxy endpoint could not be bound.", {{"address", bind_address}, {"port", port_text}}));
  return fd;
}

int ConnectTcp(const std::string& address, std::uint16_t port, std::uint64_t timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const auto port_text = std::to_string(port);
  if (::getaddrinfo(address.c_str(), port_text.c_str(), &hints, &results) != 0) return -1;
  int fd = -1;
  for (auto* ai = results; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0 || errno == EINPROGRESS) {
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      if (::poll(&pfd, 1, static_cast<int>(timeout_ms)) > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
          ::fcntl(fd, F_SETFL, flags);
          break;
        }
      }
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);
  return fd;
}

bool SendAll(int fd, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const auto rc = ::send(fd, data + sent, size - sent, 0);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (rc == 0) return false;
    sent += static_cast<std::size_t>(rc);
  }
  return true;
}

bool SendBytes(int fd, const proto::Bytes& bytes) {
  return SendAll(fd, reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool DaemonizeService(std::vector<proto::Diagnostic>* diagnostics) {
  const pid_t first = ::fork();
  if (first < 0) {
    diagnostics->push_back(Diag("MANAGER.SERVICE_MODE_UNSUPPORTED", "Manager service fork failed."));
    return false;
  }
  if (first > 0) _exit(0);
  if (::setsid() < 0) {
    diagnostics->push_back(Diag("MANAGER.SERVICE_MODE_UNSUPPORTED", "Manager service session handoff failed."));
    return false;
  }
  const pid_t second = ::fork();
  if (second < 0) {
    diagnostics->push_back(Diag("MANAGER.SERVICE_MODE_UNSUPPORTED", "Manager service second fork failed."));
    return false;
  }
  if (second > 0) _exit(0);
  ::umask(0027);
  if (::chdir("/") != 0) {
    diagnostics->push_back(Diag("MANAGER.SERVICE_MODE_UNSUPPORTED", "Manager service chdir failed."));
    return false;
  }
  const int devnull = ::open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    (void)::dup2(devnull, STDIN_FILENO);
    (void)::dup2(devnull, STDOUT_FILENO);
    (void)::dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) ::close(devnull);
  }
  return true;
}

void PutU16(proto::Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(proto::Bytes* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

std::uint16_t ReadU16(const proto::Bytes& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const proto::Bytes& data, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[off + i]) << (8 * i);
  return v;
}

void PutLpstr(proto::Bytes* out, const std::string& value) {
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadLpstr(const proto::Bytes& data, std::size_t* off, std::string* out) {
  if (!off || !out || *off + 4 > data.size()) return false;
  const auto len = ReadU32(data, *off);
  *off += 4;
  if (len > 4096 || *off + len > data.size()) return false;
  out->assign(data.begin() + static_cast<std::ptrdiff_t>(*off), data.begin() + static_cast<std::ptrdiff_t>(*off + len));
  *off += len;
  return true;
}

proto::Bytes StatusPayload(std::uint8_t request_type, const std::vector<std::pair<std::string, std::string>>& entries) {
  proto::Bytes out;
  out.push_back(request_type);
  PutU32(&out, static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    PutLpstr(&out, entry.first);
    PutLpstr(&out, entry.second);
  }
  return out;
}

std::string GetFieldValue(const std::vector<std::pair<std::string, std::string>>& fields,
                          const std::string& key,
                          const std::string& fallback = {}) {
  for (const auto& field : fields) {
    if (field.first == key) return field.second;
  }
  return fallback;
}

bool ManagerCommandArgsMatchAllowlist(const std::vector<std::pair<std::string, std::string>>& args,
                                      const std::vector<std::string_view>& allowed,
                                      std::string* field,
                                      std::string* reason) {
  std::vector<std::string_view> seen;
  for (const auto& arg : args) {
    const auto allowed_it = std::find(allowed.begin(), allowed.end(), arg.first);
    if (allowed_it == allowed.end()) {
      if (field != nullptr) *field = arg.first.empty() ? std::string("[empty]") : arg.first;
      if (reason != nullptr) *reason = "unknown_argument";
      return false;
    }
    if (std::find(seen.begin(), seen.end(), std::string_view(arg.first)) != seen.end()) {
      if (field != nullptr) *field = arg.first;
      if (reason != nullptr) *reason = "duplicate_argument";
      return false;
    }
    seen.push_back(arg.first);
  }
  return true;
}

bool ManagerCommandArgsValid(const std::string& operation,
                             const std::vector<std::pair<std::string, std::string>>& args,
                             std::string* field,
                             std::string* reason) {
  if (operation == "manager.status" || operation == "thirdparty.status_export") {
    return ManagerCommandArgsMatchAllowlist(args, {}, field, reason);
  }
  if (operation == "support.bundle_generate") {
    return ManagerCommandArgsMatchAllowlist(args, {"scope", "redaction_profile"}, field, reason);
  }
  if (operation == "manager.validate_config" || operation == "manager.reload_config") {
    return ManagerCommandArgsMatchAllowlist(args, {"config_ref"}, field, reason);
  }
  if (operation == "listener.stop") {
    if (!ManagerCommandArgsMatchAllowlist(args, {"force"}, field, reason)) return false;
    const std::string force = GetFieldValue(args, "force");
    if (!force.empty() && force != "true" && force != "false") {
      if (field != nullptr) *field = "force";
      if (reason != nullptr) *reason = "invalid_value";
      return false;
    }
    return true;
  }
  if (operation == "listener.list" ||
      operation == "listener.status" ||
      operation == "listener.start" ||
      operation == "listener.restart" ||
      operation == "listener.drain" ||
      operation == "listener.undrain" ||
      operation == "listener.reload") {
    return ManagerCommandArgsMatchAllowlist(args, {}, field, reason);
  }
  return true;
}

bool ManagerOperationRequiresIdempotency(const std::string& operation) {
  if (operation == "support.bundle_generate" || operation == "manager.reload_config") return true;
  if (operation == "listener.list" || operation == "listener.status") return false;
  if (operation.rfind("listener.", 0) == 0) return true;
  return false;
}

bool ManagerIdempotencyKeyValid(const std::string& key, std::string* reason) {
  auto fail = [&](std::string value) {
    if (reason != nullptr) *reason = std::move(value);
    return false;
  };
  if (key.empty()) return fail("missing");
  if (key.size() > 128) return fail("too_long");
  for (unsigned char ch : key) {
    const bool ok = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == ':';
    if (!ok) return fail("invalid_character");
  }
  return true;
}

bool ManagerSupportBundleLabelValid(const std::string& value) {
  if (value.empty() || value.size() > 64) return false;
  for (unsigned char ch : value) {
    const bool ok = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
    if (!ok) return false;
  }
  return true;
}

bool ManagerSupportBundleRedactionProfileValid(const std::string& value) {
  return ManagerSupportBundleLabelValid(value) &&
         (value == "default" || value == "public-redacted" || value == "enterprise-redacted");
}

std::filesystem::path ManagerLexicallyNormalAbsolutePath(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal();
}

bool ManagerConfigReferenceValid(const ManagerConfig& config,
                                 const std::string& config_ref,
                                 std::string* reason) {
  auto fail = [&](std::string value) {
    if (reason != nullptr) *reason = std::move(value);
    return false;
  };
  if (config_ref.empty()) return true;
  if (config_ref.size() > 4096) return fail("too_long");
  for (unsigned char ch : config_ref) {
    if (ch == '\0' || ch == '\n' || ch == '\r' || ch == '\t') return fail("invalid_text");
  }
  if (config.config_path.empty()) return fail("no_current_config");
  const std::filesystem::path requested(config_ref);
  if (!requested.is_absolute()) return fail("relative_path");
  if (ManagerLexicallyNormalAbsolutePath(requested) !=
      ManagerLexicallyNormalAbsolutePath(config.config_path)) {
    return fail("outside_current_config");
  }
  return true;
}

std::string ConfigReferenceResponseValue(const std::string& config_ref) {
  return config_ref.empty() ? std::string{} : std::string("[path-redacted]");
}

std::string DiagnosticCodeFromMessage(const std::string& message) {
  if (message.empty()) return "MANAGER.REQUEST_FAILED";
  bool code_like = false;
  for (char ch : message) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_';
    if (!ok) return "MANAGER.REQUEST_FAILED";
    if (ch == '.') code_like = true;
  }
  return code_like ? message : "MANAGER.REQUEST_FAILED";
}

void AppendMessageVectorSet(proto::Bytes* out, const proto::Diagnostic& diagnostic) {
  proto::MessageVectorSet set;
  set.request_uuid = proto::MakePseudoUuidV7();
  set.diagnostics.push_back(diagnostic);
  proto::Bytes encoded;
  const auto result = proto::EncodeMessageVectorSetV1(set, &encoded, 0, 4096);
  if (!result.ok) {
    PutU32(out, 0);
    return;
  }
  PutU32(out, static_cast<std::uint32_t>(encoded.size()));
  out->insert(out->end(), encoded.begin(), encoded.end());
}

proto::Bytes ProtocolErrorPayload(const std::string& message) {
  proto::Bytes out;
  PutLpstr(&out, message);
  AppendMessageVectorSet(&out, Diag(DiagnosticCodeFromMessage(message), message));
  return out;
}

std::string EndpointText(const ManagerConfig& config) {
  return config.native_bind + ":" + std::to_string(config.native_port);
}

proto::Bytes HelloResponsePayload(const ManagerConfig& config, const proto::Bytes& request_payload) {
  std::uint16_t requested_version = 0;
  std::uint16_t client_flags = 0;
  if (request_payload.size() >= 4) {
    requested_version = ReadU16(request_payload, 0);
    client_flags = ReadU16(request_payload, 2);
  }
  bool ready = false;
  const int backend_fd = ConnectTcp(config.native_bind, config.native_port, config.proxy_backend_connect_timeout_ms);
  if (backend_fd >= 0) {
    ready = true;
    ::close(backend_fd);
  }
  return StatusPayload(0x01, {
      {"mcp_version", std::to_string(kMcpProtocolVersion)},
      {"requested_version", std::to_string(requested_version)},
      {"client_flags", std::to_string(client_flags)},
      {"auth_flow", "token_auth_start_auth_continue"},
      {"database_owner", config.owner_database_name},
      {"internal_native_endpoint", EndpointText(config)},
      {"ready", ready ? "true" : "false"},
      {"product", "sbmn_manager"},
      {"db_connect_extended_magic", std::string(kMcpDbConnectExtendedMagic)},
      {"db_connect_default_profile", std::string(kCanonicalSbsqlProfile)},
      {"connect_flag_base_capabilities", std::to_string(kConnectFlagBaseCapabilities)},
      {"connect_flag_manager_dbbt", std::to_string(kConnectFlagManagerDbbt)},
      {"negative_response_message_vector", "MessageVectorSetV1"},
      {"release_profile", config.release_profile},
      {"direct_native_bypass", !config.listener_control_socket_dir.empty()
                                   ? "disabled"
                                   : (ReleaseProfileAllowsDirectNative(config.release_profile) ? "enabled" : "forbidden")},
  });
}

proto::Bytes DbListResponsePayload(const ManagerConfig& config) {
  return StatusPayload(0x03, {
      {"count", "1"},
      {"db.0", config.owner_database_name},
      {"default_db", config.owner_database_name},
      {"internal_native_endpoint", EndpointText(config)},
  });
}

bool TimingSafeEqual(const proto::Bytes& lhs, const std::string& rhs) {
  const auto rhs_size = rhs.size();
  std::uint8_t diff = static_cast<std::uint8_t>(lhs.size() ^ rhs_size);
  const std::size_t n = std::max(lhs.size(), rhs_size);
  for (std::size_t i = 0; i < n; ++i) {
    const auto a = i < lhs.size() ? lhs[i] : 0;
    const auto b = i < rhs_size ? static_cast<std::uint8_t>(rhs[i]) : 0;
    diff = static_cast<std::uint8_t>(diff | (a ^ b));
  }
  return diff == 0;
}

bool TemporaryTokenBytesValid(const proto::Bytes& token) {
  if (token.empty() || token.size() > 1024) return false;
  for (const auto ch : token) {
    const bool ok = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    if (!ok) return false;
  }
  return true;
}

std::filesystem::path ManagerSecurityTokenStorePath(const ManagerConfig& config) {
  if (!config.security_token_store_path.empty()) return config.security_token_store_path;
  if (!config.owner_database_path.empty()) {
    return std::filesystem::path(config.owner_database_path.string() + ".sb.temporary_auth_tokens");
  }
  return {};
}

bool ManagerRightNameValid(const std::string& right) {
  if (right.empty() || right.size() > 128) return false;
  for (unsigned char ch : right) {
    const bool ok = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '*';
    if (!ok) return false;
  }
  return true;
}

std::vector<std::string> SplitManagerRights(const std::string& rights_text) {
  std::vector<std::string> rights;
  std::size_t start = 0;
  while (start <= rights_text.size()) {
    const auto comma = rights_text.find(',', start);
    auto right = Trim(rights_text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!right.empty() && ManagerRightNameValid(right)) rights.push_back(std::move(right));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return rights;
}

std::vector<std::string> DeveloperMcpSecretRights() {
  return {"*"};
}

bool SecurityEvidenceFieldValueValid(const std::string& value) {
  if (value.empty() || value.size() > 256) return false;
  for (const unsigned char ch : value) {
    if (ch <= 0x20 || ch == ';' || ch == '=') return false;
  }
  return true;
}

std::string TemporaryTokenDigestHex(const std::string& token) {
  proto::Bytes bytes(token.begin(), token.end());
  const auto digest = proto::Sha256(bytes);
  return proto::Hex(digest.data(), digest.size());
}

std::string BuildEngineTemporaryTokenEvidenceFragment(const std::string& token,
                                                      const std::string& principal_uuid,
                                                      const std::string& token_handle,
                                                      const std::string& storage_authority,
                                                      const std::string& state,
                                                      const std::string& expires_at_ms,
                                                      const std::string& authorization_tags) {
  std::string fragment = token;
  if (principal_uuid.empty() && token_handle.empty() && storage_authority.empty()) return fragment;
  const std::string authority = storage_authority.empty() ? "mga_security_principal_lifecycle" : storage_authority;
  const std::string row_state = state.empty() ? "active" : state;
  const std::string row_expires = expires_at_ms.empty() ? "0" : expires_at_ms;
  fragment += ";principal_uuid=" + principal_uuid;
  fragment += ";storage_authority=" + authority;
  fragment += ";token_handle=" + token_handle;
  fragment += ";token_digest=" + TemporaryTokenDigestHex(token);
  fragment += ";state=" + row_state;
  fragment += ";expires_at_ms=" + row_expires;
  if (!authorization_tags.empty()) fragment += ";authorization_tags=" + authorization_tags;
  return fragment;
}

bool RightsGrantDatabaseConnect(const std::vector<std::string>& rights) {
  for (const auto& right : rights) {
    if (right == "*" || right == "database.connect" || right == "database.*") return true;
  }
  return false;
}

bool ParseManagerRightsStrict(const std::string& rights_text, std::vector<std::string>* rights) {
  if (rights != nullptr) rights->clear();
  std::size_t start = 0;
  while (start <= rights_text.size()) {
    const auto comma = rights_text.find(',', start);
    auto right = Trim(rights_text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (right.empty() || !ManagerRightNameValid(right)) return false;
    if (rights != nullptr) rights->push_back(std::move(right));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return true;
}

bool ManagerRightIsWildcardGrant(const std::string& right) {
  if (right == "*") return true;
  constexpr std::string_view suffix = ".*";
  return right.size() > suffix.size() &&
         right.compare(right.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> McpSecretGrantedRights(const ManagerConfig& config) {
  if (config.mcp_secret_rights.empty()) {
    return ReleaseProfileAllowsDeveloperSecret(config.release_profile)
               ? DeveloperMcpSecretRights()
               : std::vector<std::string>{};
  }
  std::vector<std::string> rights;
  if (!ParseManagerRightsStrict(config.mcp_secret_rights, &rights)) return {};
  return rights;
}

bool ValidateManagerSecurityToken(const ManagerConfig& config,
                                  const std::string& username,
                                  const proto::Bytes& token,
                                  std::vector<std::string>* granted_rights,
                                  std::string* engine_token_fragment,
                                  std::string* failure_detail) {
  if (granted_rights != nullptr) granted_rights->clear();
  if (engine_token_fragment != nullptr) engine_token_fragment->clear();
  auto fail = [&](std::string detail) {
    if (failure_detail != nullptr) *failure_detail = std::move(detail);
    return false;
  };
  if (username.empty()) return fail("principal_required");
  if (!TemporaryTokenBytesValid(token)) return fail("security_database_temporary_token_evidence_required");
  if (!ReleaseProfileAllowsLocalTokenStore(config.release_profile)) {
    return fail("security_database_temporary_token_store_forbidden_by_release_profile");
  }
  const auto store_path = ManagerSecurityTokenStorePath(config);
  if (store_path.empty()) return fail("security_database_temporary_token_store_unconfigured");
  std::ifstream in(store_path);
  if (!in) return fail("security_database_temporary_token_store_missing");
  const auto now_ms = proto::CurrentEpochMilliseconds();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    std::string row_token;
    std::string row_principal;
    std::string row_expires;
    std::string row_state;
    std::string row_rights;
    if (!std::getline(row, row_token, '\t') ||
        !std::getline(row, row_principal, '\t') ||
        !std::getline(row, row_expires, '\t')) {
      continue;
    }
    if (!std::getline(row, row_state, '\t')) row_state = "active";
    if (!std::getline(row, row_rights, '\t')) row_rights.clear();
    std::string row_principal_uuid;
    std::string row_token_handle;
    std::string row_storage_authority;
    if (!std::getline(row, row_principal_uuid, '\t')) row_principal_uuid.clear();
    if (!std::getline(row, row_token_handle, '\t')) row_token_handle.clear();
    if (!std::getline(row, row_storage_authority, '\t')) row_storage_authority.clear();
    if (!TimingSafeEqual(token, row_token)) continue;
    if (row_state != "active" && row_state != "valid") {
      return fail("security_database_temporary_token_revoked");
    }
    if (row_principal != username) {
      return fail("security_database_temporary_token_principal_mismatch");
    }
    std::uint64_t expires_at_ms = 0;
    try {
      expires_at_ms = static_cast<std::uint64_t>(std::stoull(row_expires));
    } catch (...) {
      return fail("security_database_temporary_token_expiry_invalid");
    }
    if (expires_at_ms != 0 && expires_at_ms < now_ms) {
      return fail("security_database_temporary_token_expired");
    }
    const auto rights = SplitManagerRights(row_rights);
    if (rights.empty()) return fail("security_database_temporary_token_rights_required");
    const bool has_durable_metadata = !row_principal_uuid.empty() ||
                                      !row_token_handle.empty() ||
                                      !row_storage_authority.empty();
    if (has_durable_metadata &&
        (!SecurityEvidenceFieldValueValid(row_principal_uuid) ||
         !SecurityEvidenceFieldValueValid(row_token_handle) ||
         (!row_storage_authority.empty() && !SecurityEvidenceFieldValueValid(row_storage_authority)))) {
      return fail("security_database_temporary_token_durable_metadata_invalid");
    }
    if (granted_rights != nullptr) *granted_rights = rights;
    if (engine_token_fragment != nullptr) {
      *engine_token_fragment = BuildEngineTemporaryTokenEvidenceFragment(row_token,
                                                                        row_principal_uuid,
                                                                        row_token_handle,
                                                                        row_storage_authority,
                                                                        row_state,
                                                                        row_expires,
                                                                        RightsGrantDatabaseConnect(rights)
                                                                            ? "right:CONNECT"
                                                                            : "");
    }
    if (failure_detail != nullptr) failure_detail->clear();
    return true;
  }
  return fail("security_database_temporary_token_not_found");
}

std::string ResolveMcpSecret(const ManagerConfig& config) {
  constexpr std::string_view env_prefix = "env:";
  constexpr std::string_view file_prefix = "file:";
  if (config.mcp_secret_ref.rfind(std::string(env_prefix), 0) == 0) {
    auto name = config.mcp_secret_ref.substr(env_prefix.size());
    const auto colon = name.find(':');
    if (colon != std::string::npos) name.resize(colon);
    if (const char* value = std::getenv(name.c_str())) return value;
    return {};
  }
  if (config.mcp_secret_ref.rfind(std::string(file_prefix), 0) == 0) {
    std::ifstream in(config.mcp_secret_ref.substr(file_prefix.size()));
    std::string value;
    std::getline(in, value);
    return value;
  }
  if (config.mcp_secret_ref.rfind("literal:", 0) == 0) return config.mcp_secret_ref.substr(8);
  return config.mcp_secret_ref;
}


void PutNpstr(proto::Bytes* out, const std::string& value, std::size_t width) {
  const auto n = std::min(value.size(), width);
  out->insert(out->end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(n));
  out->insert(out->end(), width - n, 0);
}

proto::Bytes AuthResponsePayload(std::uint8_t status, const std::string& error_message) {
  proto::Bytes out;
  out.push_back(status);
  PutU32(&out, 0);
  PutNpstr(&out, error_message, 256);
  if (status != 0) {
    AppendMessageVectorSet(&out, Diag(DiagnosticCodeFromMessage(error_message), error_message));
  }
  return out;
}

proto::Bytes AuthChallengePayload(const proto::UuidBytes& session_id, const std::string& username) {
  proto::Bytes out;
  out.insert(out.end(), session_id.begin(), session_id.end());
  PutNpstr(&out, username, 64);
  out.push_back(1);
  out.push_back(1);
  out.push_back(4);
  out.push_back(1);
  out.push_back(4);
  out.push_back(0x01);
  out.push_back(0);
  PutU16(&out, static_cast<std::uint16_t>(session_id.size()));
  out.insert(out.end(), session_id.begin(), session_id.end());
  return out;
}

proto::Bytes DbInfoResponsePayload(const ManagerConfig& config, const std::string& database_name) {
  const bool available = database_name == config.owner_database_name;
  return StatusPayload(0x03, {
      {"db", database_name},
      {"available", available ? "true" : "false"},
      {"state", "CLOSED"},
      {"ready", "false"},
      {"internal_native_endpoint", EndpointText(config)},
      {"product", "sbmn_manager"},
  });
}

proto::Bytes ConnectResponsePayload(bool failure,
                                    const proto::UuidBytes& session_id,
                                    std::uint16_t server_flags,
                                    const std::string& error_message,
                                    const std::string& diagnostic_code = {}) {
  proto::Bytes out;
  out.push_back(failure ? 1 : 0);
  PutU16(&out, 0x0101);
  PutU16(&out, server_flags);
  out.insert(out.end(), session_id.begin(), session_id.end());
  PutNpstr(&out, "ScratchBird", 64);
  PutNpstr(&out, "1.0.0-alpha1", 32);
  if (failure && !error_message.empty()) {
    PutLpstr(&out, error_message);
    const auto code = diagnostic_code.empty() ? DiagnosticCodeFromMessage(error_message) : diagnostic_code;
    AppendMessageVectorSet(&out, Diag(code, error_message));
  }
  return out;
}

proto::Bytes CommandFailurePayload(const std::string& operation,
                                   const std::string& diagnostic_code,
                                   const std::vector<std::pair<std::string, std::string>>& entries = {}) {
  std::vector<std::pair<std::string, std::string>> payload_entries = {
      {"operation", operation},
      {"success", "false"},
      {"diagnostic", diagnostic_code},
  };
  payload_entries.insert(payload_entries.end(), entries.begin(), entries.end());
  auto out = StatusPayload(0x04, payload_entries);
  AppendMessageVectorSet(&out, Diag(diagnostic_code, diagnostic_code));
  return out;
}

proto::Bytes RandomNonce16() {
  const auto uuid = proto::MakePseudoUuidV7();
  return proto::Bytes(uuid.begin(), uuid.end());
}

std::uint64_t NextControlRequestId() {
  static std::atomic<std::uint64_t> next_id{1};
  return next_id.fetch_add(1);
}

bool RecvControlPlaneMessage(int fd,
                             proto::ControlPlaneMessage* message,
                             std::string* error_code) {
  proto::Bytes header(28);
  if (!RecvExact(fd, header.data(), header.size())) {
    if (error_code) *error_code = "CONTROL.READ_FAILED";
    return false;
  }
  const auto payload_len = static_cast<std::uint64_t>(header[20]) |
                           (static_cast<std::uint64_t>(header[21]) << 8u) |
                           (static_cast<std::uint64_t>(header[22]) << 16u) |
                           (static_cast<std::uint64_t>(header[23]) << 24u) |
                           (static_cast<std::uint64_t>(header[24]) << 32u) |
                           (static_cast<std::uint64_t>(header[25]) << 40u) |
                           (static_cast<std::uint64_t>(header[26]) << 48u) |
                           (static_cast<std::uint64_t>(header[27]) << 56u);
  if (payload_len > 64u * 1024u) {
    if (error_code) *error_code = "CONTROL.PAYLOAD_TOO_LARGE";
    return false;
  }
  proto::Bytes encoded = header;
  encoded.resize(28 + static_cast<std::size_t>(payload_len));
  if (payload_len != 0 && !RecvExact(fd, encoded.data() + 28, static_cast<std::size_t>(payload_len))) {
    if (error_code) *error_code = "CONTROL.READ_FAILED";
    return false;
  }
  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeControlPlaneMessage(encoded, &diagnostics);
  if (!decoded) {
    if (error_code) *error_code = diagnostics.empty() ? "CONTROL.FRAME_INVALID" : diagnostics.front().code;
    return false;
  }
  *message = *decoded;
  return true;
}

struct McpSession {
  bool auth_started = false;
  // MCP authentication admits the client to the manager proxy/control transport.
  // It is not engine user authentication; SBsql database authentication still
  // flows through the server/parser handshake after DB_CONNECT forwarding.
  bool authenticated = false;
  bool proxy_ready = false;
  int prepared_backend_fd = -1;
  std::uint8_t auth_method = 0;
  std::string username;
  std::string security_token;
  std::string security_provider_family = std::string(kSecurityDatabaseTemporaryTokenProvider);
  std::vector<std::string> management_rights;
  proto::UuidBytes session_id = proto::MakePseudoUuidV7();
};

bool HasManagementControlPermission(const McpSession& session) {
  return session.authenticated;
}

bool ManagerRightMatches(const std::string& grant, const std::string& required) {
  if (grant == "*") return true;
  if (grant == required) return true;
  constexpr std::string_view suffix = ".*";
  if (grant.size() > suffix.size() &&
      grant.compare(grant.size() - suffix.size(), suffix.size(), suffix) == 0) {
    const auto prefix = grant.substr(0, grant.size() - 1);
    return required.rfind(prefix, 0) == 0;
  }
  return false;
}

bool SessionHasManagementRight(const McpSession& session, const std::string& required) {
  if (!session.authenticated || required.empty()) return false;
  for (const auto& grant : session.management_rights) {
    if (ManagerRightMatches(grant, required)) return true;
  }
  return false;
}

std::string RequiredRightForManagerOperation(const std::string& operation) {
  if (operation == "manager.status") return "manager.status";
  if (operation == "manager.shutdown") return "manager.lifecycle.shutdown";
  if (operation == "support.bundle_generate") return "manager.support.export";
  if (operation == "listener.list" || operation == "listener.status") return "manager.listener.read";
  if (operation.rfind("listener.", 0) == 0) return "manager.listener.control";
  if (operation == "manager.validate_config") return "manager.config.validate";
  if (operation == "manager.reload_config") return "manager.config.reload";
  if (operation == "thirdparty.status_export") return "manager.thirdparty.status_export";
  if (IsClusterOnlyManagerOperation(operation)) return "manager.cluster";
  return {};
}

void CloseFd(int* fd) {
  if (*fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}
#endif

#ifdef _WIN32
std::filesystem::path ManagerSecurityTokenStorePath(const ManagerConfig& config) {
  if (!config.security_token_store_path.empty()) return config.security_token_store_path;
  if (!config.owner_database_path.empty()) {
    return std::filesystem::path(config.owner_database_path.string() + ".sb.temporary_auth_tokens");
  }
  return {};
}

bool ManagerRightNameValid(const std::string& right) {
  if (right.empty() || right.size() > 128) return false;
  for (unsigned char ch : right) {
    const bool ok = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '*';
    if (!ok) return false;
  }
  return true;
}

bool ParseManagerRightsStrict(const std::string& rights_text, std::vector<std::string>* rights) {
  if (rights != nullptr) rights->clear();
  std::size_t start = 0;
  while (start <= rights_text.size()) {
    const auto comma = rights_text.find(',', start);
    auto right = Trim(rights_text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (right.empty() || !ManagerRightNameValid(right)) return false;
    if (rights != nullptr) rights->push_back(std::move(right));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return true;
}

bool ManagerRightIsWildcardGrant(const std::string& right) {
  if (right == "*") return true;
  constexpr std::string_view suffix = ".*";
  return right.size() > suffix.size() &&
         right.compare(right.size() - suffix.size(), suffix.size(), suffix) == 0;
}
#endif

class ManagerRuntime {
 public:
  explicit ManagerRuntime(ManagerConfig config)
      : config_(std::move(config)),
        lifecycle_(config_.control_dir),
        dbbt_replay_cache_(config_.dbbt_replay_cache_entries) {}
  RuntimeResult Run();

 private:
  bool PrepareRuntime(std::vector<proto::Diagnostic>* diagnostics);
  void CleanupRuntime();
  void StartHeartbeat();
  bool StartControl(std::vector<proto::Diagnostic>* diagnostics);
  void StartProxy();
  void StopAll();
  void SpawnWorker(std::thread worker);
  void JoinWorkers();
  std::string StatusJson();
  std::string MetricsSnapshotJson();
  void PublishMetricsSnapshot();
#ifndef _WIN32
  void ControlLoop(int fd);
  void HandleControlClient(int fd);
  proto::SbdbFrame HandleMcpFrame(McpSession* session, const proto::SbdbFrame& request);
  proto::SbdbFrame ConnectFailure(McpSession* session,
                                  const std::string& message,
                                  const std::string& reason,
                                  const std::vector<std::pair<std::string, std::string>>& fields = {});
  bool AuditEvent(const std::string& event,
                  bool success,
                  const std::string& reason,
                  const std::vector<std::pair<std::string, std::string>>& fields = {});
  void ProxyLoop(int fd);
  void HandleProxyClient(int client_fd);
  void ForwardPair(int client_fd, int backend_fd);
  bool SendListenerManagementCommand(const std::string& command, std::string* response_text, std::string* error_code);
  proto::SbdbFrame HandleManagerCommand(McpSession* session, const proto::SbdbFrame& request);
  proto::SbdbFrame HandleListenerCommand(McpSession* session,
                                         const std::string& operation,
                                         const std::string& idempotency_key,
                                         const std::string& payload_checksum,
                                         const std::vector<std::pair<std::string, std::string>>& args);
  proto::SbdbFrame HandleSupportBundleCommand(const std::string& operation,
                                              const std::string& idempotency_key,
                                              const std::string& payload_checksum,
                                              const std::vector<std::pair<std::string, std::string>>& args);
  proto::SbdbFrame HandleThirdPartyCommand(const std::string& operation);
  proto::SbdbFrame HandleConfigCommand(const std::string& operation,
                                       const std::string& idempotency_key,
                                       const std::string& payload_checksum,
                                       const std::vector<std::pair<std::string, std::string>>& args);
  bool GenerateSupportBundle(const std::filesystem::path& bundle_dir,
                             const std::string& scope,
                             const std::string& redaction_profile,
                             std::string* error_code);
  bool EvaluateRestartAfterMissedHeartbeat(std::uint64_t now_ms);
  bool LaunchServerRestart();
#endif

  struct IdempotencyRecord {
    std::string operation;
    std::string key;
    std::string payload_checksum;
    std::uint64_t expires_at_ms = 0;
    proto::SbdbFrame response;
  };

  std::optional<proto::SbdbFrame> LookupIdempotentResult(const std::string& operation,
                                                        const std::string& key,
                                                        const std::string& payload_checksum,
                                                        bool* conflict);
  void StoreIdempotentResult(const std::string& operation,
                             const std::string& key,
                             const std::string& payload_checksum,
                             const proto::SbdbFrame& response);

  ManagerConfig config_;
  ManagerLifecycle lifecycle_;
  proto::DbbtReplayCache dbbt_replay_cache_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::thread> threads_;
  std::mutex worker_mutex_;
  std::mutex audit_mutex_;
  std::mutex metrics_mutex_;
  std::mutex idempotency_mutex_;
  std::vector<std::thread> worker_threads_;
  std::vector<IdempotencyRecord> idempotency_records_;
  std::atomic_bool stopping_{false};
  std::uint64_t accepted_clients_ = 0;
  std::uint64_t rejected_clients_ = 0;
  std::uint64_t active_clients_ = 0;
  std::uint64_t proxy_bytes_client_to_backend_ = 0;
  std::uint64_t proxy_bytes_backend_to_client_ = 0;
  std::uint64_t management_clients_active_ = 0;
  std::uint64_t management_requests_total_ = 0;
  std::uint64_t management_clients_rejected_ = 0;
  std::atomic_uint64_t audit_sequence_{0};
  std::atomic_uint64_t audit_bytes_written_{0};
  std::atomic_uint64_t audit_write_failures_{0};
  std::atomic_uint64_t metrics_publish_failures_{0};
  std::uint64_t listener_control_requests_total_ = 0;
  std::uint64_t listener_control_failures_total_ = 0;
  std::string listener_profile_state_ = "unknown";
  std::uint64_t support_bundle_requests_total_ = 0;
  std::uint64_t support_bundle_failures_total_ = 0;
  bool lifecycle_started_ = false;
  std::uint64_t heartbeat_success_ = 0;
  std::uint64_t heartbeat_failure_ = 0;
  std::uint64_t missed_heartbeat_count_ = 0;
  std::uint64_t restart_attempts_ = 0;
  std::uint64_t restart_refusals_ = 0;
  std::uint64_t restart_window_start_ms_ = 0;
  std::uint64_t next_restart_allowed_ms_ = 0;
  bool restart_quarantined_ = false;
  std::string last_restart_reason_;
  std::string health_state_ = "unknown";
};

ManagerRuntimePaths ResolveManagerRuntimePathsImpl(const ManagerConfig& config) {
  ManagerRuntimePaths paths;
  paths.runtime_dir = config.runtime_dir;
  paths.control_dir = config.control_dir;
  paths.pid_file = PidFile(config);
  paths.owner_file = OwnerFile(config);
  paths.state_file = StateFile(config);
  paths.control_socket = ControlSocketPath(config);
  paths.audit_file = AuditFile(config);
  paths.metrics_file = MetricsFile(config);
  paths.owner_database_runtime_scope_id =
      config.owner_database_runtime_scope_id.empty()
          ? ManagerOwnerDatabaseRuntimeScopeIdImpl(config)
          : config.owner_database_runtime_scope_id;
  return paths;
}

ManagerRuntimeValidation ValidateManagerRuntimeArtifactsImpl(const ManagerConfig& config,
                                                             bool require_existing_files) {
  ManagerRuntimeValidation validation;
  const auto paths = ResolveManagerRuntimePathsImpl(config);
  auto add = [&](std::string code, std::string message, std::vector<proto::Field> fields = {}) {
    validation.diagnostics.push_back(Diag(std::move(code), std::move(message), std::move(fields)));
  };

  std::error_code ec;
  validation.directories_valid =
      std::filesystem::is_directory(paths.control_dir, ec) &&
      std::filesystem::is_directory(paths.runtime_dir, ec);
#ifndef _WIN32
  auto private_path = [](const std::filesystem::path& path, bool directory) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (st.st_uid != ::geteuid()) return false;
    if (directory && !S_ISDIR(st.st_mode)) return false;
    if (!directory && !S_ISREG(st.st_mode)) return false;
    return (st.st_mode & 0077u) == 0;
  };
  validation.directories_valid = validation.directories_valid &&
                                 private_path(paths.control_dir, true) &&
                                 private_path(paths.runtime_dir, true);
#endif
  if (!validation.directories_valid) {
    add("MANAGER.RUNTIME_DIRECTORY_INVALID",
        "Manager runtime directories are missing or unsafe.",
        std::vector<proto::Field>{{"control_dir", paths.control_dir.string()},
                                  {"runtime_dir", paths.runtime_dir.string()}});
  }

  const bool owner_exists = std::filesystem::exists(paths.owner_file);
  const bool pid_exists = std::filesystem::exists(paths.pid_file);
  if (require_existing_files && (!owner_exists || !pid_exists)) {
    add("MANAGER.RUNTIME_ARTIFACT_MISSING",
        "Manager owner or PID artifacts are missing.",
        std::vector<proto::Field>{{"owner_file", paths.owner_file.string()},
                                  {"pid_file", paths.pid_file.string()}});
  }

  validation.pid_owner_valid = owner_exists && pid_exists;
#ifndef _WIN32
  validation.pid_owner_valid = validation.pid_owner_valid &&
                               private_path(paths.owner_file, false) &&
                               private_path(paths.pid_file, false);
#endif
  std::optional<long> pid_file_pid;
#ifndef _WIN32
  const auto owner_pid = ReadPidFromOwner(paths.owner_file);
  {
    std::ifstream in(paths.pid_file);
    long value = 0;
    if (in >> value) pid_file_pid = value;
  }
  if (owner_exists && pid_exists && (!owner_pid || !pid_file_pid || *owner_pid != *pid_file_pid)) {
    validation.pid_owner_valid = false;
    add("MANAGER.OWNER_TOKEN_AMBIGUOUS",
        "Manager owner token and PID file do not identify the same process.");
  }
#endif
  if ((owner_exists || pid_exists) && !validation.pid_owner_valid) {
    add("MANAGER.OWNER_TOKEN_INVALID",
        "Manager owner or PID artifact has unsafe ownership, permissions, or identity.");
  }

  validation.database_association_valid = true;
  if (owner_exists) {
    const auto values = [&]() {
      std::ifstream in(paths.owner_file);
      std::map<std::string, std::string> parsed;
      std::string line;
      while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) parsed[line.substr(0, eq)] = line.substr(eq + 1);
      }
      return parsed;
    }();
    const auto scope = values.find("owner_database_runtime_scope_id");
    const auto name = values.find("owner_database_name");
    validation.database_association_valid =
        scope != values.end() && scope->second == paths.owner_database_runtime_scope_id &&
        name != values.end() && name->second == config.owner_database_name;
    if (!validation.database_association_valid) {
      add("MANAGER.OWNER_DATABASE_SCOPE_INVALID",
          "Manager owner token belongs to a different database scope.",
          std::vector<proto::Field>{{"owner_database", config.owner_database_name}});
    }
  }

  validation.cleanup_safe = validation.directories_valid &&
                            (!owner_exists || validation.pid_owner_valid) &&
                            validation.database_association_valid;
  return validation;
}

RuntimeResult CleanupManagerRuntimeArtifactsImpl(const ManagerConfig& config,
                                                 ManagerRuntimeCleanupOperation operation) {
  RuntimeResult result;
  const auto validation = ValidateManagerRuntimeArtifactsImpl(config, false);
  if (!validation.cleanup_safe) {
    result.exit_code = 2;
    result.diagnostics = validation.diagnostics;
    if (result.diagnostics.empty()) {
      result.diagnostics.push_back(Diag("MANAGER.RUNTIME_CLEANUP_REFUSED",
                                        "Manager runtime cleanup was refused because scope was not proven."));
    }
    return result;
  }
  std::error_code ec;
  const auto paths = ResolveManagerRuntimePathsImpl(config);
  std::filesystem::remove(paths.control_socket, ec);
  std::filesystem::remove(paths.pid_file, ec);
  std::filesystem::remove(paths.owner_file, ec);
  if (operation == ManagerRuntimeCleanupOperation::kUninstall) {
    std::filesystem::remove(paths.metrics_file, ec);
    std::filesystem::remove(paths.audit_file, ec);
    std::filesystem::remove(paths.state_file, ec);
    std::filesystem::remove(paths.control_dir, ec);
  }
  return result;
}

bool ManagerRuntime::PrepareRuntime(std::vector<proto::Diagnostic>* diagnostics) {
  std::error_code ec;
  std::filesystem::create_directories(config_.runtime_dir, ec);
  if (ec) {
    diagnostics->push_back(Diag("MANAGER.RUNTIME_DIR_INVALID", "Runtime directory is invalid.", {{"path", config_.runtime_dir.string()}}));
    return false;
  }
  std::filesystem::create_directories(config_.control_dir, ec);
  if (ec) {
    diagnostics->push_back(Diag("MANAGER.CONTROL_DIR_INVALID", "Control directory is invalid.", {{"path", config_.control_dir.string()}}));
    return false;
  }
#ifndef _WIN32
  ::chmod(config_.runtime_dir.c_str(), 0700);
  ::chmod(config_.control_dir.c_str(), 0700);
#endif
  if (config_.log_path != "stderr") std::filesystem::create_directories(config_.log_path.parent_path(), ec);
#ifndef _WIN32
  const auto owner = std::filesystem::path(OwnerFile(config_));
  if (std::filesystem::exists(owner)) {
    const auto pid = ReadPidFromOwner(owner);
    if (pid && ProcessExists(*pid)) {
      diagnostics->push_back(Diag("MANAGER.OWNER_TOKEN_BUSY", "Another manager owns the runtime directory."));
      return false;
    }
    const auto last_state = ReadLifecycleState(config_);
    if (!pid || !last_state || !IsSafeFinalLifecycleState(*last_state)) {
      diagnostics->push_back(Diag("MANAGER.OWNER_TOKEN_AMBIGUOUS", "Stale manager owner token cannot be proven safe."));
      return false;
    }
    const auto owner_fields = ReadOwnerTokenFields(owner);
    const auto owner_scope = owner_fields.find("owner_database_runtime_scope_id");
    const auto owner_name = owner_fields.find("owner_database_name");
    const auto expected_scope = config_.owner_database_runtime_scope_id.empty()
                                    ? ManagerOwnerDatabaseRuntimeScopeIdImpl(config_)
                                    : config_.owner_database_runtime_scope_id;
    if ((owner_scope != owner_fields.end() && owner_scope->second != expected_scope) ||
        (owner_name != owner_fields.end() && owner_name->second != config_.owner_database_name)) {
      diagnostics->push_back(Diag("MANAGER.OWNER_DATABASE_SCOPE_INVALID",
                                  "Manager owner token belongs to a different database scope.",
                                  {{"owner_database", config_.owner_database_name}}));
      return false;
    }
    const auto paths = ResolveManagerRuntimePathsImpl(config_);
    if (std::filesystem::exists(paths.pid_file)) {
      const auto validation = ValidateManagerRuntimeArtifactsImpl(config_, false);
      if (!validation.pid_owner_valid) {
        diagnostics->insert(diagnostics->end(), validation.diagnostics.begin(), validation.diagnostics.end());
        if (validation.diagnostics.empty()) {
          diagnostics->push_back(Diag("MANAGER.OWNER_TOKEN_AMBIGUOUS", "Stale manager owner token cannot be proven safe."));
        }
        return false;
      }
      std::filesystem::remove(paths.pid_file, ec);
    }
    std::filesystem::remove(owner, ec);
  }
#endif
  if (!lifecycle_.Transition(ManagerLifecycleState::kRuntimePreparing,
                             "runtime prepared",
                             diagnostics)) {
    return false;
  }
  lifecycle_started_ = true;
  if (!lifecycle_.Transition(ManagerLifecycleState::kOwnerAcquiring,
                             "owner token acquiring",
                             diagnostics)) {
    return false;
  }
#ifndef _WIN32
  std::ofstream owner_out(owner, std::ios::trunc);
  if (!owner_out) {
    diagnostics->push_back(Diag("MANAGER.OWNER_TOKEN_AMBIGUOUS", "Owner token cannot be written."));
    return false;
  }
  std::array<char, 256> hostname{};
  if (::gethostname(hostname.data(), hostname.size() - 1) != 0) hostname[0] = '\0';
  const auto manager_uuid = proto::Hex(proto::MakePseudoUuidV7());
  const auto process_uuid = proto::Hex(proto::MakePseudoUuidV7());
  const auto now_ms = proto::CurrentEpochMilliseconds();
  std::ostringstream token_body;
  token_body << "format=SBMN_MANAGER_OWNER_V1\n";
  token_body << "manager_uuid=" << manager_uuid << "\n";
  token_body << "process_uuid=" << process_uuid << "\n";
  token_body << "pid=" << ::getpid() << "\n";
  token_body << "pid_start_time=" << now_ms << "\n";
  token_body << "generation=" << now_ms << "\n";
  token_body << "hostname=" << hostname.data() << "\n";
  token_body << "runtime_dir=" << config_.runtime_dir.string() << "\n";
  token_body << "control_dir=" << config_.control_dir.string() << "\n";
  token_body << "owner_database_name=" << config_.owner_database_name << "\n";
  token_body << "owner_database_runtime_scope_id="
             << (config_.owner_database_runtime_scope_id.empty()
                     ? ManagerOwnerDatabaseRuntimeScopeIdImpl(config_)
                     : config_.owner_database_runtime_scope_id)
             << "\n";
  if (config_.owner_database_uuid_set) {
    token_body << "owner_database_uuid=" << proto::Hex(config_.owner_database_uuid) << "\n";
  }
  token_body << "startup_time_ms=" << now_ms << "\n";
  token_body << "last_state=owner_acquiring\n";
  token_body << "clean_shutdown_required=true\n";
  owner_out << token_body.str();
  owner_out << "signature_or_checksum=" << ChecksumText(token_body.str()) << "\n";
  ::chmod(owner.c_str(), 0600);
  std::ofstream pid_out(PidFile(config_), std::ios::trunc);
  if (pid_out) {
    pid_out << ::getpid() << '\n';
    pid_out.close();
    ::chmod(PidFile(config_).c_str(), 0600);
  }
#endif
  return true;
}

void ManagerRuntime::CleanupRuntime() {
  std::vector<proto::Diagnostic> ignored;
  lifecycle_.Transition(ManagerLifecycleState::kStopped, "cleanup complete", &ignored);
  (void)CleanupManagerRuntimeArtifactsImpl(config_, ManagerRuntimeCleanupOperation::kStop);
}

void ManagerRuntime::StartHeartbeat() {
  threads_.emplace_back([this]() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_.load()) {
      cv_.wait_for(lock, std::chrono::milliseconds(config_.heartbeat_interval_ms), [this]() { return stopping_.load(); });
      if (stopping_.load()) break;
      lock.unlock();
#ifndef _WIN32
      const int fd = ConnectTcp(config_.native_bind, config_.native_port, config_.heartbeat_timeout_ms);
      lock.lock();
      if (fd >= 0) {
        ::close(fd);
        heartbeat_success_++;
        missed_heartbeat_count_ = 0;
        health_state_ = "healthy";
      } else {
        heartbeat_failure_++;
        missed_heartbeat_count_++;
        health_state_ = missed_heartbeat_count_ >= config_.missed_heartbeat_threshold ? "unreachable" : "degraded";
        if (health_state_ == "unreachable") {
          const auto now_ms = proto::CurrentEpochMilliseconds();
          if (!config_.restart_enabled) {
            restart_refusals_++;
            last_restart_reason_ = "MANAGER.SERVER_RESTART_DISABLED";
            lifecycle_.Evidence("server_unreachable", "MANAGER.SERVER_RESTART_DISABLED");
            AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_DISABLED");
          } else {
            lock.unlock();
            const bool launched = EvaluateRestartAfterMissedHeartbeat(now_ms);
            lock.lock();
            if (launched) {
              missed_heartbeat_count_ = 0;
              health_state_ = "degraded";
            }
          }
        }
      }
      lock.unlock();
      PublishMetricsSnapshot();
      lock.lock();
#else
      lock.lock();
      heartbeat_failure_++;
      health_state_ = "unreachable";
      lock.unlock();
      PublishMetricsSnapshot();
      lock.lock();
#endif
    }
  });
}

bool ManagerRuntime::StartControl(std::vector<proto::Diagnostic>* diagnostics) {
#ifndef _WIN32
  const auto path = ControlSocketPath(config_);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    diagnostics->push_back(Diag("MANAGER.CONTROL_SOCKET_PATH_TOO_LONG",
                                "Manager control socket path exceeds the platform AF_UNIX limit.",
                                {{"path", path},
                                 {"limit", std::to_string(sizeof(addr.sun_path) - 1)}}));
    return false;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    diagnostics->push_back(Diag("MANAGER.CONTROL_SOCKET_UNAVAILABLE",
                                "Manager control socket could not be created.",
                                {{"path", path}}));
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    diagnostics->push_back(Diag("MANAGER.CONTROL_SOCKET_BIND_FAILED",
                                "Manager control socket could not be bound.",
                                {{"path", path}}));
    return false;
  }
  if (::listen(fd, static_cast<int>(config_.management_backlog)) != 0) {
    ::close(fd);
    diagnostics->push_back(Diag("MANAGER.CONTROL_SOCKET_LISTEN_FAILED",
                                "Manager control socket could not enter listen state.",
                                {{"path", path}}));
    return false;
  }
  ::chmod(path.c_str(), 0600);
  threads_.emplace_back([this, fd]() { ControlLoop(fd); });
  return true;
#else
  (void)diagnostics;
  return true;
#endif
}

void ManagerRuntime::StartProxy() {
#ifndef _WIN32
  if (!config_.proxy_enabled) return;
  std::vector<proto::Diagnostic> ignored;
  const int fd = CreateTcpListener(config_.bind_address, config_.proxy_port, static_cast<int>(config_.proxy_backlog), &ignored);
  if (fd < 0) return;
  threads_.emplace_back([this, fd]() { ProxyLoop(fd); });
#endif
}

void ManagerRuntime::StopAll() {
  stopping_.store(true);
  cv_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) thread.join();
  }
  JoinWorkers();
}

void ManagerRuntime::SpawnWorker(std::thread worker) {
  std::lock_guard<std::mutex> lock(worker_mutex_);
  worker_threads_.push_back(std::move(worker));
}

void ManagerRuntime::JoinWorkers() {
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    workers.swap(worker_threads_);
  }
  for (auto& worker : workers) {
    if (worker.joinable()) worker.join();
  }
}

std::string ManagerRuntime::StatusJson() {
  std::lock_guard<std::mutex> lock(mutex_);
  return RenderManagerStatusJson(ManagerStatusSnapshot{
      lifecycle_.CurrentName(), active_clients_, accepted_clients_, rejected_clients_,
      proxy_bytes_client_to_backend_, proxy_bytes_backend_to_client_,
      management_clients_active_, management_clients_rejected_, management_requests_total_,
      audit_sequence_.load(), audit_bytes_written_.load(), audit_write_failures_.load(),
      metrics_publish_failures_.load(), health_state_,
      heartbeat_success_, heartbeat_failure_, missed_heartbeat_count_, config_.restart_enabled,
      restart_attempts_, restart_refusals_, restart_quarantined_, next_restart_allowed_ms_,
      last_restart_reason_});
}

std::string ManagerRuntime::MetricsSnapshotJson() {
  std::lock_guard<std::mutex> lock(mutex_);
  return RenderManagerMetricsJson(ManagerMetricsSnapshot{
      proto::CurrentEpochMilliseconds(), lifecycle_.CurrentName(), active_clients_, accepted_clients_,
      rejected_clients_, proxy_bytes_client_to_backend_, proxy_bytes_backend_to_client_,
      management_clients_active_, management_clients_rejected_, management_requests_total_,
      audit_sequence_.load(), audit_bytes_written_.load(), audit_write_failures_.load(),
      metrics_publish_failures_.load(), listener_profile_state_,
      listener_control_requests_total_, listener_control_failures_total_, support_bundle_requests_total_,
      support_bundle_failures_total_, heartbeat_success_, heartbeat_failure_, health_state_, restart_attempts_,
      restart_refusals_, restart_quarantined_});
}

void ManagerRuntime::PublishMetricsSnapshot() {
  const auto snapshot = MetricsSnapshotJson();
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  if (!WriteAtomicPrivateText(MetricsFile(config_), snapshot)) {
    metrics_publish_failures_.fetch_add(1);
  }
}

std::optional<proto::SbdbFrame> ManagerRuntime::LookupIdempotentResult(const std::string& operation,
                                                                       const std::string& key,
                                                                       const std::string& payload_checksum,
                                                                       bool* conflict) {
  if (conflict) *conflict = false;
  const auto now_ms = proto::CurrentEpochMilliseconds();
  std::lock_guard<std::mutex> lock(idempotency_mutex_);
  idempotency_records_.erase(std::remove_if(idempotency_records_.begin(),
                                            idempotency_records_.end(),
                                            [now_ms](const IdempotencyRecord& record) {
                                              return record.expires_at_ms != 0 && record.expires_at_ms < now_ms;
                                            }),
                             idempotency_records_.end());
  for (const auto& record : idempotency_records_) {
    if (record.operation != operation || record.key != key) continue;
    if (record.payload_checksum != payload_checksum) {
      if (conflict) *conflict = true;
      return std::nullopt;
    }
    return record.response;
  }
  return std::nullopt;
}

void ManagerRuntime::StoreIdempotentResult(const std::string& operation,
                                           const std::string& key,
                                           const std::string& payload_checksum,
                                           const proto::SbdbFrame& response) {
  constexpr std::uint64_t kDefaultIdempotencyTtlMs = 600000;
  const auto now_ms = proto::CurrentEpochMilliseconds();
  std::lock_guard<std::mutex> lock(idempotency_mutex_);
  for (auto& record : idempotency_records_) {
    if (record.operation == operation && record.key == key) {
      record.payload_checksum = payload_checksum;
      record.expires_at_ms = now_ms + kDefaultIdempotencyTtlMs;
      record.response = response;
      return;
    }
  }
  idempotency_records_.push_back(IdempotencyRecord{operation, key, payload_checksum, now_ms + kDefaultIdempotencyTtlMs, response});
  if (idempotency_records_.size() > 4096) {
    idempotency_records_.erase(idempotency_records_.begin());
  }
}

#ifndef _WIN32
bool ManagerRuntime::AuditEvent(const std::string& event,
                                bool success,
                                const std::string& reason,
                                const std::vector<std::pair<std::string, std::string>>& fields) {
  std::lock_guard<std::mutex> lock(audit_mutex_);
  const auto sequence = audit_sequence_.fetch_add(1) + 1;
  const auto lifecycle_state = lifecycle_.CurrentName();
  std::ostringstream checksum_input;
  checksum_input << sequence << '|' << event << '|' << (success ? "true" : "false")
                 << '|' << reason << '|' << lifecycle_state;
  for (const auto& field : fields) {
    checksum_input << '|' << field.first << '=' << field.second;
  }
  const auto checksum = ChecksumText(checksum_input.str());
  const auto record = RenderManagerAuditJsonLine(ManagerAuditRecord{proto::Hex(proto::MakePseudoUuidV7()),
                                                                    sequence,
                                                                    proto::CurrentEpochMilliseconds(),
                                                                    event,
                                                                    success,
                                                                    reason,
                                                                    lifecycle_state,
                                                                    checksum,
                                                                    fields});
  if (!AppendDurablePrivateText(AuditFile(config_), record)) {
    audit_write_failures_.fetch_add(1);
    return false;
  }
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(AuditFile(config_), ec);
  if (!ec) audit_bytes_written_.store(bytes);
  return true;
}

void ManagerRuntime::ControlLoop(int fd) {
  while (!stopping_.load()) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 250);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0 || (pfd.revents & POLLIN) == 0) continue;
    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) continue;
    bool admitted = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (management_clients_active_ < config_.management_max_clients) {
        management_clients_active_++;
        admitted = true;
      } else {
        management_clients_rejected_++;
      }
    }
    if (!admitted) {
      AuditEvent("MANAGER_CONTROL_ADMISSION_DECISION", false, "MANAGER.CONTROL_MAX_CLIENTS");
      const auto response = proto::EncodeSbdbFrame(
          proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MANAGER.CONTROL_MAX_CLIENTS")});
      SendBytes(client, response);
      ::close(client);
      PublishMetricsSnapshot();
      continue;
    }
    SpawnWorker(std::thread(&ManagerRuntime::HandleControlClient, this, client));
  }
  ::close(fd);
}

void ManagerRuntime::HandleControlClient(int fd) {
  PublishMetricsSnapshot();
  McpSession session;
  while (!stopping_.load()) {
    proto::Bytes header(12);
    if (!RecvExactWithTimeout(fd, header.data(), header.size(), config_.management_idle_timeout_ms)) break;
    const auto payload_length = ReadU32(header, 8);
    const auto max_payload = std::min<std::uint32_t>(config_.management_max_payload_bytes,
                                                     16u * 1024u * 1024u);
    if (payload_length > max_payload) {
      const auto response = proto::EncodeSbdbFrame(proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("WIRE.PAYLOAD_TOO_LARGE")});
      SendBytes(fd, response);
      break;
    }
    proto::Bytes encoded = header;
    encoded.resize(12 + static_cast<std::size_t>(payload_length));
    if (payload_length != 0 &&
        !RecvExactWithTimeout(fd, encoded.data() + 12, payload_length, config_.management_idle_timeout_ms)) {
      break;
    }
    std::vector<proto::Diagnostic> diagnostics;
    const auto request = proto::DecodeSbdbFrame(encoded, &diagnostics);
    proto::SbdbFrame response;
    if (!request) {
      const std::string message = diagnostics.empty() ? "WIRE.FRAME_INVALID" : diagnostics.front().code;
      response = proto::SbdbFrame{0xff, 0, ProtocolErrorPayload(message)};
    } else {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        management_requests_total_++;
      }
      response = HandleMcpFrame(&session, *request);
    }
    SendBytes(fd, proto::EncodeSbdbFrame(response));
    if (!request || request->type == 0x03 || request->type == 0x60) break;
  }
  ::close(fd);
  if (session.prepared_backend_fd >= 0) {
    ::close(session.prepared_backend_fd);
    session.prepared_backend_fd = -1;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (management_clients_active_ > 0) management_clients_active_--;
  }
  PublishMetricsSnapshot();
}

proto::SbdbFrame ManagerRuntime::HandleMcpFrame(McpSession* session, const proto::SbdbFrame& request) {
  switch (request.type) {
    case 0x65:
      if (request.payload.size() != 4) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.HELLO_BODY_INVALID")};
      }
      return proto::SbdbFrame{0x64, 0, HelloResponsePayload(config_, request.payload)};
    case 0x68:
      if (!request.payload.empty()) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_LIST_BODY_NONEMPTY")};
      }
      return proto::SbdbFrame{0x64, 0, DbListResponsePayload(config_)};
    case 0x66: {
      std::size_t off = 0;
      std::string username;
      if (!ReadLpstr(request.payload, &off, &username) || off + 5 > request.payload.size()) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.AUTH_START_BODY_INVALID")};
      }
      const auto auth_method = request.payload[off++];
      const auto initial_len = ReadU32(request.payload, off);
      off += 4;
      if (off + initial_len != request.payload.size()) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.AUTH_START_TRAILING_BYTES")};
      }
      proto::Bytes initial_data(request.payload.begin() + static_cast<std::ptrdiff_t>(off), request.payload.end());
      if (username.empty()) {
        AuditEvent("MANAGER_AUTH_DECISION", false, "MCP_AUTH_START_USERNAME_REQUIRED");
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MCP_AUTH_START requires username")};
      }
      if (auth_method != 4) {
        AuditEvent("MANAGER_AUTH_DECISION", false, "MCP_AUTH_START_METHOD_UNSUPPORTED", {{"user", username}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MCP_AUTH_START requires auth_method=TOKEN")};
      }
      const auto secret = ResolveMcpSecret(config_);
      std::string token_failure;
      std::string engine_token_fragment;
      std::vector<std::string> token_rights;
      if (!initial_data.empty() &&
          ValidateManagerSecurityToken(config_,
                                       username,
                                       initial_data,
                                       &token_rights,
                                       &engine_token_fragment,
                                       &token_failure)) {
        if (!AuditEvent("MANAGER_AUTH_DECISION", true, "security_database_token_accepted", {{"user", username}, {"method", "TOKEN"}})) {
          return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
        }
        session->auth_started = true;
        session->authenticated = true;
        session->username = username;
        session->security_token = engine_token_fragment.empty()
                                      ? std::string(initial_data.begin(), initial_data.end())
                                      : engine_token_fragment;
        session->security_provider_family = std::string(kSecurityDatabaseTemporaryTokenProvider);
        session->management_rights = std::move(token_rights);
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(0, "")};
      }
      if (secret.empty()) {
        AuditEvent("MANAGER_AUTH_DECISION", false, "SECURITY.AUTH_SOURCE_UNAVAILABLE", {{"user", username}, {"detail", token_failure}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "security database temporary token unavailable")};
      }
      session->auth_started = true;
      session->auth_method = auth_method;
      session->username = username;
      session->session_id = proto::MakePseudoUuidV7();
      if (!initial_data.empty() && TimingSafeEqual(initial_data, secret)) {
        if (!AuditEvent("MANAGER_AUTH_DECISION", true, "accepted", {{"user", username}, {"method", "TOKEN"}})) {
          return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
        }
        session->authenticated = true;
        session->management_rights = McpSecretGrantedRights(config_);
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(0, "")};
      }
      AuditEvent("MANAGER_AUTH_DECISION", true, "challenge_issued", {{"user", username}, {"method", "TOKEN"}});
      return proto::SbdbFrame{0x12, 0, AuthChallengePayload(session->session_id, session->username)};
    }
    case 0x67: {
      if (request.payload.size() < 4) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.AUTH_CONTINUE_BODY_INVALID")};
      }
      const auto continuation_len = ReadU32(request.payload, 0);
      if (4 + continuation_len != request.payload.size()) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.AUTH_CONTINUE_TRAILING_BYTES")};
      }
      proto::Bytes continuation(request.payload.begin() + 4, request.payload.end());
      if (!session->auth_started) {
        AuditEvent("MANAGER_AUTH_DECISION", false, "MCP_AUTH_START_REQUIRED");
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MCP_AUTH_START is required before MCP_AUTH_CONTINUE")};
      }
      if (continuation.empty()) {
        AuditEvent("MANAGER_AUTH_DECISION", false, "MCP_AUTH_CONTINUE_EMPTY", {{"user", session->username}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MCP_AUTH_CONTINUE payload must not be empty")};
      }
      if (session->auth_method != 4) {
        AuditEvent("MANAGER_AUTH_DECISION", false, "MCP_AUTH_CONTINUE_METHOD_MISMATCH", {{"user", session->username}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MCP_AUTH_CONTINUE method mismatch")};
      }
      const auto secret = ResolveMcpSecret(config_);
      std::string token_failure;
      std::string engine_token_fragment;
      std::vector<std::string> token_rights;
      if (ValidateManagerSecurityToken(config_,
                                       session->username,
                                       continuation,
                                       &token_rights,
                                       &engine_token_fragment,
                                       &token_failure)) {
        if (!AuditEvent("MANAGER_AUTH_DECISION", true, "security_database_token_accepted", {{"user", session->username}, {"method", "TOKEN"}})) {
          return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
        }
        session->authenticated = true;
        session->security_token = engine_token_fragment.empty()
                                      ? std::string(continuation.begin(), continuation.end())
                                      : engine_token_fragment;
        session->security_provider_family = std::string(kSecurityDatabaseTemporaryTokenProvider);
        session->management_rights = std::move(token_rights);
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(0, "")};
      }
      if (!secret.empty() && TimingSafeEqual(continuation, secret)) {
        if (!AuditEvent("MANAGER_AUTH_DECISION", true, "accepted", {{"user", session->username}, {"method", "TOKEN"}})) {
          return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
        }
        session->authenticated = true;
        session->management_rights = McpSecretGrantedRights(config_);
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(0, "")};
      }
      session->authenticated = false;
      AuditEvent("MANAGER_AUTH_DECISION", false, "MCP_AUTHENTICATION_FAILED", {{"user", session->username}, {"detail", token_failure}});
      return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MCP authentication failed")};
    }
    case 0x6a: {
      if (!session->authenticated) {
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "Authenticate before MCP_DB_INFO")};
      }
      std::size_t off = 0;
      std::string database_name;
      if (!ReadLpstr(request.payload, &off, &database_name)) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_INFO_BODY_INVALID")};
      }
      if (off != request.payload.size()) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_INFO_TRAILING_BYTES")};
      }
      if (database_name.empty()) database_name = config_.owner_database_name;
      return proto::SbdbFrame{0x64, 0, DbInfoResponsePayload(config_, database_name)};
    }
    case 0x69: {
      if (!session->authenticated) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP_DB_CONNECT_AUTH_REQUIRED");
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Authenticate before MCP_DB_CONNECT",
                                                                "MCP.DB_CONNECT_AUTH_REQUIRED")};
      }
      if (!SessionHasManagementRight(*session, "database.connect")) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.COMMAND_UNAUTHORIZED", {{"database", config_.owner_database_name}, {"required_right", "database.connect"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Database connect requires database.connect authority",
                                                                "MANAGER.COMMAND_UNAUTHORIZED")};
      }
      std::size_t off = 0;
      std::string database_name;
      std::string connection_profile;
      std::string client_intent;
      proto::Bytes client_nonce;
      if (request.payload.size() >= 4 &&
          request.payload[0] == 'M' && request.payload[1] == 'C' &&
          request.payload[2] == 'P' && request.payload[3] == '1') {
        off = 4;
        if (!ReadLpstr(request.payload, &off, &database_name)) {
          return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_CONNECT_BODY_INVALID")};
        }
        if (!ReadLpstr(request.payload, &off, &connection_profile) ||
            !ReadLpstr(request.payload, &off, &client_intent) ||
            off + 2 > request.payload.size()) {
          return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_CONNECT_BODY_INVALID")};
        }
        const auto nonce_len = ReadU16(request.payload, off);
        off += 2;
        if (off + nonce_len > request.payload.size()) {
          return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_CONNECT_BODY_INVALID")};
        }
        client_nonce.assign(request.payload.begin() + static_cast<std::ptrdiff_t>(off),
                            request.payload.begin() + static_cast<std::ptrdiff_t>(off + nonce_len));
        off += nonce_len;
      } else if (!ReadLpstr(request.payload, &off, &database_name)) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_CONNECT_BODY_INVALID")};
      }
      if (off != request.payload.size()) {
        return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.DB_CONNECT_TRAILING_BYTES")};
      }
      if (database_name.empty()) database_name = config_.owner_database_name;
      if (!client_intent.empty() && !IsSbsqlProfileAlias(client_intent)) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP_DB_CONNECT_UNSUPPORTED_INTENT", {{"database", database_name}, {"intent", client_intent}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Unsupported MCP client intent: " + client_intent,
                                                                "MCP.DB_CONNECT_INTENT_UNSUPPORTED")};
      }
      if (!connection_profile.empty() && !IsSbsqlProfileAlias(connection_profile)) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP_DB_CONNECT_UNSUPPORTED_PROFILE", {{"database", database_name}, {"profile", connection_profile}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Unsupported MCP connection profile: " + connection_profile,
                                                                "MCP.DB_CONNECT_PROFILE_UNSUPPORTED")};
      }
      if (!client_nonce.empty() && (client_nonce.size() < 16 || client_nonce.size() > 32)) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP_DB_CONNECT_NONCE_INVALID", {{"database", database_name}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "MCP_DB_CONNECT client_nonce must be 16..32 bytes",
                                                                "MCP.DB_CONNECT_NONCE_LENGTH")};
      }
      if (database_name != config_.owner_database_name) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.BACKEND_UNAVAILABLE", {{"database", database_name}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Database not available in manager_proxy scope: " + database_name,
                                                                "MCP.DB_NOT_BOUND")};
      }
      if (config_.listener_control_socket_dir.empty()) {
        if (!ReleaseProfileAllowsDirectNative(config_.release_profile)) {
          AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.DIRECT_NATIVE_FORBIDDEN", {{"database", database_name}, {"release_profile", config_.release_profile}});
          return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                  "Direct-native manager DB_CONNECT is forbidden by release profile",
                                                                  "MANAGER.DIRECT_NATIVE_FORBIDDEN")};
        }
        const int backend_fd = ConnectTcp(config_.native_bind, config_.native_port, config_.proxy_backend_connect_timeout_ms);
        if (backend_fd < 0) {
          AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.BACKEND_UNAVAILABLE", {{"database", database_name}, {"path", "direct_native"}});
          return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                  "Internal native listener is not ready",
                                                                  "MANAGER.BACKEND_UNAVAILABLE")};
        }
        if (!AuditEvent("MANAGER_DB_CONNECT_DECISION", true, "accepted", {{"database", database_name}, {"path", "direct_native"}})) {
          ::close(backend_fd);
          return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0, "MANAGER.AUDIT_WRITE_FAILED")};
        }
        if (session->prepared_backend_fd >= 0) ::close(session->prepared_backend_fd);
        session->prepared_backend_fd = backend_fd;
        session->proxy_ready = true;
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(false, session->session_id, kConnectFlagBaseCapabilities, "")};
      }
      if (session->security_token.empty()) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP.DB_CONNECT_SECURITY_TOKEN_REQUIRED", {{"database", database_name}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager-mediated SBsql handoff requires a security database temporary token",
                                                                "MCP.DB_CONNECT_SECURITY_TOKEN_REQUIRED")};
      }
      if (config_.dbbt_keyring_path.empty()) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.SECRET_REQUIRED", {{"database", database_name}, {"missing", "dbbt_keyring_path"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect requires manager.dbbt.keyring_path",
                                                                "MANAGER.SECRET_REQUIRED")};
      }
      proto::DbbtKeyring keyring;
      const auto keyring_result = proto::LoadDbbtKeyring(config_.dbbt_keyring_path, &keyring);
      if (!keyring_result.ok) {
        const std::string code = keyring_result.diagnostics.empty() ? "MCP.DBBT_KEYRING_INVALID" : keyring_result.diagnostics.front().code;
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, code, {{"database", database_name}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect DBBT keyring rejected: " + code,
                                                                code)};
      }
      const auto now_ms = proto::CurrentEpochMilliseconds();
      if (keyring.not_before_ms != 0 && now_ms + config_.dbbt_clock_skew_ms < keyring.not_before_ms) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP.DBBT_KEYRING_NOT_YET_VALID", {{"database", database_name}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect DBBT keyring is not yet valid",
                                                                "MCP.DBBT_KEYRING_NOT_YET_VALID")};
      }
      if (keyring.not_after_ms != 0 && keyring.not_after_ms + config_.dbbt_clock_skew_ms < now_ms) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MCP.DBBT_KEYRING_EXPIRED", {{"database", database_name}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect DBBT keyring is expired",
                                                                "MCP.DBBT_KEYRING_EXPIRED")};
      }
      if (!config_.owner_database_uuid_set) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.SECRET_REQUIRED", {{"database", database_name}, {"missing", "owner_database_uuid"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect requires manager.owner.database_uuid for DBBT issuance",
                                                                "MANAGER.SECRET_REQUIRED")};
      }
      proto::DbbtToken token;
      token.version = 1;
      token.db_uuid = config_.owner_database_uuid;
      token.listener_id = config_.listener_id;
      token.issued_at_ms = now_ms;
      token.expires_at_ms = now_ms + config_.dbbt_ttl_ms;
      token.manager_session_id = session->session_id;
      token.client_nonce = client_nonce.empty() ? RandomNonce16() : client_nonce;
      token.server_nonce = RandomNonce16();
      token.flags = 0;
      const auto encoded_dbbt = proto::EncodeDbbt(token, keyring.active_key);
      proto::Lpreface preface;
      preface.listener_id = config_.listener_id;
      preface.dbbt = encoded_dbbt;
      preface.db_selector = database_name;
      preface.requested_profile = CanonicalizeSbsqlProfile(connection_profile);
      preface.auth_provider_family = session->security_provider_family.empty()
                                         ? std::string(kSecurityDatabaseTemporaryTokenProvider)
                                         : session->security_provider_family;
      preface.auth_principal = session->username;
      preface.auth_token = session->security_token;
      proto::Bytes encoded_preface;
      const auto preface_result = proto::EncodeLpreface(preface, &encoded_preface);
      if (!preface_result.ok) {
        const std::string code = preface_result.diagnostics.empty() ? "LPREFACE.ENCODE_FAILED" : preface_result.diagnostics.front().code;
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, code, {{"database", database_name}, {"path", "lpreface"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect LPREFACE encode failed: " + code,
                                                                code)};
      }
      std::string listener_text;
      std::string listener_error;
      const bool listener_ok = SendListenerManagementCommand("LPREFACE_VALIDATE " + proto::Hex(encoded_preface),
                                                            &listener_text,
                                                            &listener_error);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        listener_control_requests_total_++;
        if (!listener_ok) {
          listener_control_failures_total_++;
          listener_profile_state_ = "unavailable";
        } else {
          listener_profile_state_ = "ready";
        }
      }
      PublishMetricsSnapshot();
      if (!listener_ok) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, listener_error, {{"database", database_name}, {"path", "lpreface"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect listener LPREFACE failed: " + listener_error,
                                                                listener_error)};
      }
      constexpr std::string_view ack_prefix = "lpreface_ack:";
      if (listener_text.rfind(std::string(ack_prefix), 0) != 0) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "LPREFACE.ACK_PREFIX_INVALID", {{"database", database_name}, {"path", "lpreface"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect listener LPREFACE was rejected",
                                                                "LPREFACE.ACK_PREFIX_INVALID")};
      }
      std::vector<proto::Diagnostic> ack_diagnostics;
      const auto ack = proto::DecodeLprefaceAck(proto::FromHex(listener_text.substr(ack_prefix.size())), &ack_diagnostics);
      if (!ack || !ack->accepted) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "LPREFACE.ACK_INVALID", {{"database", database_name}, {"path", "lpreface"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect listener LPREFACE ack was invalid",
                                                                "LPREFACE.ACK_INVALID")};
      }
      const int backend_fd = ConnectTcp(config_.native_bind, config_.native_port, config_.proxy_backend_connect_timeout_ms);
      if (backend_fd < 0) {
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "MANAGER.BACKEND_UNAVAILABLE", {{"database", database_name}, {"path", "lpreface"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Internal native listener is not ready after LPREFACE",
                                                                "MANAGER.BACKEND_UNAVAILABLE")};
      }
      const auto claim_line = proto::EncodeLprefaceHandoffClaim(token.client_nonce, token.server_nonce);
      if (!SendAll(backend_fd, claim_line.data(), claim_line.size())) {
        ::close(backend_fd);
        AuditEvent("MANAGER_DB_CONNECT_DECISION", false, "LPREFACE.CLAIM_SEND_FAILED", {{"database", database_name}, {"path", "lpreface"}});
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0,
                                                                "Manager DB connect listener handoff claim send failed",
                                                                "LPREFACE.CLAIM_SEND_FAILED")};
      }
      if (!AuditEvent("MANAGER_DB_CONNECT_DECISION", true, "accepted", {{"database", database_name}, {"path", "lpreface"}})) {
        ::close(backend_fd);
        return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(true, session->session_id, 0, "MANAGER.AUDIT_WRITE_FAILED")};
      }
      if (session->prepared_backend_fd >= 0) ::close(session->prepared_backend_fd);
      session->prepared_backend_fd = backend_fd;
      session->proxy_ready = true;
      return proto::SbdbFrame{0x02, 0, ConnectResponsePayload(false,
                                                              session->session_id,
                                                              kConnectFlagBaseCapabilities | kConnectFlagManagerDbbt,
                                                              "")};
    }
    case 0x60: {
      if (!HasManagementControlPermission(*session)) {
        AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.PERMISSION_DENIED", {{"command", "shutdown"}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.PERMISSION_DENIED")};
      }
      if (!SessionHasManagementRight(*session, "manager.lifecycle.shutdown")) {
        AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_UNAUTHORIZED", {{"command", "shutdown"}, {"required_right", "manager.lifecycle.shutdown"}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.COMMAND_UNAUTHORIZED")};
      }
      std::string idempotency_key = "implicit:" + proto::Hex(session->session_id);
      if (!request.payload.empty()) {
        std::size_t off = 0;
        if (request.payload.size() < 4 ||
            request.payload[0] != 'M' || request.payload[1] != 'C' ||
            request.payload[2] != 'P' || request.payload[3] != '1') {
          return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.SHUTDOWN_BODY_INVALID")};
        }
        off = 4;
        if (!ReadLpstr(request.payload, &off, &idempotency_key) || idempotency_key.empty() || off != request.payload.size()) {
          return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.SHUTDOWN_IDEMPOTENCY_INVALID")};
        }
      }
      std::string idempotency_reason;
      if (!ManagerIdempotencyKeyValid(idempotency_key, &idempotency_reason)) {
        AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.IDEMPOTENCY_KEY_INVALID",
                   {{"command", "shutdown"}, {"reason", idempotency_reason}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_KEY_INVALID")};
      }
      bool idempotency_conflict = false;
      const auto payload_checksum = ChecksumBytes(request.payload);
      if (const auto replay = LookupIdempotentResult("manager.shutdown", idempotency_key, payload_checksum, &idempotency_conflict)) {
        AuditEvent("MANAGER_COMMAND_DECISION", true, "idempotent_replay", {{"command", "shutdown"}});
        return *replay;
      }
      if (idempotency_conflict) {
        AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.IDEMPOTENCY_CONFLICT", {{"command", "shutdown"}});
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_CONFLICT")};
      }
      if (!AuditEvent("MANAGER_COMMAND_DECISION", true, "accepted", {{"command", "shutdown"}})) {
        return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
      }
      stopping_.store(true);
      cv_.notify_all();
      std::vector<proto::Diagnostic> ignored;
      lifecycle_.Transition(ManagerLifecycleState::kDraining, "MCP shutdown accepted", &ignored);
      proto::SbdbFrame response{0x64, 0, StatusPayload(0x01, {{"shutdown", "accepted"}, {"state", lifecycle_.CurrentName()}})};
      StoreIdempotentResult("manager.shutdown", idempotency_key, payload_checksum, response);
      return response;
    }
    case 0x03:
      return proto::SbdbFrame{0x64, 0, StatusPayload(0x01, {{"disconnect", "accepted"}})};
    case 0x6b:
      return HandleManagerCommand(session, request);
    default:
      return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.MESSAGE_TYPE_UNSUPPORTED")};
  }
}

proto::SbdbFrame ManagerRuntime::HandleManagerCommand(McpSession* session, const proto::SbdbFrame& request) {
  if (!session->authenticated) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.PERMISSION_DENIED", {{"command", "manager_command"}});
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.PERMISSION_DENIED")};
  }
  std::string operation;
  std::string idempotency_key;
  std::vector<std::pair<std::string, std::string>> args;
  if (!DecodeManagerCommandPayload(request.payload, &operation, &idempotency_key, &args)) {
    return proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("MCP.MANAGER_COMMAND_BODY_INVALID")};
  }
  const auto required_right = RequiredRightForManagerOperation(operation);
  if (!required_right.empty() && !SessionHasManagementRight(*session, required_right)) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_UNAUTHORIZED", {{"command", operation}, {"required_right", required_right}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.COMMAND_UNAUTHORIZED", {{"required_right", required_right}})};
  }
  std::string arg_field;
  std::string arg_reason;
  if (!ManagerCommandArgsValid(operation, args, &arg_field, &arg_reason)) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_ARGS_INVALID",
               {{"command", operation}, {"field", arg_field}, {"reason", arg_reason}});
    return proto::SbdbFrame{0x64, 0,
                            CommandFailurePayload(operation,
                                                  "MANAGER.COMMAND_ARGS_INVALID",
                                                  {{"field", arg_field}, {"reason", arg_reason}})};
  }
  std::string idempotency_reason;
  if (ManagerOperationRequiresIdempotency(operation) &&
      !ManagerIdempotencyKeyValid(idempotency_key, &idempotency_reason)) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.IDEMPOTENCY_KEY_INVALID",
               {{"command", operation}, {"reason", idempotency_reason}});
    return proto::SbdbFrame{0x64, 0,
                            CommandFailurePayload(operation,
                                                  "MANAGER.IDEMPOTENCY_KEY_INVALID",
                                                  {{"field", "idempotency_key"},
                                                   {"reason", idempotency_reason}})};
  }
  const auto payload_checksum = ChecksumBytes(request.payload);
  if (operation.rfind("listener.", 0) == 0) {
    return HandleListenerCommand(session, operation, idempotency_key, payload_checksum, args);
  }
  if (operation == "manager.status") {
    if (!AuditEvent("MANAGER_COMMAND_DECISION", true, "accepted", {{"command", operation}})) {
      return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
    }
    return proto::SbdbFrame{0x64, 0, StatusPayload(0x04, {
        {"operation", operation},
        {"success", "true"},
        {"status_json", StatusJson()},
        {"metrics_json", MetricsSnapshotJson()},
    })};
  }
  if (operation.rfind("support.", 0) == 0) {
    return HandleSupportBundleCommand(operation, idempotency_key, payload_checksum, args);
  }
  if (operation.rfind("thirdparty.", 0) == 0) {
    return HandleThirdPartyCommand(operation);
  }
  if (operation == "manager.validate_config" || operation == "manager.reload_config") {
    return HandleConfigCommand(operation, idempotency_key, payload_checksum, args);
  }
  if (IsClusterOnlyManagerOperation(operation)) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.CLUSTER_ONLY_FORBIDDEN", {{"command", operation}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.CLUSTER_ONLY_FORBIDDEN")};
  }
  AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_UNSUPPORTED", {{"command", operation}});
  return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.COMMAND_UNSUPPORTED")};
}

proto::SbdbFrame ManagerRuntime::HandleListenerCommand(McpSession* session,
                                                       const std::string& operation,
                                                       const std::string& idempotency_key,
                                                       const std::string& payload_checksum,
                                                       const std::vector<std::pair<std::string, std::string>>& args) {
  (void)session;
  const auto listener_mapping = MapListenerControlOperation(operation, args);
  const bool mutating = listener_mapping.mutating;
  if (mutating && idempotency_key.empty()) {
    AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", false, "MANAGER.IDEMPOTENCY_REQUIRED", {{"operation", operation}});
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_REQUIRED")};
  }
  if (mutating) {
    bool conflict = false;
    if (const auto replay = LookupIdempotentResult(operation, idempotency_key, payload_checksum, &conflict)) {
      AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", true, "idempotent_replay", {{"operation", operation}});
      return *replay;
    }
    if (conflict) {
      AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", false, "MANAGER.IDEMPOTENCY_CONFLICT", {{"operation", operation}});
      return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_CONFLICT")};
    }
  }
  if (!listener_mapping.supported) {
    AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", false, listener_mapping.diagnostic_code, {{"operation", operation}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, listener_mapping.diagnostic_code)};
  }
  const std::string listener_command = listener_mapping.listener_command;
  if (config_.listener_control_socket_dir.empty()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      listener_control_failures_total_++;
      listener_profile_state_ = "unavailable";
    }
    PublishMetricsSnapshot();
    AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", false, "MANAGER.LISTENER_UNAVAILABLE", {{"operation", operation}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.LISTENER_UNAVAILABLE")};
  }
  if (mutating && !AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", true, "attempt_begin", {{"operation", operation}, {"listener_command", listener_command}})) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  std::string listener_text;
  std::string listener_error;
  const bool ok = SendListenerManagementCommand(listener_command, &listener_text, &listener_error);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_control_requests_total_++;
    if (!ok) {
      listener_control_failures_total_++;
      listener_profile_state_ = "unavailable";
    } else if (operation == "listener.drain" || listener_text == "draining") {
      listener_profile_state_ = "draining";
    } else if (operation == "listener.stop") {
      listener_profile_state_ = "stopping";
    } else {
      listener_profile_state_ = "ready";
    }
  }
  PublishMetricsSnapshot();
  if (!ok) {
    AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", false, listener_error, {{"operation", operation}, {"listener_command", listener_command}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, listener_error)};
  }
  if (!AuditEvent("MANAGER_LISTENER_CONTROL_DECISION", true, "accepted", {{"operation", operation}, {"listener_command", listener_command}}) && !mutating) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  proto::SbdbFrame response{0x64, 0, StatusPayload(0x04, {
      {"operation", operation},
      {"success", "true"},
      {"listener_id", std::to_string(config_.listener_id)},
      {"listener_command", listener_command},
      {"listener_response", listener_text},
  })};
  if (mutating) StoreIdempotentResult(operation, idempotency_key, payload_checksum, response);
  return response;
}

bool ManagerRuntime::GenerateSupportBundle(const std::filesystem::path& bundle_dir,
                                           const std::string& scope,
                                           const std::string& redaction_profile,
                                           std::string* error_code) {
  return GenerateManagerSupportBundle(config_,
                                      SupportBundleInputs{
                                          bundle_dir,
                                          scope,
                                          redaction_profile,
                                          StatusJson(),
                                          MetricsSnapshotJson(),
                                          AuditFile(config_),
                                          MetricsFile(config_),
                                          StateFile(config_),
                                          config_.control_dir / "sbmn_manager.lifecycle.journal",
                                      },
                                      error_code);
}

proto::SbdbFrame ManagerRuntime::HandleSupportBundleCommand(const std::string& operation,
                                                            const std::string& idempotency_key,
                                                            const std::string& payload_checksum,
                                                            const std::vector<std::pair<std::string, std::string>>& args) {
  if (operation != "support.bundle_generate") {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_UNSUPPORTED", {{"command", operation}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.COMMAND_UNSUPPORTED")};
  }
  if (idempotency_key.empty()) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.IDEMPOTENCY_REQUIRED", {{"command", operation}});
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_REQUIRED")};
  }
  bool conflict = false;
  if (const auto replay = LookupIdempotentResult(operation, idempotency_key, payload_checksum, &conflict)) {
    AuditEvent("MANAGER_COMMAND_DECISION", true, "idempotent_replay", {{"command", operation}});
    return *replay;
  }
  if (conflict) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.IDEMPOTENCY_CONFLICT", {{"command", operation}});
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_CONFLICT")};
  }
  const std::string scope = GetFieldValue(args, "scope", "manager");
  const std::string redaction_profile = GetFieldValue(args, "redaction_profile", "default");
  if (!ManagerSupportBundleLabelValid(scope)) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.SUPPORT_BUNDLE_ARG_INVALID", {{"command", operation}, {"field", "scope"}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.SUPPORT_BUNDLE_ARG_INVALID", {{"field", "scope"}})};
  }
  if (!ManagerSupportBundleRedactionProfileValid(redaction_profile)) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.SUPPORT_BUNDLE_ARG_INVALID", {{"command", operation}, {"field", "redaction_profile"}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.SUPPORT_BUNDLE_ARG_INVALID", {{"field", "redaction_profile"}})};
  }
  const auto bundle_uuid = proto::Hex(proto::MakePseudoUuidV7());
  const auto bundle_dir = SupportBundleRoot(config_) / bundle_uuid;
  if (!AuditEvent("MANAGER_COMMAND_DECISION", true, "support_bundle_attempt_begin", {{"command", operation}, {"bundle_uuid", bundle_uuid}})) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  std::string error_code;
  const bool ok = GenerateSupportBundle(bundle_dir, scope, redaction_profile, &error_code);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    support_bundle_requests_total_++;
    if (!ok) support_bundle_failures_total_++;
  }
  PublishMetricsSnapshot();
  if (!ok) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, error_code, {{"command", operation}, {"bundle_uuid", bundle_uuid}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, error_code)};
  }
  if (!AuditEvent("MANAGER_COMMAND_DECISION", true, "accepted", {{"command", operation}, {"bundle_uuid", bundle_uuid}})) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  proto::SbdbFrame response{0x64, 0, StatusPayload(0x04, {
      {"operation", operation},
      {"success", "true"},
      {"bundle_uuid", bundle_uuid},
      {"bundle_ref", "[path-redacted]"},
      {"redaction_profile", redaction_profile},
  })};
  StoreIdempotentResult(operation, idempotency_key, payload_checksum, response);
  return response;
}

proto::SbdbFrame ManagerRuntime::HandleThirdPartyCommand(const std::string& operation) {
  if (operation != "thirdparty.status_export") {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_UNSUPPORTED", {{"command", operation}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.COMMAND_UNSUPPORTED")};
  }
  if (!config_.third_party_management_enabled) {
    AuditEvent("MANAGER_COMMAND_DECISION", false, "MANAGER.COMMAND_UNSUPPORTED", {{"command", operation}, {"reason", "third_party_disabled"}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.COMMAND_UNSUPPORTED", {{"reason", "third_party_disabled"}})};
  }
  if (!AuditEvent("MANAGER_COMMAND_DECISION", true, "accepted", {{"command", operation}})) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  return proto::SbdbFrame{0x64, 0, StatusPayload(0x04, {
      {"operation", operation},
      {"success", "true"},
      {"status_json", StatusJson()},
      {"metrics_json", MetricsSnapshotJson()},
      {"support_bundle_root", "[path-redacted]"},
  })};
}

proto::SbdbFrame ManagerRuntime::HandleConfigCommand(const std::string& operation,
                                                     const std::string& idempotency_key,
                                                     const std::string& payload_checksum,
                                                     const std::vector<std::pair<std::string, std::string>>& args) {
  if (operation != "manager.validate_config" && operation != "manager.reload_config") {
    AuditEvent("MANAGER_CONFIG_DECISION", false, "MANAGER.COMMAND_UNSUPPORTED", {{"command", operation}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, "MANAGER.COMMAND_UNSUPPORTED")};
  }
  const bool mutating = operation == "manager.reload_config";
  if (mutating && idempotency_key.empty()) {
    AuditEvent("MANAGER_CONFIG_DECISION", false, "MANAGER.IDEMPOTENCY_REQUIRED", {{"command", operation}});
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_REQUIRED")};
  }
  if (mutating) {
    bool conflict = false;
    if (const auto replay = LookupIdempotentResult(operation, idempotency_key, payload_checksum, &conflict)) {
      AuditEvent("MANAGER_CONFIG_DECISION", true, "idempotent_replay", {{"command", operation}});
      return *replay;
    }
    if (conflict) {
      AuditEvent("MANAGER_CONFIG_DECISION", false, "MANAGER.IDEMPOTENCY_CONFLICT", {{"command", operation}});
      return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.IDEMPOTENCY_CONFLICT")};
    }
  }

  ManagerConfig candidate = config_;
  const std::string config_ref = GetFieldValue(args, "config_ref");
  std::string config_ref_reason;
  if (!ManagerConfigReferenceValid(config_, config_ref, &config_ref_reason)) {
    AuditEvent("MANAGER_CONFIG_DECISION", false, "MANAGER.CONFIG_REF_FORBIDDEN",
               {{"command", operation}, {"field", "config_ref"}, {"reason", config_ref_reason}});
    return proto::SbdbFrame{0x64, 0,
                            CommandFailurePayload(operation,
                                                  "MANAGER.CONFIG_REF_FORBIDDEN",
                                                  {{"field", "config_ref"},
                                                   {"reason", config_ref_reason}})};
  }
  const std::string config_ref_response = ConfigReferenceResponseValue(config_ref);
  bool explicit_config = false;
  if (!config_ref.empty()) {
    candidate.config_path = config_.config_path;
    explicit_config = true;
  } else {
    explicit_config = !candidate.config_path.empty();
  }
  std::vector<proto::Diagnostic> diagnostics;
  LoadConfigFile(&candidate, &diagnostics, explicit_config);
  ApplyDefaultPaths(&candidate);
  if (!candidate.no_spin_required) {
    diagnostics.push_back(Diag("MANAGER.NO_SPIN_REQUIRED", "No-spin synchronization invariant must remain enabled."));
  }
  if (!RestartExecutableValid(candidate.restart_executable.string()) ||
      !RestartArgumentsValid(candidate.restart_arguments)) {
    diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager restart descriptor is invalid."));
  }
  if (!ReleaseProfileAllowsDeveloperSecret(candidate.release_profile) &&
      McpSecretRefIsDeveloperOnly(candidate.mcp_secret_ref)) {
    diagnostics.push_back(Diag("MANAGER.RELEASE_PROFILE_FORBIDS_LITERAL_SECRET",
                               "Enterprise manager release profile forbids literal MCP secrets.",
                               {{"release_profile", candidate.release_profile}}));
  }
  if (!ReleaseProfileAllowsLocalTokenStore(candidate.release_profile) &&
      LocalTokenStoreExplicitlyConfigured(candidate)) {
    diagnostics.push_back(Diag("MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
                               "Enterprise manager release profile forbids local TSV token stores.",
                               {{"release_profile", candidate.release_profile}}));
  }
  AddEnterpriseSecretPolicyDiagnostics(candidate, &diagnostics);
  if (!diagnostics.empty()) {
    const std::string code = mutating ? "MANAGER.CONFIG_RELOAD_FAILED" : "MANAGER.CONFIG_VALIDATION_FAILED";
    AuditEvent("MANAGER_CONFIG_DECISION", false, code, {{"command", operation}, {"diagnostics", std::to_string(diagnostics.size())}});
    return proto::SbdbFrame{0x64, 0, CommandFailurePayload(operation, code, {{"diagnostics", std::to_string(diagnostics.size())}})};
  }

  std::vector<std::string> changed;
  auto note_change = [&](const char* name, const auto& before, const auto& after) {
    if (before != after) changed.push_back(name);
  };
  note_change("manager.server.restart.executable", config_.restart_executable, candidate.restart_executable);
  note_change("manager.server.restart.arguments", config_.restart_arguments, candidate.restart_arguments);
  note_change("manager.server.restart.enabled", config_.restart_enabled, candidate.restart_enabled);
  note_change("manager.server.restart.max_attempts", config_.restart_max_attempts, candidate.restart_max_attempts);
  note_change("manager.server.restart.window_ms", config_.restart_window_ms, candidate.restart_window_ms);
  note_change("manager.server.restart.initial_backoff_ms", config_.restart_initial_backoff_ms, candidate.restart_initial_backoff_ms);
  note_change("manager.server.restart.max_backoff_ms", config_.restart_max_backoff_ms, candidate.restart_max_backoff_ms);
  note_change("manager.dbbt.keyring_path", config_.dbbt_keyring_path, candidate.dbbt_keyring_path);
  note_change("manager.dbbt.ttl_ms", config_.dbbt_ttl_ms, candidate.dbbt_ttl_ms);
  note_change("manager.dbbt.clock_skew_ms", config_.dbbt_clock_skew_ms, candidate.dbbt_clock_skew_ms);
  note_change("manager.auth.mcp_secret_ref", config_.mcp_secret_ref, candidate.mcp_secret_ref);
  note_change("manager.auth.mcp_secret_rights", config_.mcp_secret_rights, candidate.mcp_secret_rights);
  note_change("manager.log.path", config_.log_path, candidate.log_path);
  note_change("manager.log.level", config_.log_level, candidate.log_level);
  note_change("manager.third_party.enabled", config_.third_party_management_enabled, candidate.third_party_management_enabled);
  note_change("manager.release.profile", config_.release_profile, candidate.release_profile);

  std::ostringstream changed_keys;
  for (std::size_t i = 0; i < changed.size(); ++i) {
    if (i != 0) changed_keys << ",";
    changed_keys << changed[i];
  }

  if (!mutating) {
    AuditEvent("MANAGER_CONFIG_DECISION", true, "validated", {{"command", operation}, {"config_ref", config_ref}});
    return proto::SbdbFrame{0x64, 0, StatusPayload(0x04, {
        {"operation", operation},
        {"success", "true"},
        {"config_ref", config_ref_response},
        {"diagnostics", "0"},
        {"changed_keys", changed_keys.str()},
    })};
  }

  if (!AuditEvent("MANAGER_CONFIG_DECISION", true, "reload_attempt_begin", {{"command", operation}, {"config_ref", config_ref}})) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.restart_executable = candidate.restart_executable;
    config_.restart_arguments = candidate.restart_arguments;
    config_.restart_enabled = candidate.restart_enabled;
    config_.restart_max_attempts = candidate.restart_max_attempts;
    config_.restart_window_ms = candidate.restart_window_ms;
    config_.restart_initial_backoff_ms = candidate.restart_initial_backoff_ms;
    config_.restart_max_backoff_ms = candidate.restart_max_backoff_ms;
    config_.dbbt_keyring_path = candidate.dbbt_keyring_path;
    config_.dbbt_ttl_ms = candidate.dbbt_ttl_ms;
    config_.dbbt_clock_skew_ms = candidate.dbbt_clock_skew_ms;
    config_.mcp_secret_ref = candidate.mcp_secret_ref;
    config_.mcp_secret_rights = candidate.mcp_secret_rights;
    config_.log_path = candidate.log_path;
    config_.log_level = candidate.log_level;
    config_.third_party_management_enabled = candidate.third_party_management_enabled;
    config_.release_profile = candidate.release_profile;
  }
  PublishMetricsSnapshot();
  if (!AuditEvent("MANAGER_CONFIG_DECISION", true, "accepted", {{"command", operation}, {"changed_keys", changed_keys.str()}})) {
    return proto::SbdbFrame{0x11, 0, AuthResponsePayload(1, "MANAGER.AUDIT_WRITE_FAILED")};
  }
  proto::SbdbFrame response{0x64, 0, StatusPayload(0x04, {
      {"operation", operation},
      {"success", "true"},
      {"config_ref", config_ref_response},
      {"reload_generation_ms", std::to_string(proto::CurrentEpochMilliseconds())},
      {"changed_keys", changed_keys.str()},
  })};
  StoreIdempotentResult(operation, idempotency_key, payload_checksum, response);
  return response;
}

void ManagerRuntime::ProxyLoop(int fd) {
  while (!stopping_.load()) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 250);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0 || (pfd.revents & POLLIN) == 0) continue;
    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) continue;
    bool rejected = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_clients_ >= config_.proxy_max_clients) {
        rejected_clients_++;
        AuditEvent("MANAGER_PROXY_ADMISSION_DECISION", false, "MANAGER.PROXY_MAX_CLIENTS", {{"active_clients", std::to_string(active_clients_)}});
        ::close(client);
        rejected = true;
      } else if (!AuditEvent("MANAGER_PROXY_ADMISSION_DECISION", true, "accepted", {{"active_clients", std::to_string(active_clients_)}})) {
        rejected_clients_++;
        ::close(client);
        rejected = true;
      } else {
        accepted_clients_++;
        active_clients_++;
      }
    }
    PublishMetricsSnapshot();
    if (rejected) continue;
    SpawnWorker(std::thread(&ManagerRuntime::HandleProxyClient, this, client));
  }
  ::close(fd);
}

void ManagerRuntime::HandleProxyClient(int client_fd) {
  bool handed_to_forwarder = false;
  McpSession session;
  while (!stopping_.load()) {
    proto::Bytes header(12);
    if (!RecvExact(client_fd, header.data(), header.size())) break;
    const auto payload_length = ReadU32(header, 8);
    if (payload_length > 16u * 1024u * 1024u) {
      const auto response = proto::EncodeSbdbFrame(proto::SbdbFrame{0xff, 0, ProtocolErrorPayload("WIRE.PAYLOAD_TOO_LARGE")});
      SendBytes(client_fd, response);
      break;
    }
    proto::Bytes encoded = header;
    encoded.resize(12 + static_cast<std::size_t>(payload_length));
    if (payload_length != 0 && !RecvExact(client_fd, encoded.data() + 12, payload_length)) break;
    std::vector<proto::Diagnostic> diagnostics;
    const auto request = proto::DecodeSbdbFrame(encoded, &diagnostics);
    proto::SbdbFrame response;
    if (!request) {
      const std::string message = diagnostics.empty() ? "WIRE.FRAME_INVALID" : diagnostics.front().code;
      response = proto::SbdbFrame{0xff, 0, ProtocolErrorPayload(message)};
    } else {
      response = HandleMcpFrame(&session, *request);
    }
    SendBytes(client_fd, proto::EncodeSbdbFrame(response));
    if (!request || request->type == 0x03 || request->type == 0x60) break;
    if (request->type == 0x69 && session.proxy_ready) {
      int backend_fd = session.prepared_backend_fd;
      session.prepared_backend_fd = -1;
      if (backend_fd < 0) {
        backend_fd = ConnectTcp(config_.native_bind, config_.native_port, config_.proxy_backend_connect_timeout_ms);
      }
      if (backend_fd < 0) break;
      handed_to_forwarder = true;
      ForwardPair(client_fd, backend_fd);
      break;
    }
  }
  if (session.prepared_backend_fd >= 0) {
    ::close(session.prepared_backend_fd);
    session.prepared_backend_fd = -1;
  }
  if (!handed_to_forwarder) ::close(client_fd);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_clients_ > 0) active_clients_--;
  }
  PublishMetricsSnapshot();
}

void ManagerRuntime::ForwardPair(int client_fd, int backend_fd) {
  std::array<char, 8192> buffer{};
  bool client_open = true;
  bool backend_open = true;
  while (!stopping_.load() && (client_open || backend_open)) {
    pollfd pfds[2]{};
    pfds[0].fd = client_fd;
    pfds[0].events = client_open ? POLLIN : 0;
    pfds[1].fd = backend_fd;
    pfds[1].events = backend_open ? POLLIN : 0;
    const int rc = ::poll(pfds, 2, static_cast<int>(config_.proxy_io_timeout_ms));
    if (rc <= 0) break;
    if (client_open && (pfds[0].revents & POLLIN)) {
      const auto n = ::recv(client_fd, buffer.data(), buffer.size(), 0);
      if (n <= 0) { client_open = false; ::shutdown(backend_fd, SHUT_WR); }
      else if (!SendAll(backend_fd, buffer.data(), static_cast<std::size_t>(n))) break;
      else {
        std::lock_guard<std::mutex> lock(mutex_);
        proxy_bytes_client_to_backend_ += static_cast<std::uint64_t>(n);
      }
    }
    if (backend_open && (pfds[1].revents & POLLIN)) {
      const auto n = ::recv(backend_fd, buffer.data(), buffer.size(), 0);
      if (n <= 0) { backend_open = false; ::shutdown(client_fd, SHUT_WR); }
      else if (!SendAll(client_fd, buffer.data(), static_cast<std::size_t>(n))) break;
      else {
        std::lock_guard<std::mutex> lock(mutex_);
        proxy_bytes_backend_to_client_ += static_cast<std::uint64_t>(n);
      }
    }
  }
  ::close(client_fd);
  ::close(backend_fd);
  PublishMetricsSnapshot();
}

bool ManagerRuntime::SendListenerManagementCommand(const std::string& command,
                                                   std::string* response_text,
                                                   std::string* error_code) {
  if (config_.listener_control_socket_dir.empty()) {
    if (error_code) *error_code = "CONTROL.SOCKET_DIR_EMPTY";
    return false;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error_code) *error_code = "CONTROL.SOCKET_CREATE_FAILED";
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto path = ListenerControlSocketPath(config_);
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    if (error_code) *error_code = "CONTROL.SOCKET_PATH_TOO_LONG";
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    if (error_code) *error_code = "CONTROL.CONNECT_FAILED";
    return false;
  }
  const auto frame = EncodeListenerManagementCommand(command, NextControlRequestId());
  if (frame.empty() || !SendBytes(fd, frame)) {
    ::close(fd);
    if (error_code) *error_code = "CONTROL.SEND_FAILED";
    return false;
  }
  proto::ControlPlaneMessage response;
  if (!RecvControlPlaneMessage(fd, &response, error_code)) {
    ::close(fd);
    return false;
  }
  ::close(fd);
  if (response.message_type != 0x0061) {
    if (error_code) *error_code = "CONTROL.MESSAGE_TYPE_INVALID";
    return false;
  }
  if (response.payload.empty()) {
    if (error_code) *error_code = "CONTROL.RESPONSE_EMPTY";
    return false;
  }
  const auto status = response.payload[0];
  std::string text(response.payload.begin() + 1, response.payload.end());
  text = NormalizeListenerManagementResponseText(std::move(text));
  if (response_text) *response_text = text;
  if (status != 0) {
    if (error_code) *error_code = text.empty() ? "CONTROL.RESPONSE_FAILED" : text;
    return false;
  }
  return true;
}

bool ManagerRuntime::EvaluateRestartAfterMissedHeartbeat(std::uint64_t now_ms) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (restart_quarantined_) {
      restart_refusals_++;
      last_restart_reason_ = "MANAGER.SERVER_RESTART_QUARANTINED";
      lifecycle_.Evidence("server_restart_refused", "MANAGER.SERVER_RESTART_QUARANTINED");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_QUARANTINED");
      return false;
    }
    if (config_.restart_max_attempts == 0) {
      restart_refusals_++;
      last_restart_reason_ = "MANAGER.SERVER_RESTART_DISABLED";
      lifecycle_.Evidence("server_restart_refused", "MANAGER.SERVER_RESTART_DISABLED");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_DISABLED");
      return false;
    }
    if (now_ms < next_restart_allowed_ms_) {
      restart_refusals_++;
      last_restart_reason_ = "MANAGER.SERVER_RESTART_BACKOFF_ACTIVE";
      lifecycle_.Evidence("server_restart_refused", "MANAGER.SERVER_RESTART_BACKOFF_ACTIVE");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_BACKOFF_ACTIVE", {{"next_allowed_ms", std::to_string(next_restart_allowed_ms_)}});
      return false;
    }
    if (restart_window_start_ms_ == 0 || now_ms - restart_window_start_ms_ > config_.restart_window_ms) {
      restart_window_start_ms_ = now_ms;
      restart_attempts_ = 0;
    }
    if (restart_attempts_ >= config_.restart_max_attempts) {
      restart_quarantined_ = true;
      health_state_ = "quarantined";
      last_restart_reason_ = "MANAGER.SERVER_RESTART_QUARANTINED";
      lifecycle_.Transition(ManagerLifecycleState::kQuarantined, "server restart crash-loop threshold exceeded", nullptr);
      lifecycle_.Evidence("server_restart_quarantined", "MANAGER.SERVER_RESTART_QUARANTINED");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_QUARANTINED", {{"attempts", std::to_string(restart_attempts_)}});
      return false;
    }
    if (config_.restart_executable.empty() || !config_.restart_executable.is_absolute()) {
      restart_refusals_++;
      last_restart_reason_ = "MANAGER.SERVER_RESTART_FAILED";
      lifecycle_.Evidence("server_restart_refused", "MANAGER.SERVER_RESTART_FAILED");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_FAILED", {{"reason", "launch_descriptor_invalid"}});
      return false;
    }
    restart_attempts_++;
    const std::uint64_t backoff = ComputeRestartBackoff(restart_attempts_,
                                                          config_.restart_initial_backoff_ms,
                                                          config_.restart_max_backoff_ms);
    next_restart_allowed_ms_ = now_ms + backoff;
    last_restart_reason_ = "restart_attempt_begin";
    lifecycle_.Evidence("server_restart_attempt_begin", "MANAGER.SERVER_RESTART_ATTEMPT");
    AuditEvent("MANAGER_SERVER_RESTART_DECISION", true, "MANAGER.SERVER_RESTART_ATTEMPT", {{"attempt", std::to_string(restart_attempts_)}, {"backoff_ms", std::to_string(backoff)}});
  }
  const bool launched = LaunchServerRestart();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (launched) {
      last_restart_reason_ = "restart_launched";
      lifecycle_.Evidence("server_restart_launched", "MANAGER.SERVER_RESTART_ATTEMPT");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", true, "restart_launched");
    } else {
      restart_refusals_++;
      last_restart_reason_ = "MANAGER.SERVER_RESTART_FAILED";
      lifecycle_.Evidence("server_restart_failed", "MANAGER.SERVER_RESTART_FAILED");
      AuditEvent("MANAGER_SERVER_RESTART_DECISION", false, "MANAGER.SERVER_RESTART_FAILED", {{"reason", "launch_failed"}});
    }
  }
  PublishMetricsSnapshot();
  return launched;
}

bool ManagerRuntime::LaunchServerRestart() {
  if (!std::filesystem::exists(config_.restart_executable)) return false;
  if (!RestartArgumentsValid(config_.restart_arguments)) return false;
  std::vector<std::string> args = SplitRestartArguments(config_.restart_arguments);
  std::vector<char*> argv;
  const auto executable = config_.restart_executable.string();
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    ::execv(executable.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  const pid_t waited = ::waitpid(pid, &status, 0);
  return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

RuntimeResult ManagerRuntime::Run() {
  RuntimeResult result;
  if (!config_.no_spin_required) {
    result.exit_code = 2;
    result.diagnostics.push_back(Diag("MANAGER.NO_SPIN_REQUIRED", "No-spin synchronization invariant is disabled."));
    return result;
  }
  if (!ManagerReleaseProfileValid(config_.release_profile)) {
    result.exit_code = 2;
    result.diagnostics.push_back(Diag("MANAGER.RELEASE_PROFILE_INVALID",
                                      "Manager release profile is invalid.",
                                      {{"release_profile", config_.release_profile}}));
    return result;
  }
  if (!ReleaseProfileAllowsDeveloperSecret(config_.release_profile) &&
      McpSecretRefIsDeveloperOnly(config_.mcp_secret_ref)) {
    result.exit_code = 2;
    result.diagnostics.push_back(Diag("MANAGER.RELEASE_PROFILE_FORBIDS_LITERAL_SECRET",
                                      "Enterprise manager release profile forbids literal MCP secrets.",
                                      {{"release_profile", config_.release_profile}}));
    return result;
  }
  if (!ReleaseProfileAllowsLocalTokenStore(config_.release_profile) &&
      LocalTokenStoreExplicitlyConfigured(config_)) {
    result.exit_code = 2;
    result.diagnostics.push_back(Diag("MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
                                      "Enterprise manager release profile forbids local TSV token stores.",
                                      {{"release_profile", config_.release_profile}}));
    return result;
  }
  AddEnterpriseSecretPolicyDiagnostics(config_, &result.diagnostics);
  if (!result.diagnostics.empty()) {
    result.exit_code = 2;
    return result;
  }
  if (config_.proxy_enabled) {
    bool missing_security_input = false;
    if (config_.mcp_secret_ref.empty() && ManagerSecurityTokenStorePath(config_).empty()) {
      result.diagnostics.push_back(Diag("MANAGER.SECRET_REQUIRED",
                                        "Manager proxy requires a security database temporary token store or manager.auth.mcp_secret_ref."));
      missing_security_input = true;
    }
    if (!config_.listener_control_socket_dir.empty() && config_.dbbt_keyring_path.empty()) {
      result.diagnostics.push_back(Diag("MANAGER.SECRET_REQUIRED", "Manager proxy requires manager.dbbt.keyring_path."));
      missing_security_input = true;
    }
    if (!config_.listener_control_socket_dir.empty() && !config_.owner_database_uuid_set) {
      result.diagnostics.push_back(Diag("MANAGER.SECRET_REQUIRED", "Manager proxy requires manager.owner.database_uuid for LPREFACE admission."));
      missing_security_input = true;
    }
    if (config_.listener_id == 0) {
      result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager listener id must be nonzero."));
      missing_security_input = true;
    }
    if (missing_security_input) {
      result.exit_code = 2;
      return result;
    }
  }
#ifdef _WIN32
  if (config_.service && !config_.validate_config) {
    result.exit_code = 2;
    result.diagnostics.push_back(Diag("MANAGER.SERVICE_MODE_UNSUPPORTED", "Windows service-control handoff is not implemented in this build."));
    return result;
  }
#else
  if (config_.service && !config_.validate_config) {
    if (!DaemonizeService(&result.diagnostics)) {
      result.exit_code = 2;
      return result;
    }
  }
#endif
  if (!PrepareRuntime(&result.diagnostics)) {
    if (lifecycle_started_) {
      (void)lifecycle_.Transition(ManagerLifecycleState::kStartupFailed,
                                  "runtime preparation failed",
                                  &result.diagnostics);
      (void)CleanupManagerRuntimeArtifactsImpl(config_, ManagerRuntimeCleanupOperation::kStop);
    }
    result.exit_code = 2;
    return result;
  }
  if (config_.service && !config_.validate_config) {
    if (!lifecycle_.Transition(ManagerLifecycleState::kDaemonizing,
                               "service daemonization complete",
                               &result.diagnostics)) {
      (void)lifecycle_.Transition(ManagerLifecycleState::kStartupFailed,
                                  "daemonizing lifecycle transition failed",
                                  &result.diagnostics);
      (void)CleanupManagerRuntimeArtifactsImpl(config_, ManagerRuntimeCleanupOperation::kStop);
      result.exit_code = 2;
      return result;
    }
  }
  if (config_.validate_config) {
    CleanupRuntime();
    return result;
  }
#ifndef _WIN32
  std::signal(SIGTERM, StopHandler);
  std::signal(SIGINT, StopHandler);
#endif
  auto fail_started = [&](const std::string& detail) {
    (void)lifecycle_.Transition(ManagerLifecycleState::kStartupFailed, detail, &result.diagnostics);
    result.exit_code = 2;
    StopAll();
    (void)CleanupManagerRuntimeArtifactsImpl(config_, ManagerRuntimeCleanupOperation::kStop);
    PublishMetricsSnapshot();
    return result;
  };
  auto transition_or_fail = [&](ManagerLifecycleState state, const std::string& detail) {
    return lifecycle_.Transition(state, detail, &result.diagnostics);
  };
  if (!transition_or_fail(ManagerLifecycleState::kServerEndpointResolving,
                          "server endpoint resolving")) {
    return fail_started("server endpoint lifecycle transition failed");
  }
  StartHeartbeat();
  if (!transition_or_fail(ManagerLifecycleState::kListenerEndpointResolving,
                          "listener endpoint resolving")) {
    return fail_started("listener endpoint lifecycle transition failed");
  }
  if (!transition_or_fail(ManagerLifecycleState::kProxyBinding, "proxy binding")) {
    return fail_started("proxy binding lifecycle transition failed");
  }
  StartProxy();
  if (!transition_or_fail(ManagerLifecycleState::kManagementBinding, "management binding")) {
    return fail_started("management binding lifecycle transition failed");
  }
  if (!StartControl(&result.diagnostics)) {
    return fail_started("management binding failed");
  }
  if (!transition_or_fail(ManagerLifecycleState::kReady, "ready")) {
    return fail_started("ready lifecycle transition failed");
  }
  PublishMetricsSnapshot();
  while (!g_stop_requested.load() && !stopping_.load()) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(250), [this]() { return stopping_.load(); });
  }
  if (!lifecycle_.Transition(ManagerLifecycleState::kDraining,
                             "shutdown drain begin",
                             &result.diagnostics)) {
    result.exit_code = 2;
  }
  StopAll();
  CleanupRuntime();
  PublishMetricsSnapshot();
  return result;
}

}  // namespace

std::string ManagerOwnerDatabaseRuntimeScopeId(const ManagerConfig& config) {
  return ManagerOwnerDatabaseRuntimeScopeIdImpl(config);
}

ManagerRuntimePaths ResolveManagerRuntimePaths(const ManagerConfig& config) {
  return ResolveManagerRuntimePathsImpl(config);
}

ManagerRuntimeValidation ValidateManagerRuntimeArtifacts(const ManagerConfig& config,
                                                         bool require_existing_files) {
  return ValidateManagerRuntimeArtifactsImpl(config, require_existing_files);
}

RuntimeResult CleanupManagerRuntimeArtifacts(const ManagerConfig& config,
                                             ManagerRuntimeCleanupOperation operation) {
  return CleanupManagerRuntimeArtifactsImpl(config, operation);
}

std::string ProductVersionLine() { return "sbmn_manager 0.1.0 product=sbmn_manager"; }

std::string HelpText() {
  return "sbmn_manager - ScratchBird Node Manager\n"
         "Usage: sbmn_manager [--foreground|--service|--validate-config] [options]\n"
         "  --config PATH\n  --runtime-dir PATH\n  --control-dir PATH\n"
         "  --bind ADDRESS --port PORT\n  --native-bind ADDRESS --native-port PORT\n"
         "  --management-max-clients N --management-max-payload-bytes BYTES\n"
         "  --management-idle-timeout-ms MS --management-backlog N\n"
         "  --owner-db NAME --owner-db-path PATH --owner-db-uuid UUID\n"
         "  --listener-id ID --listener-control-dir PATH\n"
         "  --security-token-store PATH\n"
         "  --mcp-secret-ref REF --mcp-secret-rights RIGHT[,RIGHT...]\n  --dbbt-keyring PATH\n"
         "  --release-profile enterprise|developer|test|native_only|diagnostic\n"
         "  --help\n  --version\n";
}

ParseResult ParseManagerCli(int argc, char** argv) {
  ParseResult result;
  bool explicit_config = false;
  bool explicit_control_dir = false;
  ApplyDefaultPaths(&result.config);
  const auto initial_auto_control_dir = result.config.control_dir;
  auto report_invalid_cli_value = [&](const std::string& option, const std::string& value) {
    result.diagnostics.push_back(Diag("MANAGER.CLI_VALUE_INVALID",
                                      "Manager CLI option value is invalid.",
                                      {{"option", option}, {"value", value}}));
  };
  auto parse_cli_u16 = [&](const std::string& option, const std::string& value, std::uint16_t* out) {
    if (ParseU16(value, out)) return;
    report_invalid_cli_value(option, value);
  };
  auto parse_cli_u32 = [&](const std::string& option, const std::string& value, std::uint32_t* out) {
    if (ParseU32(value, out)) return;
    report_invalid_cli_value(option, value);
  };
  auto parse_cli_nonzero_u32 = [&](const std::string& option, const std::string& value, std::uint32_t* out) {
    std::uint32_t parsed = 0;
    if (ParseU32(value, &parsed) && parsed != 0) {
      *out = parsed;
      return;
    }
    report_invalid_cli_value(option, value);
  };
  auto parse_cli_nonzero_u64 = [&](const std::string& option, const std::string& value, std::uint64_t* out) {
    std::uint64_t parsed = 0;
    if (ParseU64(value, &parsed) && parsed != 0) {
      *out = parsed;
      return;
    }
    report_invalid_cli_value(option, value);
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& option) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        result.diagnostics.push_back(Diag("MANAGER.CLI_UNKNOWN_OPTION", "Manager CLI option requires a value.", {{"option", option}}));
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };
    if (arg == "--help") result.config.help = true;
    else if (arg == "--version") result.config.version = true;
    else if (arg == "--foreground") result.config.foreground = true;
    else if (arg == "--service") result.config.service = true;
    else if (arg == "--validate-config") result.config.validate_config = true;
    else if (arg == "--config") { if (auto v = require_value(arg)) { result.config.config_path = *v; explicit_config = true; } }
    else if (arg == "--runtime-dir") { if (auto v = require_value(arg)) result.config.runtime_dir = *v; }
    else if (arg == "--control-dir") { if (auto v = require_value(arg)) { result.config.control_dir = *v; explicit_control_dir = true; } }
    else if (arg == "--bind") { if (auto v = require_value(arg)) result.config.bind_address = *v; }
    else if (arg == "--port") { if (auto v = require_value(arg)) parse_cli_u16(arg, *v, &result.config.proxy_port); }
    else if (arg == "--management-backlog") { if (auto v = require_value(arg)) parse_cli_nonzero_u32(arg, *v, &result.config.management_backlog); }
    else if (arg == "--management-max-clients") { if (auto v = require_value(arg)) parse_cli_nonzero_u32(arg, *v, &result.config.management_max_clients); }
    else if (arg == "--management-max-payload-bytes") {
      if (auto v = require_value(arg)) {
        parse_cli_nonzero_u32(arg, *v, &result.config.management_max_payload_bytes);
        if (result.config.management_max_payload_bytes > 16u * 1024u * 1024u) report_invalid_cli_value(arg, *v);
      }
    }
    else if (arg == "--management-idle-timeout-ms") { if (auto v = require_value(arg)) parse_cli_nonzero_u64(arg, *v, &result.config.management_idle_timeout_ms); }
    else if (arg == "--native-bind") { if (auto v = require_value(arg)) result.config.native_bind = *v; }
    else if (arg == "--native-port") { if (auto v = require_value(arg)) parse_cli_u16(arg, *v, &result.config.native_port); }
    else if (arg == "--owner-db") { if (auto v = require_value(arg)) result.config.owner_database_name = *v; }
    else if (arg == "--owner-db-path" || arg == "--default-database") {
      if (auto v = require_value(arg)) {
        result.config.owner_database_path = std::filesystem::absolute(*v).lexically_normal();
        if (result.config.owner_database_name.empty() || result.config.owner_database_name == "main") {
          result.config.owner_database_name = result.config.owner_database_path.string();
        }
      }
    }
    else if (arg == "--owner-db-uuid") {
      if (auto v = require_value(arg)) {
        result.config.owner_database_uuid_set = ParseUuidHex(*v, &result.config.owner_database_uuid);
        if (!result.config.owner_database_uuid_set) {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager owner database UUID is invalid.", {{"option", arg}}));
        }
      }
    }
    else if (arg == "--listener-id") { if (auto v = require_value(arg)) parse_cli_u32(arg, *v, &result.config.listener_id); }
    else if (arg == "--listener-control-dir") { if (auto v = require_value(arg)) result.config.listener_control_socket_dir = *v; }
    else if (arg == "--server-restart-executable") {
      if (auto v = require_value(arg)) {
        result.config.restart_executable = *v;
        if (!RestartExecutableValid(*v)) {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager server restart executable is invalid.", {{"option", arg}}));
        }
      }
    }
    else if (arg == "--server-restart-arguments") {
      if (auto v = require_value(arg)) {
        result.config.restart_arguments = *v;
        if (!RestartArgumentsValid(*v)) {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager server restart arguments are invalid.", {{"option", arg}}));
        }
      }
    }
    else if (arg == "--dbbt-keyring") { if (auto v = require_value(arg)) result.config.dbbt_keyring_path = *v; }
    else if (arg == "--security-token-store") { if (auto v = require_value(arg)) result.config.security_token_store_path = *v; }
    else if (arg == "--mcp-secret-ref") { if (auto v = require_value(arg)) result.config.mcp_secret_ref = *v; }
    else if (arg == "--mcp-secret-rights") {
      if (auto v = require_value(arg)) {
        std::vector<std::string> rights;
        if (ParseManagerRightsStrict(*v, &rights)) {
          result.config.mcp_secret_rights = *v;
        } else {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID",
                                            "Manager MCP secret rights list is invalid.",
                                            {{"option", arg}}));
        }
      }
    }
    else if (arg == "--release-profile") {
      if (auto v = require_value(arg)) {
        result.config.release_profile = *v;
        if (!ManagerReleaseProfileValid(*v)) report_invalid_cli_value(arg, *v);
      }
    }
    else if (arg == "--log") { if (auto v = require_value(arg)) result.config.log_path = *v; }
    else if (arg == "--log-level") { if (auto v = require_value(arg)) result.config.log_level = *v; }
    else result.diagnostics.push_back(Diag("MANAGER.CLI_UNKNOWN_OPTION", "Unknown manager CLI option.", {{"option", arg}}));
  }
  if (result.config.foreground && result.config.service) {
    result.diagnostics.push_back(Diag("MANAGER.CLI_MODE_CONFLICT", "Manager foreground and service modes conflict."));
  }
  LoadConfigFile(&result.config, &result.diagnostics, explicit_config);
  if (const char* level = std::getenv("SCRATCHBIRD_MANAGER_LOG_LEVEL")) result.config.log_level = level;
  if (result.config.mcp_secret_ref.empty()) {
    if (const char* secret = std::getenv("SCRATCHBIRD_MCP_AUTH_SECRET")) result.config.mcp_secret_ref = std::string("env:SCRATCHBIRD_MCP_AUTH_SECRET:") + std::to_string(std::strlen(secret));
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string(argv[++i]);
    };
    auto apply_nonzero_u32 = [](const std::string& text, std::uint32_t* out) {
      std::uint32_t parsed = 0;
      if (!ParseU32(text, &parsed) || parsed == 0) return;
      *out = parsed;
    };
    auto apply_nonzero_u64 = [](const std::string& text, std::uint64_t* out) {
      std::uint64_t parsed = 0;
      if (!ParseU64(text, &parsed) || parsed == 0) return;
      *out = parsed;
    };
    if (arg == "--config") { (void)value(); }
    else if (arg == "--runtime-dir") { if (auto v = value()) result.config.runtime_dir = *v; }
    else if (arg == "--control-dir") { if (auto v = value()) { result.config.control_dir = *v; explicit_control_dir = true; } }
    else if (arg == "--bind") { if (auto v = value()) result.config.bind_address = *v; }
    else if (arg == "--port") { if (auto v = value()) (void)ParseU16(*v, &result.config.proxy_port); }
    else if (arg == "--management-backlog") { if (auto v = value()) apply_nonzero_u32(*v, &result.config.management_backlog); }
    else if (arg == "--management-max-clients") { if (auto v = value()) apply_nonzero_u32(*v, &result.config.management_max_clients); }
    else if (arg == "--management-max-payload-bytes") {
      if (auto v = value()) {
        std::uint32_t parsed = 0;
        if (ParseU32(*v, &parsed) && parsed != 0 && parsed <= 16u * 1024u * 1024u) {
          result.config.management_max_payload_bytes = parsed;
        }
      }
    }
    else if (arg == "--management-idle-timeout-ms") { if (auto v = value()) apply_nonzero_u64(*v, &result.config.management_idle_timeout_ms); }
    else if (arg == "--native-bind") { if (auto v = value()) result.config.native_bind = *v; }
    else if (arg == "--native-port") { if (auto v = value()) (void)ParseU16(*v, &result.config.native_port); }
    else if (arg == "--owner-db") { if (auto v = value()) result.config.owner_database_name = *v; }
    else if (arg == "--owner-db-path" || arg == "--default-database") {
      if (auto v = value()) {
        result.config.owner_database_path = std::filesystem::absolute(*v).lexically_normal();
        if (result.config.owner_database_name.empty() || result.config.owner_database_name == "main") {
          result.config.owner_database_name = result.config.owner_database_path.string();
        }
      }
    }
    else if (arg == "--owner-db-uuid") {
      if (auto v = value()) {
        result.config.owner_database_uuid_set = ParseUuidHex(*v, &result.config.owner_database_uuid);
        if (!result.config.owner_database_uuid_set) {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager owner database UUID is invalid.", {{"option", arg}}));
        }
      }
    }
    else if (arg == "--listener-id") { if (auto v = value()) (void)ParseU32(*v, &result.config.listener_id); }
    else if (arg == "--listener-control-dir") { if (auto v = value()) result.config.listener_control_socket_dir = *v; }
    else if (arg == "--server-restart-executable") {
      if (auto v = value()) {
        result.config.restart_executable = *v;
        if (!RestartExecutableValid(*v)) {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager server restart executable is invalid.", {{"option", arg}}));
        }
      }
    }
    else if (arg == "--server-restart-arguments") {
      if (auto v = value()) {
        result.config.restart_arguments = *v;
        if (!RestartArgumentsValid(*v)) {
          result.diagnostics.push_back(Diag("MANAGER.CONFIG_FIELD_INVALID", "Manager server restart arguments are invalid.", {{"option", arg}}));
        }
      }
    }
    else if (arg == "--dbbt-keyring") { if (auto v = value()) result.config.dbbt_keyring_path = *v; }
    else if (arg == "--security-token-store") { if (auto v = value()) result.config.security_token_store_path = *v; }
    else if (arg == "--mcp-secret-ref") { if (auto v = value()) result.config.mcp_secret_ref = *v; }
    else if (arg == "--mcp-secret-rights") {
      if (auto v = value()) {
        std::vector<std::string> rights;
        if (ParseManagerRightsStrict(*v, &rights)) result.config.mcp_secret_rights = *v;
      }
    }
    else if (arg == "--release-profile") { if (auto v = value()) result.config.release_profile = *v; }
    else if (arg == "--log") { if (auto v = value()) result.config.log_path = *v; }
    else if (arg == "--log-level") { if (auto v = value()) result.config.log_level = *v; }
  }
  result.config.owner_database_runtime_scope_id = ManagerOwnerDatabaseRuntimeScopeIdImpl(result.config);
  if (!explicit_control_dir && result.config.control_dir == initial_auto_control_dir) {
    result.config.control_dir.clear();
  }
  ApplyDefaultPaths(&result.config);
  if (!result.config.no_spin_required) result.diagnostics.push_back(Diag("MANAGER.NO_SPIN_REQUIRED", "No-spin synchronization invariant must remain enabled."));
  return result;
}

RuntimeResult RunManager(const ManagerConfig& config) {
  return ManagerRuntime(config).Run();
}

}  // namespace scratchbird::manager::node
