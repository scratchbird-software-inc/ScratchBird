// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "listener_orchestrator.hpp"
#include "uuid.hpp"
#include "management_request_codec.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <set>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace {

namespace server = scratchbird::server;
using NativeUuid = scratchbird::core::platform::Uuid;
static_assert(std::is_same_v<decltype(server::ServerListenerProfileRuntime::listener_uuid), NativeUuid>);
static_assert(std::is_same_v<decltype(server::ServerListenerProfileRuntime::listener_profile_uuid), NativeUuid>);
static_assert(std::is_same_v<decltype(server::ServerListenerOperationResult::target_uuid), NativeUuid>);

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

server::ServerListenerProfileConfig Profile(std::string key,
                                            std::string protocol,
                                            std::string package,
                                            std::string parser,
                                            std::uint64_t port) {
  server::ServerListenerProfileConfig profile;
  profile.config_key = std::move(key);
  profile.enabled = true;
  profile.protocol_family = std::move(protocol);
  profile.profile_id = profile.config_key + ".profile";
  profile.parser_package = std::move(package);
  profile.parser_package_uuid = profile.config_key + ".package.uuid";
  profile.dialect_profile_uuid = profile.config_key + ".dialect.uuid";
  profile.bundle_contract_id = profile.config_key + ".bundle@1";
  profile.parser_api_major = 1;
  profile.parser_executable_path = std::move(parser);
  profile.bind_address = "127.0.0.1";
  profile.port = port;
  profile.database_selector = "database_uuid:" + profile.config_key;
  profile.sbps_endpoint = "/tmp/" + profile.config_key + ".sbps.sock";
  profile.control_dir = "/tmp/" + profile.config_key + ".control";
  profile.runtime_dir = "/tmp/" + profile.config_key + ".runtime";
  profile.tls_required = false;
  profile.ready_timeout_ms = 2500;
  profile.warm_pool_min = 1;
  profile.warm_pool_max = 3;
  return profile;
}

}  // namespace

