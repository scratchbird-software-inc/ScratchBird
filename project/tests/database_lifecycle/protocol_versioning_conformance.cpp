// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "config_policy_security_lifecycle.hpp"
#include "database_format.hpp"
#include "disk_device.hpp"
#include "parser_server_ipc.hpp"
#include "sbps.hpp"
#include "server_ipc_lifecycle.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace legacy = scratchbird::parser::sbsql;
namespace ps = scratchbird::server::sbps;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasServerDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                         std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasLegacyDiagnostic(const legacy::MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013o_protocol.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013O protocol versioning test");
  return std::filesystem::path(made);
}

void TestSbpsVersionRefusals() {
  ps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(ps::MessageType::kPing);
  header.flags = ps::kFlagFinal;
  header.request_uuid = ps::MakeUuidV7Bytes();
  const std::vector<std::uint8_t> payload{'o', 'k'};
  const auto encoded = ps::EncodeFrame(header, payload);
  Require(ps::DecodeFrameBytes(encoded, 4096).ok(),
          "DBLC-013O current SBPS protocol version was not accepted");

  auto future_minor = encoded;
  future_minor[8] = 1;
  Require(HasServerDiagnostic(ps::DecodeFrameBytes(future_minor, 4096).diagnostics,
                              "PARSER_SERVER_IPC.PROTOCOL_VERSION_FUTURE"),
          "DBLC-013O future SBPS minor version did not fail closed");

  auto too_old_major = encoded;
  too_old_major[6] = 0;
  Require(HasServerDiagnostic(ps::DecodeFrameBytes(too_old_major, 4096).diagnostics,
                              "PARSER_SERVER_IPC.PROTOCOL_VERSION_TOO_OLD"),
          "DBLC-013O too-old SBPS major version did not fail closed");
}

void TestLegacyParserIpcVersionRefusals() {
  legacy::ParserServerPacket packet;
  packet.opcode = legacy::ParserServerOpcode::kParserHello;
  packet.protocol_version = legacy::kParserServerIpcProtocolCurrent;
  packet.payload = {'h', 'i'};
  legacy::MessageVectorSet messages;
  Require(legacy::DecodePacket(legacy::EncodePacket(packet), &messages).has_value(),
          "DBLC-013O current legacy parser IPC protocol was not accepted");

  packet.protocol_version = legacy::kParserServerIpcProtocolMaxSupported + 1;
  messages.diagnostics.clear();
  Require(!legacy::DecodePacket(legacy::EncodePacket(packet), &messages).has_value(),
          "DBLC-013O future legacy parser IPC protocol was admitted");
  Require(HasLegacyDiagnostic(messages, "PARSER_IPC.PROTOCOL.FUTURE_UNSUPPORTED"),
          "DBLC-013O future legacy parser IPC diagnostic missing");
}

server::ServerIpcEndpointDescriptor BaseEndpointDescriptor(const std::filesystem::path& work) {
  server::ServerBootstrapConfig config;
  config.control_dir = work / "control";
  config.sbps_endpoint = work / "control" / "sbps.sock";
  config.sbps_max_frame_bytes = 4096;
  config.sbps_max_streams = 4;
  config.config_source_epoch = 7;
  config.config_reload_generation = 8;
  config.capability_policy_generation = 9;
  config.security_policy_generation = 10;
  config.security_epoch = 11;
  config.cache_invalidation_epoch = 12;

  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 130;

  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_uuid = "018f58bd-98f0-7000-8000-0000000130aa";
  database.database_path = (work / "database.sbdb").string();
  database.config_source_epoch = 7;
  database.config_reload_generation = 8;
  database.capability_policy_generation = 9;
  database.policy_generation = 10;
  database.security_epoch = 11;
  database.cache_invalidation_epoch = 12;
  server::HostedEngineState engine;
  engine.engine_context_active = true;
  engine.databases.push_back(database);
  return server::BuildParserServerEndpointDescriptor(config, artifacts, engine);
}

