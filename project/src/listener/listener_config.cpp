// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace scratchbird::listener {
namespace {

std::string Trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ParseBool(std::string_view value, bool* out) {
  const std::string lowered = Lower(std::string(value));
  if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
    *out = true;
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
    *out = false;
    return true;
  }
  return false;
}

template <typename T>
bool ParseUnsigned(std::string_view value, T* out) {
  if (value.empty()) {
    return false;
  }
  std::uint64_t tmp = 0;
  const char* begin = value.data();
  const char* end = value.data() + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, tmp);
  if (ec != std::errc() || ptr != end || tmp > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    return false;
  }
  *out = static_cast<T>(tmp);
  return true;
}

void AddError(std::vector<proto::Diagnostic>* diagnostics, std::string code, std::string message,
              std::vector<proto::Field> fields = {}) {
  diagnostics->push_back(MakeDiagnostic(std::move(code), "ERROR", std::move(message), "sb_listener.config", std::move(fields)));
}

bool ReadableRegularFile(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in);
}

bool ApplyKeyValue(ListenerConfig* cfg, std::string_view raw_key, std::string_view raw_value,
                   std::vector<proto::Diagnostic>* diagnostics) {
  std::string key = Lower(Trim(std::string(raw_key)));
  std::replace(key.begin(), key.end(), '-', '_');
  const std::string value = Trim(std::string(raw_value));
  if (key.empty()) {
    AddError(diagnostics, "LISTENER.CONFIG.EMPTY_KEY", "configuration key is empty");
    return false;
  }
  if (key == "listener_uuid") cfg->listener_uuid = value;
  else if (key == "listener_profile") cfg->listener_profile = value;
  else if (key == "listener_profile_uuid") cfg->listener_profile_uuid = value;
  else if (key == "lifecycle_generation") {
    if (!ParseUnsigned(value, &cfg->lifecycle_generation)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "lifecycle_generation must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  }
  else if (key == "controller_type") cfg->controller_type = Lower(value);
  else if (key == "controller_uuid") cfg->controller_uuid = value;
  else if (key == "protocol_family") cfg->protocol_family = Lower(value);
  else if (key == "parser_package") cfg->parser_package = value;
  else if (key == "parser_package_uuid") cfg->parser_package_uuid = value;
  else if (key == "dialect_profile_uuid") cfg->dialect_profile_uuid = value;
  else if (key == "bundle_contract_id") cfg->bundle_contract_id = value;
  else if (key == "parser_executable") cfg->parser_executable = value;
  else if (key == "database_selector") cfg->database_selector = value;
  else if (key == "server_endpoint") cfg->server_endpoint = value;
  else if (key == "bind_address") cfg->bind_address = value;
  else if (key == "control_dir") cfg->control_dir = value;
  else if (key == "runtime_dir") cfg->runtime_dir = value;
  else if (key == "metrics_namespace") cfg->metrics_namespace = value;
  else if (key == "language") cfg->language = value;
  else if (key == "dialect") cfg->dialect = value;
  else if (key == "tls_cert_file") cfg->tls_cert_file = value;
  else if (key == "tls_key_file") cfg->tls_key_file = value;
  else if (key == "tls_ca_file") cfg->tls_ca_file = value;
  else if (key == "port") {
    if (!ParseUnsigned(value, &cfg->port)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_PORT", "port must be an unsigned 16-bit integer", {{"value", value}});
      return false;
    }
  } else if (key == "parser_api_major") {
    if (!ParseUnsigned(value, &cfg->parser_api_major)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "parser_api_major must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "parser_api_minor") {
    if (!ParseUnsigned(value, &cfg->parser_api_minor)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "parser_api_minor must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "warm_pool_min") {
    if (!ParseUnsigned(value, &cfg->warm_pool_min)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "warm_pool_min must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "warm_pool_max") {
    if (!ParseUnsigned(value, &cfg->warm_pool_max)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "warm_pool_max must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "accept_backlog") {
    if (!ParseUnsigned(value, &cfg->accept_backlog)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "accept_backlog must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "idle_poll_ms") {
    if (!ParseUnsigned(value, &cfg->idle_poll_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "idle_poll_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "child_restart_base_ms") {
    if (!ParseUnsigned(value, &cfg->child_restart_base_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "child_restart_base_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "child_restart_max_ms") {
    if (!ParseUnsigned(value, &cfg->child_restart_max_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "child_restart_max_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "child_quarantine_failures") {
    if (!ParseUnsigned(value, &cfg->child_quarantine_failures)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "child_quarantine_failures must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "child_quarantine_window_ms") {
    if (!ParseUnsigned(value, &cfg->child_quarantine_window_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "child_quarantine_window_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "dbbt_ttl_ms") {
    if (!ParseUnsigned(value, &cfg->dbbt_ttl_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "dbbt_ttl_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "preauth_timeout_ms") {
    if (!ParseUnsigned(value, &cfg->preauth_timeout_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "preauth_timeout_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "handoff_ack_timeout_ms") {
    if (!ParseUnsigned(value, &cfg->handoff_ack_timeout_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "handoff_ack_timeout_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "graceful_drain_timeout_ms") {
    if (!ParseUnsigned(value, &cfg->graceful_drain_timeout_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "graceful_drain_timeout_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "management_poll_ms") {
    if (!ParseUnsigned(value, &cfg->management_poll_ms)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "management_poll_ms must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "per_client_max_connections") {
    if (!ParseUnsigned(value, &cfg->per_client_max_connections)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "per_client_max_connections must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "accept_rate_limit_per_second") {
    if (!ParseUnsigned(value, &cfg->accept_rate_limit_per_second)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "accept_rate_limit_per_second must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "accept_rate_limit_burst") {
    if (!ParseUnsigned(value, &cfg->accept_rate_limit_burst)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "accept_rate_limit_burst must be an unsigned integer", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "managed_by_server") {
    if (!ParseBool(value, &cfg->managed_by_server)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "managed_by_server must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "managed_by_manager") {
    if (!ParseBool(value, &cfg->managed_by_manager)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "managed_by_manager must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "tls_required") {
    if (!ParseBool(value, &cfg->tls_required)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "tls_required must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "allow_dev_dbbt_env") {
    if (!ParseBool(value, &cfg->allow_dev_dbbt_env)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "allow_dev_dbbt_env must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "allow_test_dbbt_builtin") {
    if (!ParseBool(value, &cfg->allow_test_dbbt_builtin)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "allow_test_dbbt_builtin must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "enable_metrics") {
    if (!ParseBool(value, &cfg->enable_metrics)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "enable_metrics must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "enable_accept_loop") {
    if (!ParseBool(value, &cfg->enable_accept_loop)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "enable_accept_loop must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "read_only") {
    if (!ParseBool(value, &cfg->read_only)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "read_only must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "maintenance_mode") {
    if (!ParseBool(value, &cfg->maintenance_mode)) {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "maintenance_mode must be boolean", {{"key", key}, {"value", value}});
      return false;
    }
  } else if (key == "spawn_strategy") {
    const std::string lowered = Lower(value);
    if (lowered == "warm_pool") cfg->spawn_strategy = ParserSpawnStrategy::kWarmPool;
    else if (lowered == "on_demand") cfg->spawn_strategy = ParserSpawnStrategy::kOnDemand;
    else {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_SPAWN_STRATEGY", "spawn_strategy must be warm_pool or on_demand", {{"value", value}});
      return false;
    }
  } else if (key == "dbbt_key_source") {
    const std::string lowered = Lower(value);
    if (lowered == "keyring") cfg->dbbt_key_source = DbbtKeySource::kKeyring;
    else if (lowered == "dev_environment") cfg->dbbt_key_source = DbbtKeySource::kDevEnvironment;
    else if (lowered == "test_builtin") cfg->dbbt_key_source = DbbtKeySource::kTestBuiltin;
    else {
      AddError(diagnostics, "LISTENER.CONFIG.INVALID_DBBT_KEY_SOURCE", "dbbt_key_source must be keyring, dev_environment, or test_builtin", {{"value", value}});
      return false;
    }
  } else {
    AddError(diagnostics, "LISTENER.CONFIG.UNKNOWN_KEY", "configuration key is not recognized", {{"key", key}});
    return false;
  }
  return true;
}

void ApplyProfileDefaults(ListenerConfig* cfg) {
  if (auto profile = FindDonorProtocolProfile(cfg->protocol_family)) {
    if (cfg->parser_package.empty()) {
      cfg->parser_package = profile->default_parser_package;
    }
    if (cfg->dialect.empty()) {
      cfg->dialect = profile->default_wire_protocol;
    }
    if (cfg->port == 0) {
      cfg->port = profile->default_port;
    }
  }
}

bool ValidateConfig(const ListenerConfig& cfg, std::vector<proto::Diagnostic>* diagnostics) {
  bool ok = true;
  if (!FindDonorProtocolProfile(cfg.protocol_family)) {
    AddError(diagnostics, "LISTENER.CONFIG.UNKNOWN_PROTOCOL_FAMILY", "protocol_family is not a registered source-backed donor family", {{"protocol_family", cfg.protocol_family}});
    ok = false;
  }
  if (cfg.database_selector.empty()) {
    AddError(diagnostics, "LISTENER.CONFIG.MISSING_DATABASE_SELECTOR", "database_selector is required and replaces parser-side database_path authority");
    ok = false;
  }
  if (cfg.server_endpoint.empty()) {
    AddError(diagnostics, "LISTENER.CONFIG.MISSING_SERVER_ENDPOINT", "server_endpoint is required so parser workers can reach sb_server");
    ok = false;
  }
  if (cfg.warm_pool_min > cfg.warm_pool_max) {
    AddError(diagnostics, "LISTENER.CONFIG.INVALID_POOL_RANGE", "warm_pool_min must be less than or equal to warm_pool_max",
             {{"warm_pool_min", std::to_string(cfg.warm_pool_min)}, {"warm_pool_max", std::to_string(cfg.warm_pool_max)}});
    ok = false;
  }
  if (cfg.child_restart_base_ms == 0 || cfg.child_restart_max_ms == 0) {
    AddError(diagnostics,
             "LISTENER.CONFIG.INVALID_CHILD_RESTART_BACKOFF",
             "child restart backoff values must be nonzero",
             {{"child_restart_base_ms", std::to_string(cfg.child_restart_base_ms)},
              {"child_restart_max_ms", std::to_string(cfg.child_restart_max_ms)}});
    ok = false;
  }
  if (cfg.child_restart_base_ms > cfg.child_restart_max_ms) {
    AddError(diagnostics,
             "LISTENER.CONFIG.INVALID_CHILD_RESTART_BACKOFF",
             "child_restart_base_ms must be less than or equal to child_restart_max_ms",
             {{"child_restart_base_ms", std::to_string(cfg.child_restart_base_ms)},
              {"child_restart_max_ms", std::to_string(cfg.child_restart_max_ms)}});
    ok = false;
  }
  if (cfg.child_quarantine_failures == 0 || cfg.child_quarantine_window_ms == 0) {
    AddError(diagnostics,
             "LISTENER.CONFIG.INVALID_CHILD_QUARANTINE",
             "child quarantine threshold and window must be nonzero",
             {{"child_quarantine_failures", std::to_string(cfg.child_quarantine_failures)},
              {"child_quarantine_window_ms", std::to_string(cfg.child_quarantine_window_ms)}});
    ok = false;
  }
  if (cfg.parser_api_major != 1) {
    AddError(diagnostics, "LISTENER.CONFIG.UNSUPPORTED_PARSER_API", "parser_api_major must be 1 for the first listener implementation", {{"parser_api_major", std::to_string(cfg.parser_api_major)}});
    ok = false;
  }
  if (cfg.lifecycle_generation == 0) {
    AddError(diagnostics, "LISTENER.START_INPUT_INVALID", "lifecycle_generation must be nonzero");
    ok = false;
  }
  if (cfg.bundle_contract_id.empty()) {
    AddError(diagnostics, "LISTENER.START_INPUT_INVALID", "bundle_contract_id is required for parser HELLO admission");
    ok = false;
  }
  if (cfg.tls_required) {
    if (cfg.protocol_family != "native" && cfg.protocol_family != "sbsql") {
      AddError(diagnostics,
               "LISTENER.TLS_POLICY_FAILED",
               "tls_required=true is currently supported only by native or sbsql SBWP listener workers",
               {{"protocol_family", cfg.protocol_family}});
      ok = false;
    }
    if (cfg.tls_cert_file.empty() || cfg.tls_key_file.empty()) {
      AddError(diagnostics,
               "LISTENER.TLS_POLICY_FAILED",
               "tls_required=true requires tls_cert_file and tls_key_file",
               {{"protocol_family", cfg.protocol_family}});
      ok = false;
    }
    if (!cfg.tls_cert_file.empty() && !ReadableRegularFile(cfg.tls_cert_file)) {
      AddError(diagnostics,
               "LISTENER.TLS_POLICY_FAILED",
               "tls_cert_file must be a readable regular file",
               {{"path", cfg.tls_cert_file}});
      ok = false;
    }
    if (!cfg.tls_key_file.empty() && !ReadableRegularFile(cfg.tls_key_file)) {
      AddError(diagnostics,
               "LISTENER.TLS_POLICY_FAILED",
               "tls_key_file must be a readable regular file",
               {{"path", cfg.tls_key_file}});
      ok = false;
    }
    if (!cfg.tls_ca_file.empty() && !ReadableRegularFile(cfg.tls_ca_file)) {
      AddError(diagnostics,
               "LISTENER.TLS_POLICY_FAILED",
               "tls_ca_file must be a readable regular file when supplied",
               {{"path", cfg.tls_ca_file}});
      ok = false;
    }
  }
  if (cfg.accept_rate_limit_burst != 0 && cfg.accept_rate_limit_per_second == 0) {
    AddError(diagnostics, "LISTENER.CONFIG.INVALID_VALUE", "accept_rate_limit_burst requires accept_rate_limit_per_second to be nonzero");
    ok = false;
  }
  if (cfg.dbbt_key_source == DbbtKeySource::kDevEnvironment && !cfg.allow_dev_dbbt_env) {
    AddError(diagnostics, "LISTENER.CONFIG.DEV_DBBT_KEY_DISABLED", "dev_environment DBBT keys require allow_dev_dbbt_env=true");
    ok = false;
  }
  if (cfg.dbbt_key_source == DbbtKeySource::kTestBuiltin && !cfg.allow_test_dbbt_builtin) {
    AddError(diagnostics, "LISTENER.CONFIG.TEST_DBBT_KEY_DISABLED", "test_builtin DBBT keys require allow_test_dbbt_builtin=true");
    ok = false;
  }
  return ok;
}

} // namespace

const std::vector<DonorProtocolProfile>& DonorProtocolProfiles() {
  static const std::vector<DonorProtocolProfile> profiles = {
      {"apache_ignite", "sbp_apache_ignite", "ignite.sql.v1", 10800, false},
      {"cassandra", "sbp_cassandra", "cql.v3", 9042, false},
      {"clickhouse", "sbp_clickhouse", "clickhouse.sql.v1", 9000, false},
      {"cockroachdb", "sbp_cockroachdb", "postgresql.wire.v3", 26257, false},
      {"dolt", "sbp_dolt", "mysql.wire.v10", 3306, false},
      {"duckdb", "sbp_duckdb", "duckdb.embedded.v1", 0, false},
      {"firebird", "sbp_firebird", "firebird.wire.v13", 3050, false},
      {"foundationdb", "sbp_foundationdb", "foundationdb.api.v7", 4500, false},
      {"immudb", "sbp_immudb", "immudb.sql.v1", 3322, false},
      {"influxdb", "sbp_influxdb", "influxql.v1", 8086, false},
      {"mariadb", "sbp_mariadb", "mysql.wire.v10", 3306, false},
      {"milvus", "sbp_milvus", "milvus.grpc.v2", 19530, false},
      {"mongodb", "sbp_mongodb", "mongodb.wire.v6", 27017, false},
      {"mysql", "sbp_mysql", "mysql.wire.v10", 3306, false},
      {"native", "sbp_native", "sbwp.v1", 3092, true},
      {"neo4j", "sbp_neo4j", "bolt.v5", 7687, false},
      {"opensearch", "sbp_opensearch", "opensearch.rest.v2", 9200, false},
      {"postgresql", "sbp_postgresql", "postgresql.wire.v3", 5432, false},
      {"redis", "sbp_redis", "resp.v3", 6379, false},
      {"sqlite", "sbp_sqlite", "sqlite.api.v3", 0, false},
      {"tidb", "sbp_tidb", "mysql.wire.v10", 4000, false},
      {"tikv", "sbp_tikv", "tikv.grpc.v1", 20160, false},
      {"vitess", "sbp_vitess", "mysql.wire.v10", 15306, false},
      {"xtdb", "sbp_xtdb", "xtql.v2", 3000, false},
      {"yugabytedb", "sbp_yugabytedb", "postgresql.wire.v3", 5433, false},
      {"sbsql", "sbp_sbsql", "sbsql.v3", 3050, true},
  };
  return profiles;
}

std::optional<DonorProtocolProfile> FindDonorProtocolProfile(std::string_view family) {
  const std::string needle = Lower(std::string(family));
  for (const auto& profile : DonorProtocolProfiles()) {
    if (profile.family == needle) {
      return profile;
    }
  }
  return std::nullopt;
}

ConfigResult LoadListenerConfigFile(const std::string& path, ListenerConfig base) {
  std::vector<proto::Diagnostic> diagnostics;
  base.config_path = path;
  std::ifstream input(path);
  if (!input) {
    AddError(&diagnostics, "LISTENER.CONFIG.FILE_OPEN_FAILED", "configuration file could not be opened", {{"path", path}});
    return {base, MakeMessageVectorSet(std::move(diagnostics), base.language, base.dialect), false};
  }

  std::string line;
  std::uint64_t line_number = 0;
  bool ok = true;
  while (std::getline(input, line)) {
    ++line_number;
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    const auto pos = trimmed.find('=');
    if (pos == std::string::npos) {
      AddError(&diagnostics, "LISTENER.CONFIG.INVALID_LINE", "configuration line must be key=value",
               {{"path", path}, {"line", std::to_string(line_number)}});
      ok = false;
      continue;
    }
    ok = ApplyKeyValue(&base, trimmed.substr(0, pos), trimmed.substr(pos + 1), &diagnostics) && ok;
  }
  ApplyProfileDefaults(&base);
  ok = ValidateConfig(base, &diagnostics) && ok;
  return {base, MakeMessageVectorSet(std::move(diagnostics), base.language, base.dialect), ok};
}

ConfigResult LoadListenerConfigFromArgs(int argc, char** argv) {
  ListenerConfig cfg;
  std::vector<proto::Diagnostic> diagnostics;
  bool ok = true;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      cfg.mode = ListenerMode::kHelp;
      return {cfg, MakeMessageVectorSet({}, cfg.language, cfg.dialect), true};
    }
    if (arg == "--version") {
      cfg.mode = ListenerMode::kVersion;
      return {cfg, MakeMessageVectorSet({}, cfg.language, cfg.dialect), true};
    }
    if (arg == "--validate-config") {
      cfg.mode = ListenerMode::kValidateConfig;
      continue;
    }
    if (arg == "--managed") {
      cfg.mode = ListenerMode::kManaged;
      cfg.managed_by_server = true;
      continue;
    }
    if (arg == "--foreground" || arg == "-F") {
      cfg.mode = ListenerMode::kForeground;
      continue;
    }
    if (arg.starts_with("--config=")) {
      cfg.config_path = arg.substr(std::string("--config=").size());
      continue;
    }
    if (arg.starts_with("--")) {
      const auto pos = arg.find('=');
      if (pos == std::string::npos) {
        AddError(&diagnostics, "LISTENER.CLI.INVALID_ARGUMENT", "listener option must use --key=value form", {{"argument", arg}});
        ok = false;
        continue;
      }
      ok = ApplyKeyValue(&cfg, arg.substr(2, pos - 2), arg.substr(pos + 1), &diagnostics) && ok;
      continue;
    }
    AddError(&diagnostics, "LISTENER.CLI.UNKNOWN_ARGUMENT", "listener command-line argument is not recognized", {{"argument", arg}});
    ok = false;
  }

  if (!cfg.config_path.empty()) {
    auto file_result = LoadListenerConfigFile(cfg.config_path, cfg);
    file_result.config.mode = cfg.mode;
    if (!diagnostics.empty()) {
      file_result.messages.diagnostics.insert(file_result.messages.diagnostics.begin(), diagnostics.begin(), diagnostics.end());
      file_result.ok = false;
    }
    return file_result;
  }
  ApplyProfileDefaults(&cfg);
  ok = ValidateConfig(cfg, &diagnostics) && ok;
  return {cfg, MakeMessageVectorSet(std::move(diagnostics), cfg.language, cfg.dialect), ok};
}

std::string ListenerModeName(ListenerMode mode) {
  switch (mode) {
    case ListenerMode::kForeground: return "foreground";
    case ListenerMode::kManaged: return "managed";
    case ListenerMode::kValidateConfig: return "validate_config";
    case ListenerMode::kVersion: return "version";
    case ListenerMode::kHelp: return "help";
  }
  return "unknown";
}

std::string SpawnStrategyName(ParserSpawnStrategy strategy) {
  switch (strategy) {
    case ParserSpawnStrategy::kWarmPool: return "warm_pool";
    case ParserSpawnStrategy::kOnDemand: return "on_demand";
  }
  return "unknown";
}

std::string DbbtKeySourceName(DbbtKeySource source) {
  switch (source) {
    case DbbtKeySource::kKeyring: return "keyring";
    case DbbtKeySource::kDevEnvironment: return "dev_environment";
    case DbbtKeySource::kTestBuiltin: return "test_builtin";
  }
  return "unknown";
}

std::string HelpText() {
  std::ostringstream out;
  out << "SBgate\n"
      << "  --config=<path>                  load key=value listener configuration\n"
      << "  --validate-config                validate configuration and exit\n"
      << "  --foreground|-F                  run in foreground\n"
      << "  --managed                        run under SBsrv/SBmgr control\n"
      << "  --protocol-family=<family>       source-backed donor or sbsql family\n"
      << "  --database-selector=<selector>   opaque server-side database selector\n"
      << "  --server-endpoint=<endpoint>     SBsrv IPC endpoint for parser workers\n"
      << "  --parser-executable=<path>       parser worker executable; SBParser is the core parser worker\n"
      << "  --bundle-contract-id=<id>        parser bundle contract expected in HELLO\n"
      << "  --tls-required=<bool>            require TLS before SBWP frames for native or sbsql listeners\n"
      << "  --tls-cert-file=<path>           listener TLS certificate chain\n"
      << "  --tls-key-file=<path>            listener TLS private key\n"
      << "  --tls-ca-file=<path>             optional CA bundle for TLS peer-proof policy\n"
      << "  --bind-address=<address>         network bind address\n"
      << "  --port=<port>                    listener port\n";
  out << "  --per-client-max-connections=<n> cap active connections per remote host; 0 disables\n"
      << "  --accept-rate-limit-per-second=<n> token-bucket accept rate; 0 disables\n"
      << "  --accept-rate-limit-burst=<n>    token-bucket burst; defaults to 2x rate when omitted\n";
  return out.str();
}

std::string VersionText() {
  return "SBgate 0.1 implementation-slice\n";
}

} // namespace scratchbird::listener
