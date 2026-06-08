// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime/parser_runtime.hpp"

#include "control_plane.hpp"
#include "lifecycle/parser_lifecycle.hpp"
#include "statement/statement_catalog.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace scratchbird::parser::sbsql {
namespace {

std::string Env(const char* name, std::string fallback = {}) {
  const char* value = std::getenv(name);
  return value == nullptr || *value == '\0' ? std::move(fallback) : std::string(value);
}

std::uint64_t ParseU64(const char* value, std::uint64_t fallback) {
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != nullptr && *end == '\0' ? static_cast<std::uint64_t>(parsed) : fallback;
}

std::string ValueAfter(std::string_view arg, std::string_view prefix) {
  if (arg.starts_with(prefix)) return std::string(arg.substr(prefix.size()));
  return {};
}

std::uint64_t ReadU64Payload(const std::vector<std::uint8_t>& data) {
  if (data.size() < 8) return 0;
  std::uint64_t out = 0;
  for (int i = 0; i < 8; ++i) out |= static_cast<std::uint64_t>(data[static_cast<std::size_t>(i)]) << (i * 8);
  return out;
}
#ifdef _WIN32
bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}

void CloseSocket(std::intptr_t* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::closesocket(static_cast<SOCKET>(*fd));
    *fd = -1;
  }
}

std::intptr_t ConnectListenerControlSocket(const std::string& path) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path) ||
      !EnsureWinsockInitialized()) {
    return -1;
  }
  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::closesocket(fd);
    return -1;
  }
  return static_cast<std::intptr_t>(fd);
}

std::intptr_t RehydrateWindowsClientSocket(
    const scratchbird::listener::WindowsSocketHandoffPayload& payload) {
  if (payload.socket_protocol_info.size() != sizeof(WSAPROTOCOL_INFOA) ||
      !EnsureWinsockInitialized()) {
    return -1;
  }
  WSAPROTOCOL_INFOA protocol_info{};
  std::memcpy(&protocol_info,
              payload.socket_protocol_info.data(),
              sizeof(protocol_info));
  SOCKET fd = ::WSASocketA(FROM_PROTOCOL_INFO,
                           FROM_PROTOCOL_INFO,
                           FROM_PROTOCOL_INFO,
                           &protocol_info,
                           0,
                           WSA_FLAG_OVERLAPPED);
  return fd == INVALID_SOCKET ? -1 : static_cast<std::intptr_t>(fd);
}
#else
void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}
#endif

} // namespace

std::string SbsqlParserLifecycleMappingReportJson() {
  std::size_t lifecycle_api_count = 0;
  std::size_t exact_diagnostic_count = 0;
  for (const auto& mapping : BuiltinSbsqlLifecycleMappings()) {
    if (mapping.disposition == LifecycleMappingDisposition::kScratchBirdLifecycleApi) {
      ++lifecycle_api_count;
    } else if (mapping.disposition == LifecycleMappingDisposition::kEmulatedNonFileDiagnostic &&
               mapping.exact_emulated_diagnostic) {
      ++exact_diagnostic_count;
    }
  }
  return "{\"gate\":\"DBLC_P14_DONOR_MAPPING_COMPLETE\","
         "\"static_gate\":\"DBLC_STATIC_NO_DONOR_ENGINE_SQL\","
         "\"dialect\":\"sbsql\","
         "\"lifecycle_api_mappings\":" + std::to_string(lifecycle_api_count) + ","
         "\"exact_emulated_non_file_diagnostics\":" + std::to_string(exact_diagnostic_count) + ","
         "\"engine_authority\":\"scratchbird\","
         "\"parser_executes_sql\":false,"
         "\"real_file_effects\":false,"
         "\"cross_dialect_dependencies\":false}";
}

