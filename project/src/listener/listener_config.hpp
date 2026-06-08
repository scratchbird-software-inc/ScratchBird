// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "listener_diagnostics.hpp"

namespace scratchbird::listener {

enum class ListenerMode {
  kForeground,
  kManaged,
  kValidateConfig,
  kVersion,
  kHelp,
};

enum class ParserSpawnStrategy {
  kWarmPool,
  kOnDemand,
};

enum class ParserTrustMode {
  kUntrustedExternalProcess,
};

enum class DbbtKeySource {
  kKeyring,
  kDevEnvironment,
  kTestBuiltin,
};

struct DonorProtocolProfile {
  std::string family;
  std::string default_parser_package;
  std::string default_wire_protocol;
  std::uint16_t default_port;
  bool management_surface_allowed{false};
};

struct ListenerConfig {
  ListenerMode mode{ListenerMode::kForeground};
  ParserSpawnStrategy spawn_strategy{ParserSpawnStrategy::kWarmPool};
  ParserTrustMode trust_mode{ParserTrustMode::kUntrustedExternalProcess};
  DbbtKeySource dbbt_key_source{DbbtKeySource::kKeyring};

  std::string config_path;
  std::string listener_uuid;
  std::string listener_profile{"default"};
  std::string listener_profile_uuid;
  std::uint64_t lifecycle_generation{1};
  std::string controller_type{"direct"};
  std::string controller_uuid;
  std::string protocol_family{"sbsql"};
  std::string parser_package;
  std::string parser_package_uuid;
  std::string dialect_profile_uuid;
  std::string bundle_contract_id{"bundle.default@1"};
  std::string parser_executable;
  std::string database_selector;
  std::string server_endpoint;
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{3050};
  std::string control_dir{"/tmp/scratchbird/listener/control"};
  std::string runtime_dir{"/tmp/scratchbird/listener/runtime"};
  std::string metrics_namespace{"sys.metrics.listener"};
  std::string language{"en"};
  std::string dialect;
  std::string tls_cert_file;
  std::string tls_key_file;
  std::string tls_ca_file;

  std::uint32_t parser_api_major{1};
  std::uint32_t parser_api_minor{0};
  std::uint32_t warm_pool_min{1};
  std::uint32_t warm_pool_max{16};
  std::uint32_t accept_backlog{128};
  std::uint32_t idle_poll_ms{250};
  std::uint32_t child_restart_base_ms{250};
  std::uint32_t child_restart_max_ms{30000};
  std::uint32_t child_quarantine_failures{5};
  std::uint32_t child_quarantine_window_ms{60000};
  std::uint32_t dbbt_ttl_ms{30000};
  std::uint32_t preauth_timeout_ms{30000};
  std::uint32_t handoff_ack_timeout_ms{2000};
  std::uint32_t graceful_drain_timeout_ms{30000};
  std::uint32_t management_poll_ms{250};
  std::uint32_t per_client_max_connections{0};
  std::uint32_t accept_rate_limit_per_second{0};
  std::uint32_t accept_rate_limit_burst{0};
  bool managed_by_server{false};
  bool managed_by_manager{false};
  bool tls_required{false};
  bool allow_dev_dbbt_env{false};
  bool allow_test_dbbt_builtin{false};
  bool enable_metrics{true};
  bool enable_accept_loop{true};
  bool read_only{false};
  bool maintenance_mode{false};
};

struct ConfigResult {
  ListenerConfig config;
  proto::MessageVectorSet messages;
  bool ok{true};
};

const std::vector<DonorProtocolProfile>& DonorProtocolProfiles();
std::optional<DonorProtocolProfile> FindDonorProtocolProfile(std::string_view family);
ConfigResult LoadListenerConfigFromArgs(int argc, char** argv);
ConfigResult LoadListenerConfigFile(const std::string& path, ListenerConfig base = {});
std::string ListenerModeName(ListenerMode mode);
std::string SpawnStrategyName(ParserSpawnStrategy strategy);
std::string DbbtKeySourceName(DbbtKeySource source);
std::string HelpText();
std::string VersionText();

} // namespace scratchbird::listener
