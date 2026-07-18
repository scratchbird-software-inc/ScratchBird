// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "listener_orchestrator.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace server = scratchbird::server;

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