ParserConfig ConfigFromArgs(int argc, char** argv, bool force_probe) {
  ParserConfig config;
  config.probe_mode = force_probe;
  config.allow_probe_auth = force_probe;
  config.listener_control_fd = static_cast<int>(ParseU64(std::getenv("SB_LISTENER_CONTROL_FD"), 0));
  config.listener_control_socket = Env("SB_LISTENER_CONTROL_SOCKET", "");
  config.worker_numeric_id = ParseU64(std::getenv("SB_PARSER_WORKER_NUMERIC_ID"), 1);
  config.parser_uuid = Env("SB_PARSER_UUID", "00000000-0000-7000-8000-00000000sbsq");
  config.listener_uuid = Env("SB_LISTENER_UUID", "");
  config.database_token = Env("SB_DATABASE_TOKEN", "");
  config.server_endpoint = Env("SB_SERVER_PARSER_IPC_ENDPOINT", Env("SB_SERVER_ENDPOINT", ""));
  config.dialect = Env("SB_PROTOCOL_FAMILY", "sbsql");
  config.profile_id = Env("SB_PARSER_PROFILE_ID", "default");
  config.bundle_contract_id = Env("SB_PARSER_BUNDLE_CONTRACT_ID", config.bundle_contract_id);
  config.build_id = Env("SB_PARSER_BUILD_ID", "dev");
  config.tls_required = Env("SB_TLS_REQUIRED", "0") == "1";
  config.tls_cert_file = Env("SB_TLS_CERT_FILE", "");
  config.tls_key_file = Env("SB_TLS_KEY_FILE", "");
  config.tls_ca_file = Env("SB_TLS_CA_FILE", "");
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--listener-worker") config.listener_worker = true;
    if (arg == "--probe-worker") {
      config.probe_mode = true;
      config.allow_probe_auth = true;
      config.bundle_contract_id = "sbp_probe@1";
    }
    if (arg == "--allow-probe-auth") config.allow_probe_auth = true;
    if (auto value = ValueAfter(arg, "--server-endpoint="); !value.empty()) config.server_endpoint = std::move(value);
    if (auto value = ValueAfter(arg, "--database-token="); !value.empty()) config.database_token = std::move(value);
    if (auto value = ValueAfter(arg, "--parser-uuid="); !value.empty()) config.parser_uuid = std::move(value);
    if (auto value = ValueAfter(arg, "--listener-uuid="); !value.empty()) config.listener_uuid = std::move(value);
    if (auto value = ValueAfter(arg, "--dialect="); !value.empty()) config.dialect = std::move(value);
    if (auto value = ValueAfter(arg, "--profile="); !value.empty()) config.profile_id = std::move(value);
    if (auto value = ValueAfter(arg, "--tls-cert-file="); !value.empty()) config.tls_cert_file = std::move(value);
    if (auto value = ValueAfter(arg, "--tls-key-file="); !value.empty()) config.tls_key_file = std::move(value);
    if (auto value = ValueAfter(arg, "--tls-required="); !value.empty()) {
      const auto normalized = ToUpperAscii(value);
      config.tls_required = normalized == "1" || normalized == "TRUE" || normalized == "YES" || normalized == "ON";
    }
  }
  return config;
}

int RunParserWorker(ParserConfig config) {
  ParserMetrics metrics;
  ParserLifecycle lifecycle;
  SblrTemplateCache cache(static_cast<std::size_t>(
      std::max<std::uint64_t>(1, config.resource_budget.max_parser_cache_entries)));
  metrics.SetState(ParserState::kInitializing);

  if (!config.listener_worker) {
    std::cout << "SBParser requires --listener-worker for production use. Use --probe-worker only under test policy.\n";
    return 0;
  }
  if (config.protocol_version < kSbsqlWorkerProtocolMinSupported) {
    std::cerr << "SBParser protocol version is too old for SBPS admission.\n";
    metrics.SetState(ParserState::kFailed);
    return 1;
  }
  if (config.protocol_version > kSbsqlWorkerProtocolMaxSupported) {
    std::cerr << "SBParser protocol version is newer than this worker supports.\n";
    metrics.SetState(ParserState::kFailed);
    return 1;
  }
#ifdef _WIN32
  config.listener_control_fd = ConnectListenerControlSocket(config.listener_control_socket);
#endif
  if (config.listener_control_fd <= 0) {
#ifdef _WIN32
    std::cerr << "SB_LISTENER_CONTROL_SOCKET is required.\n";
#else
    std::cerr << "SB_LISTENER_CONTROL_FD is required.\n";
#endif
    metrics.SetState(ParserState::kFailed);
    return 1;
  }
  if (!lifecycle.RecordWorkerSpawned().accepted) {
    std::cerr << "parser lifecycle rejected worker spawn.\n";
    metrics.SetState(ParserState::kFailed);
    return 1;
  }
  metrics.SetState(ParserState::kPackageAdmitted);

  scratchbird::listener::ParserHelloPayload hello;
  hello.protocol = config.dialect;
#ifdef _WIN32
  hello.pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  hello.pid = static_cast<std::uint32_t>(::getpid());
#endif
  hello.worker_id = config.worker_numeric_id;
  hello.dialect_protocol_version = config.protocol_version;
  hello.parser_api_major = config.parser_api_major;
  hello.profile_id = config.profile_id;
  hello.bundle_contract_id = config.bundle_contract_id;

  scratchbird::listener::ListenerControlFrame hello_frame;
  hello_frame.opcode = scratchbird::listener::ListenerControlOpcode::kHello;
  hello_frame.sequence = config.worker_numeric_id;
  hello_frame.payload = scratchbird::listener::EncodeHelloPayload(hello);
  if (!lifecycle.RecordHelloSent().accepted) {
    metrics.SetState(ParserState::kFailed);
    return 1;
  }
  if (!scratchbird::listener::SendControlFrame(config.listener_control_fd, hello_frame)) {
    lifecycle.RecordFailure("HELLO send failed");
#ifdef _WIN32
    CloseSocket(&config.listener_control_fd);
#endif
    metrics.SetState(ParserState::kFailed);
    return 1;
  }

  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(config.listener_control_fd, &decoded, &received_fd, 30000) ||
      decoded.frame.opcode != scratchbird::listener::ListenerControlOpcode::kHelloAck) {
#ifdef _WIN32
    CloseSocket(&config.listener_control_fd);
#else
    CloseFd(&received_fd);
#endif
    lifecycle.RecordFailure("HELLO_ACK was not received");
    metrics.SetState(ParserState::kFailed);
    return 1;
  }