int main() {
  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 17;

  server::ServerBootstrapConfig empty;
  const auto empty_orchestrator = server::BuildListenerOrchestrator(empty, artifacts);
  Require(empty_orchestrator.profiles.empty(),
          "unconfigured server synthesized a listener profile");
  Require(empty_orchestrator.diagnostics.empty(),
          "empty listener inventory produced diagnostics");

  server::ServerBootstrapConfig configured;
  configured.listener_executable_path = "/opt/scratchbird/bin/SBgate";
  configured.listener_profiles.push_back(Profile(
      "future_alpha", "Vendor.Future-A", "vendor.future.alpha", "/opt/parsers/future-a", 42101));
  configured.listener_profiles.push_back(Profile(
      "future_beta", "Vendor.Future-B", "vendor.future.beta", "/opt/parsers/future-b", 42102));

  const auto orchestrator = server::BuildListenerOrchestrator(configured, artifacts);
  Require(orchestrator.diagnostics.empty(),
          "complete opaque profiles were refused");
  Require(orchestrator.profiles.size() == 2,
          "server did not retain every configured listener profile");
  const auto& alpha = orchestrator.profiles[0];
  const auto& beta = orchestrator.profiles[1];
  Require(alpha.listener_executable_path == configured.listener_executable_path.string() &&
              beta.listener_executable_path == configured.listener_executable_path.string(),
          "profiles did not use the same generic SBgate executable");
  Require(alpha.protocol_family == "Vendor.Future-A" &&
              beta.protocol_family == "Vendor.Future-B",
          "opaque protocol discriminators were rewritten or registry-filtered");
  Require(alpha.parser_executable_path == "/opt/parsers/future-a" &&
              beta.parser_executable_path == "/opt/parsers/future-b",
          "parser executables were inferred or substituted");
  Require(alpha.port == 42101 && beta.port == 42102,
          "explicit ports were defaulted or rewritten");
  Require(alpha.engine_endpoint == "/tmp/future_alpha.sbps.sock" &&
              beta.engine_endpoint == "/tmp/future_beta.sbps.sock",
          "per-profile SBPS endpoints were replaced by a global endpoint");
  Require(alpha.state == "stopped" && beta.state == "stopped",
          "valid enabled profiles were not launch-ready");

  std::set<NativeUuid> identities;
  for (unsigned iteration = 0; iteration != 32; ++iteration) {
    // The same captions and lifecycle generation must never issue the same
    // runtime ownership identity for separate controller instances.
    const auto fresh = server::BuildListenerOrchestrator(configured, artifacts);
    for (const auto& profile : fresh.profiles) {
      for (const auto& id : {profile.listener_uuid, profile.listener_profile_uuid}) {
        Require(scratchbird::core::uuid::IsEngineIdentityUuid(id), "non-v7 listener identity");
        Require(identities.insert(id).second, "listener identities reused across owners/roles");
      }
    }
    const auto status = server::ListenerOrchestratorStatusJson(fresh);
    Require(status == server::ListenerOrchestratorStatusJson(fresh), "status minted new identities");
    Require(status.find(scratchbird::core::uuid::UuidToString(fresh.profiles[0].listener_uuid)) !=
                std::string::npos, "status lost rendered listener UUID");
  }
  auto targeted = orchestrator;
  const auto refuse_target = [&](NativeUuid id) {
    const auto before = server::ListenerOrchestratorStatusJson(targeted);
    const auto generation = targeted.generation;
    const auto result = server::ApplyListenerOperation(
        &targeted, configured, artifacts, "stop_listener", id, "force");
    Require(!result.ok && !result.diagnostics.empty() &&
                result.diagnostics.front().code == "LISTENER.NOT_FOUND",
            "invalid/ambiguous target selected a listener");
    Require(targeted.generation == generation &&
                server::ListenerOrchestratorStatusJson(targeted) == before,
            "refused target changed listener state");
  };
  refuse_target({});
  refuse_target(*identities.begin());
  auto invalid_version = alpha.listener_uuid;
  invalid_version.bytes[6] = (invalid_version.bytes[6] & 0x0f) | 0x40;
  refuse_target(invalid_version);
  targeted.profiles.push_back(targeted.profiles[1]);
  refuse_target(beta.listener_uuid);
  targeted.profiles.pop_back();
  const auto stopped = server::ApplyListenerOperation(
      &targeted, configured, artifacts, "stop_listener", beta.listener_uuid, "force");
  Require(stopped.ok && stopped.target_uuid == beta.listener_uuid &&
              targeted.profiles[0].enabled && !targeted.profiles[1].enabled &&
              targeted.profiles[1].listener_profile_uuid == beta.listener_profile_uuid,
          "binary target did not select precisely the second listener");
  NativeUuid decoded;
  Require(scratchbird::wire::DecodeManagementTarget(
              scratchbird::wire::ManagementTargetBytes(beta.listener_uuid), &decoded) &&
              decoded == beta.listener_uuid,
          "management transport changed native listener identity");
  Require(!scratchbird::wire::DecodeManagementTarget(beta.profile_name, &decoded) &&
              !scratchbird::wire::DecodeManagementTarget(
                  scratchbird::core::uuid::UuidToString(beta.listener_uuid), &decoded),
          "management target accepted caption/text UUID instead of binary16");

  // NUL, punctuation and high octets are identity data, not string terminators,
  // field delimiters or display encodings. Exercise every permitted byte value
  // at every position through the real binary target decoder and selector.
  // Use one record: adjacent issued identities can differ in just one octet;
  // duplicate ownership is tested independently above.
  targeted.profiles.erase(targeted.profiles.begin());
  for (std::size_t position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto id = beta.listener_uuid;
      id.bytes[position] = static_cast<std::uint8_t>(octet);
      if (!scratchbird::core::uuid::IsEngineIdentityUuid(id)) continue;
      targeted.profiles[0].listener_uuid = id;
      Require(scratchbird::wire::DecodeManagementTarget(
                  scratchbird::wire::ManagementTargetBytes(id), &decoded),
              "binary listener target rejected a permitted octet");
      const auto generation = targeted.generation;
      const auto selected = server::ApplyListenerOperation(
          &targeted, configured, artifacts, "listener_proxy_execute", decoded, "");
      Require(!selected.ok && selected.target_uuid == id &&
                  selected.diagnostics.size() == 1 &&
                  selected.diagnostics.front().code == "LISTENER.EXECUTION_PROXY_FORBIDDEN" &&
                  targeted.generation == generation,
              "binary selector lost exact target or enabled forbidden execution proxy");
    }
  }

  server::ServerBootstrapConfig invalid;
  invalid.listener_executable_path = "/opt/scratchbird/bin/SBgate";
  auto missing_port = Profile(
      "missing_port", "Opaque.Protocol", "opaque.package", "/opt/parsers/opaque", 0);
  invalid.listener_profiles.push_back(std::move(missing_port));
  const auto refused = server::BuildListenerOrchestrator(invalid, artifacts);
  Require(refused.profiles.size() == 1 && refused.profiles.front().state == "failed",
          "missing explicit port did not fail closed");
  Require(!refused.diagnostics.empty() &&
              refused.profiles.front().diagnostic_code == "LISTENER.PORT_INVALID",
          "missing explicit port did not produce the required diagnostic");

  server::ServerBootstrapConfig missing_api_config;
  missing_api_config.listener_executable_path = "/opt/scratchbird/bin/SBgate";
  auto missing_api = Profile(
      "missing_api", "Opaque.Protocol", "opaque.package", "/opt/parsers/opaque", 42104);
  missing_api.parser_api_major = 0;
  missing_api_config.listener_profiles.push_back(std::move(missing_api));
  const auto api_refused =
      server::BuildListenerOrchestrator(missing_api_config, artifacts);
  Require(api_refused.profiles.size() == 1 &&
              api_refused.profiles.front().state == "failed",
          "missing explicit parser API major did not fail closed");
  Require(!api_refused.diagnostics.empty() &&
              api_refused.profiles.front().diagnostic_code ==
                  "LISTENER.PARSER_API_MAJOR_REQUIRED",
          "missing explicit parser API major did not produce the required diagnostic");

  const auto status = server::ListenerOrchestratorStatusJson(orchestrator);
  Require(status.find("Vendor.Future-A") != std::string::npos &&
              status.find("/opt/parsers/future-b") != std::string::npos,
          "listener status did not preserve opaque configured identity");

  std::string template_path = "/tmp/sb_server_listener_profile.XXXXXX";
  std::vector<char> writable(template_path.begin(), template_path.end());
  writable.push_back('\0');
  const char* temp_root = ::mkdtemp(writable.data());
  Require(temp_root != nullptr, "could not create listener profile config fixture");
  const auto config_path = std::filesystem::path(temp_root) / "sb_server.conf";
  {
    std::ofstream out(config_path);
    out << "[config]\nformat=SBCD1\n"
        << "[server.listener]\n"
        << "executable_path=/opt/scratchbird/bin/SBgate\n"
        << "[server.listener.profile.configured_future]\n"
        << "enabled=true\n"
        << "protocol_family=ThirdParty.FutureWire\n"
        << "profile_id=thirdparty.future.profile\n"
        << "parser_package=thirdparty.future.package\n"
        << "parser_package_uuid=thirdparty-future-package-v1\n"
        << "dialect_profile_uuid=thirdparty-future-dialect-v1\n"
        << "bundle_contract_id=thirdparty.future.bundle@1\n"
        << "parser_api_major=1\n"
        << "parser_executable_path=/opt/parsers/thirdparty-future\n"
        << "bind_address=127.0.0.1\n"
        << "port=42103\n"
        << "database_selector=database_uuid:configured-future\n"
        << "sbps_endpoint=/tmp/configured-future.sbps.sock\n"
        << "tls_required=false\n";
    Require(static_cast<bool>(out), "could not write listener profile config fixture");
  }
  server::ServerCliOptions no_listeners;
  no_listeners.config_path = config_path.string();
  no_listeners.no_listeners = true;
  const auto resolved = server::ResolveServerBootstrapConfig(no_listeners);
  Require(resolved.ok(), "generic listener profile config did not parse");
  Require(resolved.config.listener_profiles.size() == 1,
          "generic listener profile config did not produce one record");
  Require(!resolved.config.listener_profiles.front().enabled,
          "--no-listeners did not disable the configured generic profile");
  Require(resolved.config.listener_profiles.front().protocol_family ==
              "ThirdParty.FutureWire",
          "config parsing rewrote the opaque protocol discriminator");

  std::cout << "server_generic_listener_profile_probe=passed\n";
  return EXIT_SUCCESS;
}
