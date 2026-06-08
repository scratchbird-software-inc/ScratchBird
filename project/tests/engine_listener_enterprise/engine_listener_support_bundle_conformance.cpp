// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"
#include "listener_support_bundle.hpp"
#include "listener_socket_identity.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int FindFreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_eler068.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireContains(const std::string& text,
                     const std::string& needle,
                     const std::string& message) {
  if (text.find(needle) == std::string::npos) {
    std::cerr << message << "\nneedle=" << needle << "\ntext=" << text << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireNotContains(const std::string& text,
                        const std::string& needle,
                        const std::string& message) {
  if (text.find(needle) != std::string::npos) {
    std::cerr << message << "\nneedle=" << needle << "\ntext=" << text << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path FindManagementSocket(const std::filesystem::path& control_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(control_dir, ec)) return {};
  for (const auto& entry : std::filesystem::directory_iterator(control_dir, ec)) {
    if (ec) return {};
    const auto path = entry.path();
    const auto name = path.filename().string();
    if (name.size() >= std::string(".management.sock").size() &&
        name.ends_with(".management.sock")) {
      return path;
    }
  }
  return {};
}

int ConnectUnix(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto text = path.string();
  if (text.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, text.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct CommandResult {
  bool got_frame{false};
  bool ok{false};
  std::string body;
};

CommandResult SendPayload(const std::filesystem::path& socket_path,
                          const std::vector<std::uint8_t>& payload,
                          std::uint64_t sequence) {
  CommandResult result;
  const int fd = ConnectUnix(socket_path);
  if (fd < 0) return result;
  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = sequence;
  frame.payload = payload;
  if (!scratchbird::listener::SendControlFrame(fd, frame)) {
    ::close(fd);
    return result;
  }
  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, 3000)) {
    if (received_fd >= 0) ::close(received_fd);
    ::close(fd);
    return result;
  }
  if (received_fd >= 0) ::close(received_fd);
  ::close(fd);
  result.got_frame = decoded.frame.opcode ==
                         scratchbird::listener::ListenerControlOpcode::kManagementResponse &&
                     !decoded.frame.payload.empty();
  if (!result.got_frame) return result;
  result.ok = decoded.frame.payload[0] == 0;
  result.body.assign(decoded.frame.payload.begin() + 1, decoded.frame.payload.end());
  return result;
}

void StopChild(pid_t child) {
  if (child <= 0) return;
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(child, &status, WNOHANG) == child) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (::waitpid(child, &status, WNOHANG) != child) {
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
  }
}

std::vector<std::uint8_t> RawPayload(const std::string& command) {
  return std::vector<std::uint8_t>(command.begin(), command.end());
}

std::vector<std::uint8_t> EnvelopePayload(const std::string& command,
                                          std::uint64_t sequence,
                                          std::uint64_t issued_at_ms,
                                          std::uint64_t expires_at_ms) {
  auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
      command,
      sequence,
      issued_at_ms,
      expires_at_ms,
      "manager",
      "listener_manager",
      scratchbird::listener::kListenerManagementAuthPeerOwner);
  if (!envelope) return {};
  return scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
}

void DirectSupportBundleProof() {
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create direct proof temp dir");
  scratchbird::listener::ListenerConfig config;
  config.protocol_family = "sbsql";
  config.listener_uuid = "listener-eler068-direct";
  config.listener_profile = "enterprise";
  config.database_selector = "dev_bootstrap_path:/tmp/secret-canary-eler068-direct.sbdb";
  config.server_endpoint = "unix:/tmp/secret-canary-eler068-direct.sbps.sock";
  config.parser_executable =
      std::string("/") + "home" + "/dcalford/secret-canary-eler068-parser";
  config.tls_key_file = "/tmp/secret-canary-eler068.key";
  config.bundle_contract_id = "bundle.default@1-secret-canary-eler068";
  config.control_dir = (work / "control").string();
  config.runtime_dir = (work / "runtime").string();
  config.lifecycle_generation = 68;
  const auto identity = scratchbird::listener::BuildSocketIdentity(config);
  std::string error;
  Require(scratchbird::listener::WriteOwnerToken(identity, &error),
          "owner token for direct bundle proof failed: " + error);
  Require(scratchbird::listener::WriteLifecycleStateToken(identity,
                                                          "running",
                                                          "running",
                                                          scratchbird::listener::SocketIdentityJson(identity),
                                                          "{\"running\":true}",
                                                          &error),
          "lifecycle token for direct bundle proof failed: " + error);

  scratchbird::listener::ListenerSupportBundleSnapshot snapshot;
  snapshot.config = config;
  snapshot.identity = identity;
  snapshot.lifecycle_state = "running";
  snapshot.accepting_new_connections = true;
  snapshot.pool_status.running = true;
  snapshot.pool_status.running_worker_count = 1;
  snapshot.metrics_json = "{\"sys.metrics.listener.secret_canary\":1}";
  for (int i = 0; i < 70; ++i) {
    scratchbird::listener::ParserPoolFaultEvent fault;
    fault.timestamp_ms = static_cast<std::uint64_t>(1000 + i);
    fault.worker_id = "worker-" + std::to_string(i);
    fault.event = "crash";
    fault.diagnostic = "secret-canary-eler068-fault /tmp/private-parser-" + std::to_string(i);
    snapshot.pool_status.fault_history.push_back(std::move(fault));

    scratchbird::listener::ListenerSupportBundleEvent event;
    event.timestamp_ms = static_cast<std::uint64_t>(2000 + i);
    event.event_type = "management_decision";
    event.operation = "DRAIN";
    event.outcome = "accepted";
    event.diagnostic_code = "LISTENER.DRAIN.OK";
    event.safe_detail = "secret-canary-eler068-management";
    snapshot.management_decisions.push_back(event);
  }
  snapshot.runtime_events.push_back({3001,
                                     "handoff_failure",
                                     "PARSER_HANDOFF",
                                     "refused",
                                     "LISTENER.HANDOFF.FAIL",
                                     "secret-canary-eler068-handoff"});
  snapshot.runtime_events.push_back({3002,
                                     "auth_refusal",
                                     "DBBT_VALIDATE",
                                     "refused",
                                     "MCP.DBBT_INVALID",
                                     "secret-canary-eler068-auth"});

  const auto bundle = scratchbird::listener::BuildListenerSupportBundleJson(snapshot);
  RequireContains(bundle, "\"schema\":\"SB_LISTENER_SUPPORT_BUNDLE_V1\"",
                  "support bundle schema marker missing");
  RequireContains(bundle, "\"support_bundle_is_authority\":false",
                  "support bundle must explicitly deny authority");
  RequireContains(bundle, "\"parser_worker_faults\":{\"count\":64,\"source_count\":70",
                  "parser worker fault history must be bounded and source-counted");
  RequireContains(bundle, "\"management_decisions\":{\"count\":64,\"source_count\":70",
                  "management decision history must be bounded and source-counted");
  RequireContains(bundle, "\"handoff_failures\":1", "handoff failure summary missing");
  RequireContains(bundle, "\"auth_refusals\":1", "auth refusal summary missing");
  RequireContains(bundle, "[redacted:security]", "security redaction marker missing");
  RequireContains(bundle, "[path-redacted]", "path redaction marker missing");
  RequireNotContains(bundle, "secret-canary-eler068", "support bundle leaked secret canary");
  RequireNotContains(bundle, "/tmp/private-parser", "support bundle leaked local path");
  RequireNotContains(bundle, "worker-0", "support bundle did not discard oldest fault history");
  RequireContains(bundle, "worker-69", "support bundle did not retain newest fault history");

  std::error_code ec;
  std::filesystem::remove_all(work, ec);
}

void LiveManagementSupportBundleProof(const std::string& listener_path,
                                      const std::string& parser_path) {
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create live proof temp dir");
  const int port = FindFreePort();
  Require(port > 0, "could not allocate listener port");
  const auto control_dir = work / "control";
  const auto runtime_dir = work / "runtime";
  const std::string canary = "secret-canary-eler068-live";
  std::vector<std::string> args = {
      listener_path,
      "--foreground",
      "--spawn-strategy=warm_pool",
      "--parser-executable=" + parser_path,
      "--protocol-family=sbsql",
      "--listener-uuid=listener-eler068-live",
      "--listener-profile=enterprise",
      "--bundle-contract-id=bundle.default@1-" + canary,
      "--database-selector=dev_bootstrap_path:/tmp/" + canary + ".sbdb",
      "--server-endpoint=unix:/tmp/" + canary + ".sbps.sock",
      "--control-dir=" + control_dir.string(),
      "--runtime-dir=" + runtime_dir.string(),
      "--bind-address=127.0.0.1",
      "--port=" + std::to_string(port),
      "--warm-pool-min=1",
      "--warm-pool-max=1",
      "--allow-test-dbbt-builtin=true",
      "--dbbt-key-source=test_builtin",
      "--idle-poll-ms=25",
      "--management-poll-ms=25",
      "--lifecycle-generation=68"};
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  const pid_t child = ::fork();
  Require(child >= 0, "fork failed");
  if (child == 0) {
    ::execv(listener_path.c_str(), argv.data());
    std::_Exit(127);
  }

  std::filesystem::path management_socket;
  for (int i = 0; i < 160; ++i) {
    management_socket = FindManagementSocket(control_dir);
    if (!management_socket.empty()) {
      auto probe = SendPayload(management_socket, RawPayload("STATUS"), 1);
      if (probe.got_frame && probe.ok) break;
    }
    int status = 0;
    if (::waitpid(child, &status, WNOHANG) == child) {
      std::cerr << "listener exited before support bundle proof\n";
      std::exit(EXIT_FAILURE);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  Require(!management_socket.empty(), "management socket was not created");

  const auto raw = SendPayload(management_socket, RawPayload("SUPPORT_BUNDLE"), 2);
  Require(raw.got_frame && !raw.ok, "raw support bundle command must be refused");
  RequireContains(raw.body, "LISTENER.MANAGEMENT.ENVELOPE_REQUIRED",
                  "raw support bundle refusal did not require envelope");

  const auto now = scratchbird::listener::proto::CurrentEpochMilliseconds();
  Require(SendPayload(management_socket,
                      EnvelopePayload("DRAIN", 3, now - 100, now + 30000),
                      3).ok,
          "authenticated DRAIN failed");
  Require(SendPayload(management_socket,
                      EnvelopePayload("UNDRAIN", 4, now - 100, now + 30000),
                      4).ok,
          "authenticated UNDRAIN failed");
  const auto auth_refusal = SendPayload(
      management_socket,
      EnvelopePayload("DBBT_VALIDATE 00", 5, now - 100, now + 30000),
      5);
  Require(auth_refusal.got_frame && !auth_refusal.ok,
          "invalid DBBT_VALIDATE should fail and record auth refusal");

  const auto support_payload = EnvelopePayload("SUPPORT_BUNDLE", 6, now - 100, now + 30000);
  Require(!support_payload.empty(), "SUPPORT_BUNDLE envelope did not build");
  const auto bundle = SendPayload(
      management_socket,
      support_payload,
      6);
  if (!bundle.got_frame || !bundle.ok) {
    (void)SendPayload(management_socket,
                      EnvelopePayload("STOP FORCE", 7, now - 100, now + 30000),
                      7);
    StopChild(child);
    std::cerr << "authenticated SUPPORT_BUNDLE failed got_frame="
              << (bundle.got_frame ? "true" : "false")
              << " ok=" << (bundle.ok ? "true" : "false")
              << " body=" << bundle.body << '\n';
    std::exit(EXIT_FAILURE);
  }
  RequireContains(bundle.body, "\"schema\":\"SB_LISTENER_SUPPORT_BUNDLE_V1\"",
                  "live support bundle schema marker missing");
  RequireContains(bundle.body, "\"management_decisions\"",
                  "live support bundle missing management decisions");
  RequireContains(bundle.body, "\"auth_refusals\":1",
                  "live support bundle missing auth refusal summary");
  RequireContains(bundle.body, "\"lifecycle_evidence\"",
                  "live support bundle missing lifecycle evidence");
  RequireContains(bundle.body, "\"redaction_profile\":\"listener.support_bundle.default_redaction.v1\"",
                  "live support bundle missing redaction profile");
  RequireNotContains(bundle.body, canary, "live support bundle leaked canary");
  RequireNotContains(bundle.body, "/tmp/" + canary, "live support bundle leaked local path");

  (void)SendPayload(management_socket,
                    EnvelopePayload("STOP FORCE", 7, now - 100, now + 30000),
                    7);
  StopChild(child);
  std::error_code ec;
  std::filesystem::remove_all(work, ec);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: engine_listener_support_bundle_conformance <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  DirectSupportBundleProof();
  LiveManagementSupportBundleProof(argv[1], argv[2]);
  std::cout << "engine_listener_support_bundle_conformance=passed\n";
  return EXIT_SUCCESS;
}
