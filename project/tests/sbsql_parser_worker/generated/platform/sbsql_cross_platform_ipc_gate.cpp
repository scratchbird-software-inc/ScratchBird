// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"
#include "ipc/sbps_client.hpp"
#include "parser_package_registry.hpp"
#include "sbps.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace listener = scratchbird::listener;
namespace parser = scratchbird::parser::sbsql;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

namespace {

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string message) {
    if (condition) return;
    ok = false;
    if (failures < 100) std::cerr << message << '\n';
    ++failures;
  }
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasListenerDiagnostic(const listener::proto::MessageVectorSet& messages,
                           std::string_view code) {
  return std::any_of(messages.diagnostics.begin(), messages.diagnostics.end(),
                     [code](const listener::proto::Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool HasParserDiagnostic(const parser::MessageVectorSet& messages,
                         std::string_view code) {
  return std::any_of(messages.diagnostics.begin(), messages.diagnostics.end(),
                     [code](const parser::Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool HasServerDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                         std::string_view code) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [code](const server::ServerDiagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::array<std::uint8_t, 16> UuidSeed(std::uint8_t base) {
  std::array<std::uint8_t, 16> out{};
  for (std::size_t index = 0; index < out.size(); ++index) {
    out[index] = static_cast<std::uint8_t>(base + index);
  }
  return out;
}

void ValidateControlPlanePayloads(Harness* harness) {
  listener::ParserHelloPayload hello;
  hello.protocol = "sbsql";
  hello.pid = 1234;
  hello.worker_id = 99;
  hello.parser_api_major = listener::kCurrentParserApiMajor;
  hello.profile_id = "default";
  hello.bundle_contract_id = "sbp_sbsql@1";
  auto messages = listener::MakeMessageVectorSet({});
  const auto hello_payload = listener::EncodeHelloPayload(hello);
  const auto decoded_hello = listener::DecodeHelloPayload(hello_payload, &messages);
  harness->Check(decoded_hello && decoded_hello->protocol == hello.protocol &&
                     decoded_hello->pid == hello.pid &&
                     decoded_hello->bundle_contract_id == hello.bundle_contract_id,
                 "listener HELLO payload did not round trip");

  listener::ParserHelloPayload oversized_hello = hello;
  oversized_hello.profile_id.assign(2048, 'p');
  harness->Check(listener::EncodeHelloPayload(oversized_hello).empty(),
                 "oversized HELLO profile did not fail closed during encode");

  messages = listener::MakeMessageVectorSet({});
  harness->Check(!listener::DecodeHelloPayload({1, 2, 3}, &messages) &&
                     HasListenerDiagnostic(messages, "LISTENER.HELLO_TRUNCATED"),
                 "truncated HELLO did not produce LISTENER.HELLO_TRUNCATED");

  listener::HandoffSocketPayload handoff;
  handoff.connection_id = 42;
  handoff.protocol = "sbsql";
  handoff.client_addr = "127.0.0.1";
  handoff.client_port = 3050;
  handoff.db_uuid = UuidSeed(0x10);
  handoff.dbbt_id = UuidSeed(0x20);
  handoff.manager_session_id = UuidSeed(0x30);
  const auto handoff_payload = listener::EncodeHandoffSocketPayload(handoff);
  harness->Check(handoff_payload.size() == 193,
                 "HANDOFF_SOCKET payload size is not stable");

  listener::HandoffAckPayload ack;
  ack.connection_id_echo = handoff.connection_id;
  ack.accepted = false;
  ack.reason = "client_fd_missing";
  const auto decoded_ack =
      listener::DecodeHandoffAckPayload(listener::EncodeHandoffAckPayload(ack), &messages);
  harness->Check(decoded_ack && !decoded_ack->accepted &&
                     decoded_ack->reason == ack.reason &&
                     decoded_ack->connection_id_echo == handoff.connection_id,
                 "HANDOFF_ACK payload did not round trip");

  listener::SessionBindingReportPayload binding;
  binding.attachment_id = UuidSeed(0x40);
  binding.catalog_session_id = UuidSeed(0x50);
  binding.transaction_uuid = UuidSeed(0x60);
  binding.protocol_session_id = UuidSeed(0x70);
  binding.authenticated_principal_id = UuidSeed(0x80);
  binding.session_user_id = UuidSeed(0x90);
  binding.active_role_id = UuidSeed(0xA0);
  binding.authkey_id = UuidSeed(0xB0);
  binding.current_txn_id = 0x0102030405060708ULL;
  for (std::size_t index = 0; index < 256; ++index) {
    binding.effective_group_ids.push_back(UuidSeed(static_cast<std::uint8_t>(index)));
  }
  const auto binding_payload = listener::EncodeSessionBindingReportPayload(binding);
  const auto decoded_binding =
      listener::DecodeSessionBindingReportPayload(binding_payload, &messages);
  harness->Check(decoded_binding &&
                     decoded_binding->effective_group_ids.size() == 256 &&
                     decoded_binding->current_txn_id == binding.current_txn_id,
                 "SESSION_BINDING_REPORT max group payload did not round trip");
  binding.effective_group_ids.push_back(UuidSeed(0x01));
  harness->Check(listener::EncodeSessionBindingReportPayload(binding).empty(),
                 "SESSION_BINDING_REPORT above max groups did not fail closed");

  messages = listener::MakeMessageVectorSet({});
  harness->Check(!listener::DecodeTakeoverRequestPayload({0, 0, 0, 0}, &messages) &&
                     HasListenerDiagnostic(messages, "LISTENER.TAKEOVER_REQUEST_EMPTY"),
                 "empty TAKEOVER_REQUEST did not fail closed");
}

void ValidateSbpsAndParserEndpoint(Harness* harness) {
  sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPing);
  header.request_uuid = sbps::MakeUuidV7Bytes();
  header.connection_uuid = sbps::MakeUuidV7Bytes();
  std::vector<std::uint8_t> payload(128, 'x');
  const auto chunks = sbps::EncodeFrameSequence(header, payload, 16);
  harness->Check(chunks.size() > 1, "SBPS payload did not chunk below frame limit");
  for (const auto& chunk : chunks) {
    const auto decoded = sbps::DecodeFrameBytes(chunk, 16);
    harness->Check(decoded.ok(), "SBPS chunk frame did not decode under physical limit");
  }

  const auto oversized = sbps::EncodeFrame(header, payload);
  harness->Check(HasServerDiagnostic(sbps::DecodeFrameBytes(oversized, 16).diagnostics,
                                     "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID"),
                 "SBPS oversized physical frame did not fail closed");

  parser::SbpsClient client("unix:/" + std::string(256, 'x'));
  parser::MessageVectorSet messages;
  harness->Check(!client.SendHello(&messages) &&
                     HasParserDiagnostic(messages,
                                         "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG"),
                 "parser SBPS client did not reject an overlong endpoint path");
}

void ValidateParserChildPolicy(Harness* harness) {
  const auto policy = server::DefaultParserChildLaunchPolicy();
  const auto has_env = [&](std::string_view value) {
    return std::find(policy.env_whitelist.begin(), policy.env_whitelist.end(), value) !=
           policy.env_whitelist.end();
  };
  const auto has_handle = [&](std::string_view value) {
    return std::find(policy.inherited_handles.begin(),
                     policy.inherited_handles.end(),
                     value) != policy.inherited_handles.end();
  };
  harness->Check(has_env("PATH") && has_env("LANG") && has_env("LC_ALL") &&
                     has_env("TZ") && has_env("SCRATCHBIRD_CONFIG"),
                 "parser child environment whitelist is incomplete");
  harness->Check(has_handle("stdin:closed") &&
                     has_handle("stdout:server_log_pipe") &&
                     has_handle("stderr:server_log_pipe") &&
                     has_handle("sbps:endpoint_descriptor"),
                 "parser child inherited-handle policy is incomplete");
  harness->Check(policy.open_handle_limit <= 128 && policy.process_count == 1,
                 "parser child launch policy handle/process bounds regressed");
}

void ValidateSourceContracts(const std::filesystem::path& source_root,
                             Harness* harness) {
  const auto control_plane = ReadFile(source_root / "src/listener/control_plane.cpp");
  const auto control_plane_h = ReadFile(source_root / "src/listener/control_plane.hpp");
  const auto parser_pool = ReadFile(source_root / "src/listener/parser_pool.cpp");
  const auto listener_runtime = ReadFile(source_root / "src/listener/listener_runtime.cpp");
  const auto socket_identity = ReadFile(source_root / "src/listener/listener_socket_identity.cpp");
  const auto server_ipc = ReadFile(source_root / "src/server/ipc_server.cpp");
  const auto server_ipc_lifecycle = ReadFile(source_root / "src/server/server_ipc_lifecycle.cpp");
  const auto server_ipc_surface = server_ipc + "\n" + server_ipc_lifecycle;
  const auto orchestrator = ReadFile(source_root / "src/server/listener_orchestrator.cpp");
  const auto parser_runtime = ReadFile(source_root / "src/parsers/sbsql_worker/runtime/parser_runtime.cpp");
  const auto sbps_client = ReadFile(source_root / "src/parsers/sbsql_worker/ipc/sbps_client.cpp");

  harness->Check(Contains(control_plane_h, "kControlFlagHasHandle") &&
                     Contains(control_plane, "SCM_RIGHTS") &&
                     Contains(control_plane, "CMSG_SPACE") &&
                     Contains(control_plane, "CONTROL.PAYLOAD_TOO_LARGE") &&
                     Contains(control_plane, "CONTROL.READ_TIMEOUT"),
                 "listener control plane lacks handle-transfer or bounded-read evidence");
  harness->Check(Contains(parser_pool, "socketpair(AF_UNIX") &&
                     Contains(parser_pool, "SB_LISTENER_CONTROL_FD") &&
                     Contains(parser_pool, "SB_SERVER_ENDPOINT") &&
                     Contains(parser_pool, "execl") &&
                     Contains(parser_pool, "_exit(127)"),
                 "parser pool lacks expected Unix control-fd/env/exec evidence");
  harness->Check(Contains(listener_runtime, "LISTENER.MANAGEMENT_SOCKET_PATH_TOO_LONG") &&
                     Contains(listener_runtime, "EnsureWinsockInitialized") &&
                     Contains(listener_runtime, "::socket(AF_UNIX, SOCK_STREAM, 0)") &&
                     Contains(listener_runtime, "IPV6_V6ONLY") &&
                     Contains(listener_runtime, "sizeof(addr.sun_path)"),
                 "listener runtime lacks path-limit or Windows transport evidence");
  harness->Check(Contains(socket_identity, "ExistingOwnerIsLive") &&
                     Contains(socket_identity, "::kill") &&
                     Contains(socket_identity, "clean_shutdown_required=true"),
                 "listener socket identity lacks owner-token live-pid cleanup evidence");
  harness->Check(Contains(server_ipc_surface, "format=SBPS_ENDPOINT_V1") &&
                     Contains(server_ipc_surface, "descriptor.transport = \"af_unix\"") &&
                     Contains(server_ipc_surface, "PARSER_SERVER_IPC.ENDPOINT_DESCRIPTOR_WRITE_FAILED") &&
                     Contains(server_ipc_surface, "sizeof(addr.sun_path)") &&
                     Contains(server_ipc_surface, "#ifdef _WIN32"),
                 "server IPC endpoint descriptor/path/platform evidence is incomplete");
  harness->Check(Contains(orchestrator, "CreateProcessA") &&
                     Contains(orchestrator, "OpenProcess") &&
                     Contains(orchestrator, "TerminateProcess") &&
                     Contains(orchestrator, "LISTENER.MANAGEMENT_SOCKET_INVALID") &&
                     Contains(orchestrator, "sizeof(addr.sun_path)"),
                 "server listener orchestrator lacks Windows lifecycle or management path evidence");
  harness->Check(Contains(parser_runtime, "SB_LISTENER_CONTROL_FD") &&
                     Contains(parser_runtime, "SB_LISTENER_CONTROL_SOCKET") &&
                     Contains(parser_runtime, "DecodeWindowsSocketHandoffPayload") &&
                     Contains(parser_runtime, "WSASocketA(FROM_PROTOCOL_INFO") &&
                     Contains(parser_runtime, "SB_SERVER_PARSER_IPC_ENDPOINT") &&
                     Contains(parser_runtime, "client_fd_missing") &&
                     Contains(parser_runtime, "#ifdef _WIN32"),
                 "parser runtime lacks listener handoff/env/platform evidence");
  harness->Check(Contains(sbps_client, "EnsureWinsockInitialized") &&
                     Contains(sbps_client, "using SbpsSocketHandle = SOCKET") &&
                     Contains(sbps_client, "::socket(AF_UNIX, SOCK_STREAM, 0)") &&
                     !Contains(sbps_client, "PARSER_SERVER_IPC.UNSUPPORTED_PLATFORM") &&
                     Contains(sbps_client, "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG") &&
                     Contains(sbps_client, "unix:") &&
                     Contains(sbps_client, "sizeof(addr.sun_path)"),
                 "parser SBPS client lacks Windows AF_UNIX or endpoint path-limit evidence");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sbsql_cross_platform_ipc_gate <project-source-root>\n";
    return 1;
  }

  Harness harness;
  try {
    const auto source_root = std::filesystem::path(argv[1]);
    ValidateControlPlanePayloads(&harness);
    ValidateSbpsAndParserEndpoint(&harness);
    ValidateParserChildPolicy(&harness);
    ValidateSourceContracts(source_root, &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-012D cross-platform IPC gate failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-012D cross-platform IPC gate passed: "
              << "control_plane=true sbps=true platform_refusals=true "
              << "path_limits=true child_policy=true\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-012D cross-platform IPC gate failed: "
              << ex.what() << '\n';
    return 1;
  }
}