void TestEndpointDescriptorVersionAndEpochs() {
  const auto base = BaseEndpointDescriptor(MakeTempDir());
  auto current = server::EvaluateServerIpcEndpointLifecycle(
      base, server::ServerIpcEndpointOperation::kParserHello);
  Require(current.admitted && current.descriptor_valid,
          "DBLC-013O current IPC endpoint descriptor was not admitted");

  auto future_descriptor = base;
  future_descriptor.descriptor_format_version =
      server::kServerIpcEndpointDescriptorFormatMaxSupported + 1;
  auto future_result = server::EvaluateServerIpcEndpointLifecycle(
      future_descriptor, server::ServerIpcEndpointOperation::kParserHello);
  Require(!future_result.admitted &&
              HasServerDiagnostic(future_result.diagnostics,
                                  "IPC.LIFECYCLE.DESCRIPTOR_VERSION_FUTURE"),
          "DBLC-013O future endpoint descriptor format was admitted");

  auto stale_epoch = base;
  stale_epoch.security_epoch = base.security_epoch - 1;
  auto stale_result = server::EvaluateServerIpcEndpointLifecycle(
      stale_epoch, server::ServerIpcEndpointOperation::kParserHello);
  Require(!stale_result.admitted && stale_result.stale_cleanup_required &&
              HasServerDiagnostic(stale_result.diagnostics, "IPC.LIFECYCLE.EPOCH_STALE"),
          "DBLC-013O stale cross-epoch IPC descriptor was admitted");
}

void TestConfigPolicySecurityVersionAndEpochs() {
  server::ServerBootstrapConfig config;
  config.config_source_epoch = 3;
  config.config_reload_generation = 4;
  config.capability_policy_generation = 5;
  config.security_policy_generation = 6;
  config.security_epoch = 7;
  config.security_provider_generation = 8;
  config.cache_invalidation_epoch = 8;
  auto input = server::BuildConfigPolicySecurityLifecycleInput(
      config,
      "/tmp/sb_dblc013o.sbdb",
      "018f58bd-98f0-7000-8000-0000000130bb",
      true,
      false);
  auto started = server::StartConfigPolicySecurityLifecycle(input);
  Require(started.ok(), "DBLC-013O current config/policy/security lifecycle was refused");

  auto future = input;
  future.descriptor_version =
      server::kConfigPolicySecurityLifecycleDescriptorMaxSupported + 1;
  auto future_result = server::StartConfigPolicySecurityLifecycle(future);
  Require(!future_result.accepted &&
              future_result.diagnostic.code == "ENGINE.DBLC_PROTOCOL_VERSION_FUTURE_REFUSED",
          "DBLC-013O future config/policy/security descriptor was admitted");

  const auto stale = server::ValidateConfigPolicySecurityAdmission(
      started.lifecycle,
      started.lifecycle.capability_policy_generation - 1,
      started.lifecycle.policy_generation,
      started.lifecycle.security_epoch,
      started.lifecycle.provider_generation,
      started.lifecycle.provider_family,
      "engine");
  Require(!stale.accepted &&
              stale.diagnostic.code == "ENGINE.DBLC_STALE_POLICY_REFUSED",
          "DBLC-013O stale config/policy/security epoch was admitted");
}

void TestPersistedFormatVersionRefusals() {
  auto db_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779131300000).value;
  auto header = disk::MakeDatabaseHeader(db_uuid.value, 16384, 1779131300001, 0, 1).header;
  header.format_major = 0;
  const auto too_old = disk::ValidateDatabaseHeader(header);
  Require(!too_old.ok() && too_old.diagnostic.diagnostic_code == "FORMAT.VERSION_TOO_OLD",
          "DBLC-013O too-old database header format was admitted");

  auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779131300002).value;
  auto startup = db::MakeInitialStartupState(db_uuid, filespace_uuid, 16384);
  startup.format_major = db::kStartupStateFormatMajorMaxSupported + 1;
  disk::FileDevice device;
  const auto work = MakeTempDir();
  Require(device.Open((work / "startup_state.sbdb").string(),
                      disk::FileOpenMode::create_or_truncate)
              .ok(),
          "DBLC-013O startup state test file could not be opened");
  const auto write = db::WriteStartupStatePageBody(&device, startup);
  Require(!write.ok() &&
              write.diagnostic.diagnostic_code == "SB-STARTUP-STATE-FORMAT-FUTURE",
          "DBLC-013O future startup-state format was admitted for write");
  (void)device.Close();
}

}  // namespace

int main() {
  TestSbpsVersionRefusals();
  TestLegacyParserIpcVersionRefusals();
  TestEndpointDescriptorVersionAndEpochs();
  TestConfigPolicySecurityVersionAndEpochs();
  TestPersistedFormatVersionRefusals();
  return EXIT_SUCCESS;
}