#ifdef _WIN32
  (void)received_fd;
#else
  CloseFd(&received_fd);
#endif
  auto ack = scratchbird::listener::DecodeHelloAckPayload(decoded.frame.payload, &decoded.messages);
  if (!ack || !lifecycle.RecordHelloAck(ack->accepted).accepted) {
#ifdef _WIN32
    CloseSocket(&config.listener_control_fd);
#endif
    metrics.SetState(ParserState::kFailed);
    return 1;
  }

  metrics.SetState(ParserState::kIdlePreAuth);
  for (;;) {
    scratchbird::listener::ListenerControlDecodeResult inbound;
#ifdef _WIN32
    std::intptr_t client_fd = -1;
    int ignored_received_fd = -1;
    if (!scratchbird::listener::ReadControlFrame(config.listener_control_fd, &inbound, &ignored_received_fd, 300000)) {
      CloseSocket(&client_fd);
      CloseSocket(&config.listener_control_fd);
#else
    int client_fd = -1;
    if (!scratchbird::listener::ReadControlFrame(config.listener_control_fd, &inbound, &client_fd, 300000)) {
      CloseFd(&client_fd);
#endif
      lifecycle.RecordDisconnectRequested();
      lifecycle.RecordTerminated();
      metrics.SetState(ParserState::kDisconnected);
      return 0;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHealthCheck) {
      lifecycle.RecordIdlePreauthRelay("PING");
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kHealthReport;
      response.sequence = inbound.frame.sequence;
      const auto state = metrics.State();
      response.payload = scratchbird::listener::EncodeHealthReportPayload(
          scratchbird::listener::HealthReportPayload{inbound.frame.sequence,
                                                     static_cast<std::uint8_t>(
                                                         state == ParserState::kFailed ||
                                                                 state == ParserState::kQuarantined
                                                             ? 9
                                                             : 0),
                                                     0});
      scratchbird::listener::SendControlFrame(config.listener_control_fd, response);
      continue;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kManagementCommand) {
      const std::string command(inbound.frame.payload.begin(), inbound.frame.payload.end());
      const auto upper_command = ToUpperAscii(command);
      if (upper_command.find("FLUSH") != std::string::npos) cache.Flush();
      if (upper_command.find("DRAIN") != std::string::npos) metrics.SetState(ParserState::kDraining);
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kManagementResponse;
      response.sequence = inbound.frame.sequence;
      const auto text = upper_command.find("LIFECYCLE_MAPPING") != std::string::npos
                            ? SbsqlParserLifecycleMappingReportJson()
                            : std::string("sbp_sbsql management command accepted: ") + command;
      response.payload.assign(text.begin(), text.end());
      scratchbird::listener::SendControlFrame(config.listener_control_fd, response);
      if (upper_command.find("QUARANTINE") != std::string::npos) {
        lifecycle.RecordFailure("management quarantine requested");
        lifecycle.ApplyFailurePolicy(ParserFailurePolicy{1, 1});
        metrics.SetState(ParserState::kQuarantined);
#ifdef _WIN32
        CloseSocket(&config.listener_control_fd);
#endif
        return 1;
      }
      if (upper_command.find("RECYCLE") != std::string::npos) {
        lifecycle.RecordRecycleRequested();
        lifecycle.RecordTerminated();
        metrics.SetState(ParserState::kRecycled);
#ifdef _WIN32
        CloseSocket(&config.listener_control_fd);
#endif
        return 0;
      }
      if (upper_command.find("KILL") != std::string::npos) {
        lifecycle.RecordTerminateRequested();
        lifecycle.RecordTerminated();
        metrics.SetState(ParserState::kTerminating);
#ifdef _WIN32
        CloseSocket(&config.listener_control_fd);
#endif
        return 0;
      }
      if (upper_command.find("UNDRAIN") != std::string::npos) metrics.SetState(ParserState::kIdlePreAuth);
      continue;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kRecycle ||
        inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kShutdown) {
      if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kRecycle) {
        lifecycle.RecordRecycleRequested();
        lifecycle.RecordTerminated();
        metrics.SetState(ParserState::kRecycled);
      } else {
        lifecycle.RecordTerminateRequested();
        lifecycle.RecordTerminated();
        metrics.SetState(ParserState::kTerminating);
      }
#ifdef _WIN32
      CloseSocket(&config.listener_control_fd);
#endif
      return 0;
    }
    if (inbound.frame.opcode != scratchbird::listener::ListenerControlOpcode::kHandoffSocket) {
#ifdef _WIN32
      CloseSocket(&client_fd);
#else
      CloseFd(&client_fd);
#endif
      continue;
    }

#ifdef _WIN32
    std::optional<scratchbird::listener::HandoffSocketPayload> handoff_payload;
    if ((inbound.frame.flags & scratchbird::listener::kControlFlagWindowsSocketInfo) != 0) {
      auto windows_handoff =
          scratchbird::listener::DecodeWindowsSocketHandoffPayload(inbound.frame.payload,
                                                                   &inbound.messages);
      if (windows_handoff) {
        handoff_payload = windows_handoff->handoff;
        client_fd = RehydrateWindowsClientSocket(*windows_handoff);
      }
    } else {
      handoff_payload = scratchbird::listener::DecodeHandoffSocketPayload(inbound.frame.payload,
                                                                          &inbound.messages);
    }
#else
    auto handoff_payload = scratchbird::listener::DecodeHandoffSocketPayload(inbound.frame.payload,
                                                                             &inbound.messages);
#endif
    const auto connection_id = handoff_payload ? handoff_payload->connection_id : ReadU64Payload(inbound.frame.payload);
    scratchbird::listener::HandoffAckPayload handoff_ack;
    handoff_ack.connection_id_echo = connection_id == 0 ? inbound.frame.sequence : connection_id;
    handoff_ack.accepted = client_fd >= 0;
    handoff_ack.reason = handoff_ack.accepted ? "" : "client_fd_missing";
    scratchbird::listener::ListenerControlFrame response;
    response.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffAck;
    response.sequence = inbound.frame.sequence;
    response.payload = scratchbird::listener::EncodeHandoffAckPayload(handoff_ack);
    scratchbird::listener::SendControlFrame(config.listener_control_fd, response);
    if (client_fd < 0) continue;

    metrics.SetState(ParserState::kIdlePreAuth);
    ParserConfig session_config = config;
    if (handoff_payload) {
      session_config.manager_auth_provider_family = handoff_payload->auth_provider_family;
      session_config.manager_auth_principal = handoff_payload->auth_principal;
      session_config.manager_auth_token = handoff_payload->auth_token;
    }
    SbsqlTestWireSession session(session_config, &metrics, &cache);
    const int rc = session.ServeFd(client_fd);
#ifdef _WIN32
    CloseSocket(&client_fd);
#else
    CloseFd(&client_fd);
#endif
    if (rc == 0) {
      lifecycle.RecordDisconnectRequested();
      lifecycle.RecordTerminated();
      metrics.SetState(ParserState::kDisconnected);
    } else {
      lifecycle.RecordFailure("client handoff session failed");
      metrics.SetState(ParserState::kFailed);
    }
#ifdef _WIN32
    CloseSocket(&config.listener_control_fd);
#endif
    return rc;
  }
}

} // namespace scratchbird::parser::sbsql
