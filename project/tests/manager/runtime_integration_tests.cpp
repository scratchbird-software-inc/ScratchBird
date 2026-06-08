// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_MANAGER_RUNTIME_INTEGRATION_TESTS

#include "manager_protocol.hpp"
#include "manager_runtime.hpp"
#include "control_plane.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace proto = scratchbird::manager::protocol;
namespace node = scratchbird::manager::node;
namespace listener = scratchbird::listener;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

bool HasDiagnostic(const std::vector<proto::Diagnostic>& diagnostics,
                   const std::string& code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void PutU16(proto::Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(proto::Bytes* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

std::uint16_t ReadU16(const proto::Bytes& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off] | (static_cast<std::uint16_t>(data[off + 1]) << 8u));
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

std::string ReadLpstr(const proto::Bytes& data, std::size_t* off) {
  if (*off + 4 > data.size()) return {};
  const auto len = ReadU32(data, *off);
  *off += 4;
  if (*off + len > data.size()) return {};
  std::string out(data.begin() + static_cast<std::ptrdiff_t>(*off),
                  data.begin() + static_cast<std::ptrdiff_t>(*off + len));
  *off += len;
  return out;
}

const std::string* ManagementArg(const listener::ListenerManagementEnvelope& envelope,
                                 const std::string& key) {
  for (const auto& arg : envelope.arguments) {
    if (arg.key == key) return &arg.value;
  }
  return nullptr;
}

std::string CommandFromListenerManagementPayload(const proto::Bytes& payload,
                                                 std::uint64_t request_id) {
  if (!listener::IsListenerManagementEnvelopePayload(payload)) {
    Check(false, "listener-control stub must receive SBME envelope payloads for every manager command");
    return {};
  }
  proto::MessageVectorSet messages;
  auto envelope = listener::DecodeListenerManagementEnvelope(payload, &messages);
  Check(envelope.has_value(), "listener-control stub must receive a decodable SBME envelope");
  if (!envelope) return {};
  Check(envelope->version == 1, "listener-control SBME envelope version must be 1");
  Check(envelope->request_id == std::to_string(request_id),
        "listener-control SBME request id must match control-plane request id");
  Check(envelope->authenticator_scheme == listener::kListenerManagementAuthPeerOwner,
        "listener-control privileged command must use peer-owner auth in local tests");
  if (envelope->operation == "LPREFACE_VALIDATE") {
    const auto* preface = ManagementArg(*envelope, "preface_hex");
    return preface == nullptr ? "LPREFACE_VALIDATE" : "LPREFACE_VALIDATE " + *preface;
  }
  if (envelope->operation == "STOP") {
    const auto* force = ManagementArg(*envelope, "force");
    return force != nullptr && *force == "true" ? "STOP FORCE" : "STOP GRACEFUL";
  }
  if (envelope->operation == "POOL_RESTART") return "POOL RESTART";
  return envelope->operation;
}

proto::Bytes AuthStartPayload(const std::string& token = "integration-secret",
                              const std::string& username = "integration") {
  proto::Bytes out;
  PutLpstr(&out, username);
  out.push_back(4);
  PutU32(&out, static_cast<std::uint32_t>(token.size()));
  out.insert(out.end(), token.begin(), token.end());
  return out;
}

proto::Bytes HelloPayload() {
  proto::Bytes out;
  PutU16(&out, 0x0100);
  PutU16(&out, 0);
  return out;
}

proto::Bytes ManagerCommandPayload(const std::string& operation, const std::string& idempotency_key) {
  proto::Bytes out = {'M', 'C', 'P', '1'};
  PutLpstr(&out, operation);
  PutLpstr(&out, idempotency_key);
  PutU32(&out, 0);
  return out;
}

proto::Bytes ShutdownPayload(const std::string& idempotency_key) {
  proto::Bytes out = {'M', 'C', 'P', '1'};
  PutLpstr(&out, idempotency_key);
  return out;
}

proto::Bytes ManagerCommandPayloadWithArgs(
    const std::string& operation,
    const std::string& idempotency_key,
    const std::vector<std::pair<std::string, std::string>>& args) {
  proto::Bytes out = {'M', 'C', 'P', '1'};
  PutLpstr(&out, operation);
  PutLpstr(&out, idempotency_key);
  PutU32(&out, static_cast<std::uint32_t>(args.size()));
  for (const auto& arg : args) {
    PutLpstr(&out, arg.first);
    PutLpstr(&out, arg.second);
  }
  return out;
}

proto::Bytes ExtendedDbConnectPayload(const std::string& database_name,
                                      const std::string& connection_profile,
                                      const std::string& client_intent,
                                      const proto::Bytes& client_nonce = {}) {
  proto::Bytes out = {'M', 'C', 'P', '1'};
  PutLpstr(&out, database_name);
  PutLpstr(&out, connection_profile);
  PutLpstr(&out, client_intent);
  PutU16(&out, static_cast<std::uint16_t>(client_nonce.size()));
  out.insert(out.end(), client_nonce.begin(), client_nonce.end());
  return out;
}

std::vector<std::pair<std::string, std::string>> StatusEntries(const proto::SbdbFrame& frame, std::uint32_t* appended_mvs_len) {
  std::vector<std::pair<std::string, std::string>> entries;
  if (appended_mvs_len) *appended_mvs_len = 0;
  if (frame.type != 0x64 || frame.payload.size() < 5) return entries;
  std::size_t off = 1;
  const auto count = ReadU32(frame.payload, off);
  off += 4;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto key = ReadLpstr(frame.payload, &off);
    auto value = ReadLpstr(frame.payload, &off);
    entries.push_back({std::move(key), std::move(value)});
  }
  if (appended_mvs_len && off + 4 <= frame.payload.size()) *appended_mvs_len = ReadU32(frame.payload, off);
  return entries;
}

std::string EntryValue(const std::vector<std::pair<std::string, std::string>>& entries, const std::string& key) {
  for (const auto& entry : entries) {
    if (entry.first == key) return entry.second;
  }
  return {};
}

std::uint64_t JsonU64Value(const std::string& json, const std::string& key) {
  const auto needle = "\"" + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return 0;
  std::size_t off = pos + needle.size();
  while (off < json.size() && std::isspace(static_cast<unsigned char>(json[off]))) ++off;
  std::uint64_t value = 0;
  while (off < json.size() && std::isdigit(static_cast<unsigned char>(json[off]))) {
    value = (value * 10u) + static_cast<std::uint64_t>(json[off] - '0');
    ++off;
  }
  return value;
}

std::uint16_t ConnectServerFlags(const proto::SbdbFrame& frame) {
  if (frame.type != 0x02 || frame.payload.size() < 5) return 0;
  return ReadU16(frame.payload, 3);
}

std::string ConnectErrorText(const proto::SbdbFrame& frame) {
  if (frame.type != 0x02 || frame.payload.size() <= 117) return {};
  std::size_t off = 117;
  return ReadLpstr(frame.payload, &off);
}

bool FileContainsWithin(const std::filesystem::path& path,
                        const std::string& needle,
                        std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream in(path);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (text.find(needle) != std::string::npos) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

#ifndef _WIN32
std::uint16_t ReserveLoopbackPort() {
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
  const auto port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}
#endif

#ifndef _WIN32
bool RecvExact(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t got = 0;
  while (got < size) {
    const auto rc = ::recv(fd, data + got, size - got, 0);
    if (rc <= 0) return false;
    got += static_cast<std::size_t>(rc);
  }
  return true;
}

bool SendAll(int fd, const proto::Bytes& bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto rc = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
    if (rc <= 0) return false;
    sent += static_cast<std::size_t>(rc);
  }
  return true;
}

bool SendControlResponse(int fd, std::uint64_t request_id, bool success, const std::string& text) {
  proto::Bytes payload;
  payload.push_back(success ? 0 : 1);
  payload.insert(payload.end(), text.begin(), text.end());
  return SendAll(fd, proto::EncodeControlPlaneMessage(proto::ControlPlaneMessage{0x0061, 0, request_id, payload}));
}

std::optional<proto::SbdbFrame> SendFrame(int fd, const proto::SbdbFrame& frame) {
  const auto encoded = proto::EncodeSbdbFrame(frame);
  if (!SendAll(fd, encoded)) return std::nullopt;
  proto::Bytes header(12);
  if (!RecvExact(fd, header.data(), header.size())) return std::nullopt;
  const auto payload_len = ReadU32(header, 8);
  proto::Bytes response = header;
  response.resize(12 + payload_len);
  if (payload_len != 0 && !RecvExact(fd, response.data() + 12, payload_len)) return std::nullopt;
  std::vector<proto::Diagnostic> diagnostics;
  return proto::DecodeSbdbFrame(response, &diagnostics);
}

std::optional<proto::SbdbFrame> RecvFrame(int fd) {
  proto::Bytes header(12);
  if (!RecvExact(fd, header.data(), header.size())) return std::nullopt;
  const auto payload_len = ReadU32(header, 8);
  proto::Bytes response = header;
  response.resize(12 + payload_len);
  if (payload_len != 0 && !RecvExact(fd, response.data() + 12, payload_len)) return std::nullopt;
  std::vector<proto::Diagnostic> diagnostics;
  return proto::DecodeSbdbFrame(response, &diagnostics);
}

std::string AuthErrorMessage(const std::optional<proto::SbdbFrame>& frame) {
  if (!frame || frame->payload.size() < 5) return {};
  std::size_t end = 5;
  while (end < frame->payload.size() && frame->payload[end] != 0) ++end;
  return std::string(frame->payload.begin() + 5, frame->payload.begin() + static_cast<std::ptrdiff_t>(end));
}

class TcpBackendStub {
 public:
  bool Start() { return StartOnPort(0); }

  bool StartOnPort(std::uint16_t requested_port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(requested_port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd_, 16) != 0) return false;
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this]() {
      while (!stopping_.load()) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, 50);
        if (rc <= 0 || (pfd.revents & POLLIN) == 0) continue;
        const int client = ::accept(fd_, nullptr, nullptr);
        if (client >= 0) {
          CaptureClientPrelude(client);
          ::close(client);
        }
      }
    });
    return true;
  }

  void Stop() {
    stopping_.store(true);
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
  }

  std::uint16_t port() const { return port_; }

  std::optional<std::string> WaitForPrelude(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!captured_preludes_.empty()) return captured_preludes_.front();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!captured_preludes_.empty()) return captured_preludes_.front();
    return std::nullopt;
  }

 private:
  void CaptureClientPrelude(int client) {
    pollfd pfd{};
    pfd.fd = client;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 500);
    if (rc <= 0 || (pfd.revents & POLLIN) == 0) return;
    char buffer[512]{};
    const auto got = ::recv(client, buffer, sizeof(buffer), 0);
    if (got <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    captured_preludes_.emplace_back(buffer, buffer + got);
  }

  int fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic_bool stopping_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<std::string> captured_preludes_;
};

class ListenerControlStub {
 public:
  bool Start(const std::filesystem::path& dir, std::uint32_t listener_id) {
    std::filesystem::create_directories(dir);
    path_ = dir / ("listener-" + std::to_string(listener_id) + ".sock");
    std::filesystem::remove(path_);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto path_text = path_.string();
    std::strncpy(addr.sun_path, path_text.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd_, 16) != 0) return false;
    thread_ = std::thread([this]() { Loop(); });
    return true;
  }

  void Stop() {
    stopping_.store(true);
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    std::filesystem::remove(path_);
  }

 private:
  void Loop() {
    while (!stopping_.load()) {
      pollfd pfd{};
      pfd.fd = fd_;
      pfd.events = POLLIN;
      const int rc = ::poll(&pfd, 1, 50);
      if (rc <= 0 || (pfd.revents & POLLIN) == 0) continue;
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) continue;
      Handle(client);
      ::close(client);
    }
  }

  void Handle(int client) {
    proto::Bytes header(28);
    if (!RecvExact(client, header.data(), header.size())) return;
    const auto payload_len = static_cast<std::uint64_t>(header[20]) |
                             (static_cast<std::uint64_t>(header[21]) << 8u) |
                             (static_cast<std::uint64_t>(header[22]) << 16u) |
                             (static_cast<std::uint64_t>(header[23]) << 24u) |
                             (static_cast<std::uint64_t>(header[24]) << 32u) |
                             (static_cast<std::uint64_t>(header[25]) << 40u) |
                             (static_cast<std::uint64_t>(header[26]) << 48u) |
                             (static_cast<std::uint64_t>(header[27]) << 56u);
    proto::Bytes encoded = header;
    encoded.resize(28 + payload_len);
    if (payload_len != 0 && !RecvExact(client, encoded.data() + 28, static_cast<std::size_t>(payload_len))) return;
    std::vector<proto::Diagnostic> diagnostics;
    const auto message = proto::DecodeControlPlaneMessage(encoded, &diagnostics);
    if (!message) return;
    const std::string command = CommandFromListenerManagementPayload(message->payload,
                                                                     message->request_id);
    if (command.rfind("LPREFACE_VALIDATE ", 0) == 0) {
      proto::Bytes ack_bytes;
      (void)proto::EncodeLprefaceAck(proto::LprefaceAck{true, 0, "ok"}, &ack_bytes);
      (void)SendControlResponse(client, message->request_id, true, "lpreface_ack:" + proto::Hex(ack_bytes));
    } else if (command == "STATUS") {
      (void)SendControlResponse(client, message->request_id, true, "listener_id=7;parser_pool_ready=true;draining=false");
    } else if (command == "DRAIN") {
      (void)SendControlResponse(client, message->request_id, true, "draining");
    } else if (command == "STOP FORCE") {
      (void)SendControlResponse(client, message->request_id, true, "stopped");
    } else if (command == "UNDRAIN") {
      (void)SendControlResponse(client, message->request_id, true, "undrained");
    } else if (command == "POOL RESTART") {
      (void)SendControlResponse(client, message->request_id, true, "parser_pool_restarted");
    } else if (command == "STOP GRACEFUL") {
      (void)SendControlResponse(client, message->request_id, true, "draining");
    } else if (command == "RELOAD") {
      (void)SendControlResponse(client, message->request_id, true, "reloaded");
    } else {
      (void)SendControlResponse(client, message->request_id, false, "unknown_command");
    }
  }

  int fd_ = -1;
  std::filesystem::path path_;
  std::atomic_bool stopping_{false};
  std::thread thread_;
};

int ConnectControlWhenReady(const std::filesystem::path& socket_path) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto path = socket_path.string();
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

void TestLiveManagerMcpCommandPath() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_runtime_integration";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto manager_config_path = root / "manager.conf";
  {
    std::ofstream out(manager_config_path, std::ios::trunc);
    out << "manager.release.profile=developer\n";
    out << "manager.log.level=debug\n";
  }

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.config_path = manager_config_path;
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "developer";
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const auto control_socket = config.control_dir / "sbmn_manager.control.sock";
  const int fd = ConnectControlWhenReady(control_socket);
  Check(fd >= 0, "live manager control socket must become reachable");
  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth.has_value(), "auth response required");
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "auth must succeed");

    auto bad_scope = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "support.bundle_generate",
                             "integration-bad-scope",
                             {{"scope", "manager\ninjected"}, {"redaction_profile", "default"}})});
    auto bad_scope_entries = bad_scope ? StatusEntries(*bad_scope, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(bad_scope_entries, "success") == "false",
          "support bundle must reject newline-injected scope before generation");
    Check(EntryValue(bad_scope_entries, "diagnostic") == "MANAGER.SUPPORT_BUNDLE_ARG_INVALID",
          "support bundle invalid scope diagnostic required");
    Check(EntryValue(bad_scope_entries, "field") == "scope",
          "support bundle invalid scope must identify field");
    Check(!std::filesystem::exists(config.control_dir / "support-bundles"),
          "support bundle invalid scope must not create bundle root");

    auto bad_redaction = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "support.bundle_generate",
                             "integration-bad-redaction",
                             {{"scope", "manager"}, {"redaction_profile", "../plaintext"}})});
    auto bad_redaction_entries = bad_redaction ? StatusEntries(*bad_redaction, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(bad_redaction_entries, "success") == "false",
          "support bundle must reject unsafe redaction profile before generation");
    Check(EntryValue(bad_redaction_entries, "diagnostic") == "MANAGER.SUPPORT_BUNDLE_ARG_INVALID",
          "support bundle invalid redaction profile diagnostic required");
    Check(EntryValue(bad_redaction_entries, "field") == "redaction_profile",
          "support bundle invalid redaction profile must identify field");
    Check(!std::filesystem::exists(config.control_dir / "support-bundles"),
          "support bundle invalid redaction profile must not create bundle root");

    auto duplicate_scope = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "support.bundle_generate",
                             "integration-duplicate-scope",
                             {{"scope", "manager"}, {"scope", "engine"}})});
    auto duplicate_scope_entries = duplicate_scope ? StatusEntries(*duplicate_scope, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(duplicate_scope_entries, "success") == "false",
          "support bundle must reject duplicate scope arguments");
    Check(EntryValue(duplicate_scope_entries, "diagnostic") == "MANAGER.COMMAND_ARGS_INVALID",
          "support bundle duplicate argument diagnostic required");
    Check(EntryValue(duplicate_scope_entries, "field") == "scope",
          "support bundle duplicate argument must identify field");
    Check(EntryValue(duplicate_scope_entries, "reason") == "duplicate_argument",
          "support bundle duplicate argument must disclose stable reason");
    Check(!std::filesystem::exists(config.control_dir / "support-bundles"),
          "support bundle duplicate arguments must not create bundle root");

    auto invalid_idempotency = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayload(
                             "support.bundle_generate",
                             "integration bad key")});
    auto invalid_idempotency_entries = invalid_idempotency ? StatusEntries(*invalid_idempotency, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(invalid_idempotency_entries, "success") == "false",
          "support bundle must reject invalid idempotency key text");
    Check(EntryValue(invalid_idempotency_entries, "diagnostic") == "MANAGER.IDEMPOTENCY_KEY_INVALID",
          "support bundle invalid idempotency diagnostic required");
    Check(EntryValue(invalid_idempotency_entries, "field") == "idempotency_key",
          "support bundle invalid idempotency must identify field");
    Check(EntryValue(invalid_idempotency_entries, "reason") == "invalid_character",
          "support bundle invalid idempotency must disclose stable reason");
    Check(!std::filesystem::exists(config.control_dir / "support-bundles"),
          "support bundle invalid idempotency must not create bundle root");

    auto support = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("support.bundle_generate", "integration-support")});
    Check(support.has_value(), "support bundle response required");
    auto support_entries = support ? StatusEntries(*support, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    const auto bundle_uuid = EntryValue(support_entries, "bundle_uuid");
    const auto bundle_ref = EntryValue(support_entries, "bundle_ref");
    const auto bundle_path = config.control_dir / "support-bundles" / bundle_uuid;
    Check(EntryValue(support_entries, "success") == "true", "support bundle must succeed");
    Check(bundle_ref == "[path-redacted]", "support bundle response must redact local path");
    Check(!bundle_uuid.empty() && std::filesystem::exists(bundle_path), "support bundle path must exist");
    Check(std::filesystem::exists(bundle_path / "manifest.txt"), "support bundle manifest must exist");

    auto validate_config = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("manager.validate_config", "")});
    Check(validate_config.has_value(), "manager.validate_config response required");
    auto validate_entries = validate_config ? StatusEntries(*validate_config, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(validate_entries, "success") == "true", "manager.validate_config must succeed");
    Check(EntryValue(validate_entries, "diagnostics") == "0", "manager.validate_config must report zero diagnostics");

    auto status_unknown_arg = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "manager.status",
                             "",
                             {{"unexpected", "true"}})});
    auto status_unknown_entries = status_unknown_arg ? StatusEntries(*status_unknown_arg, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(status_unknown_entries, "success") == "false",
          "manager.status must reject unknown arguments");
    Check(EntryValue(status_unknown_entries, "diagnostic") == "MANAGER.COMMAND_ARGS_INVALID",
          "manager.status unknown argument diagnostic required");
    Check(EntryValue(status_unknown_entries, "reason") == "unknown_argument",
          "manager.status unknown argument must disclose stable reason");

    auto bad_config_ref = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "manager.validate_config",
                             "",
                             {{"config_ref", (root / "not-current.conf").string()}})});
    auto bad_config_entries = bad_config_ref ? StatusEntries(*bad_config_ref, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(bad_config_entries, "success") == "false",
          "manager.validate_config must reject arbitrary config_ref paths");
    Check(EntryValue(bad_config_entries, "diagnostic") == "MANAGER.CONFIG_REF_FORBIDDEN",
          "manager.validate_config arbitrary config_ref diagnostic required");
    Check(EntryValue(bad_config_entries, "field") == "config_ref",
          "manager.validate_config arbitrary config_ref must identify field");

    auto relative_config_ref = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "manager.reload_config",
                             "integration-bad-config-ref",
                             {{"config_ref", "../manager.conf"}})});
    auto relative_config_entries = relative_config_ref ? StatusEntries(*relative_config_ref, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(relative_config_entries, "success") == "false",
          "manager.reload_config must reject relative config_ref paths");
    Check(EntryValue(relative_config_entries, "diagnostic") == "MANAGER.CONFIG_REF_FORBIDDEN",
          "manager.reload_config relative config_ref diagnostic required");
    Check(EntryValue(relative_config_entries, "reason") == "relative_path",
          "manager.reload_config relative config_ref must disclose stable reason");

    auto current_config_ref = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "manager.validate_config",
                             "",
                             {{"config_ref", manager_config_path.string()}})});
    auto current_config_entries = current_config_ref ? StatusEntries(*current_config_ref, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(current_config_entries, "success") == "true",
          "manager.validate_config must allow current configured config_ref");
    Check(EntryValue(current_config_entries, "config_ref") == "[path-redacted]",
          "manager.validate_config response must redact accepted config_ref");
    Check(EntryValue(current_config_entries, "config_ref") != manager_config_path.string(),
          "manager.validate_config response must not echo local config_ref path");

    auto reload_config = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("manager.reload_config", "integration-reload")});
    Check(reload_config.has_value(), "manager.reload_config response required");
    auto reload_entries = reload_config ? StatusEntries(*reload_config, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(reload_entries, "success") == "true", "manager.reload_config must succeed");
    Check(!EntryValue(reload_entries, "reload_generation_ms").empty(), "manager.reload_config must report a reload generation");

    auto third_party = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("thirdparty.status_export", "")});
    Check(third_party.has_value(), "third-party disabled response required");
    std::uint32_t mvs_len = 0;
    auto third_party_entries = third_party ? StatusEntries(*third_party, &mvs_len) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(third_party_entries, "success") == "false", "third-party export must be disabled by default");
    Check(EntryValue(third_party_entries, "diagnostic") == "MANAGER.COMMAND_UNSUPPORTED", "third-party disabled diagnostic required");
    Check(mvs_len != 0, "third-party disabled response must append MessageVectorSetV1");

    auto cluster_join = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("manager.join_cluster", "integration-cluster")});
    Check(cluster_join.has_value(), "cluster-only manager command response required");
    auto cluster_entries = cluster_join ? StatusEntries(*cluster_join, &mvs_len) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(cluster_entries, "success") == "false", "sbmn_manager must fail closed on cluster-only manager commands");
    Check(EntryValue(cluster_entries, "diagnostic") == "MANAGER.CLUSTER_ONLY_FORBIDDEN", "cluster-only manager diagnostic required");
    Check(mvs_len != 0, "cluster-only manager response must append MessageVectorSetV1");

    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    Check(shutdown.has_value(), "shutdown response required");
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted", "shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "live manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestLiveManagerDirectNativeBypassPath() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_direct_native_integration";
  std::filesystem::remove_all(root);

  TcpBackendStub backend;
  Check(backend.Start(), "direct-native backend stub must start");

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "native_only";
  config.native_bind = "127.0.0.1";
  config.native_port = backend.port();
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;
  config.proxy_backend_connect_timeout_ms = 500;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "direct-native manager control socket must become reachable");
  if (fd >= 0) {
    auto hello = SendFrame(fd, proto::SbdbFrame{0x65, 0, HelloPayload()});
    auto hello_entries = hello ? StatusEntries(*hello, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(hello_entries, "mcp_version") == "256", "MCP_HELLO must expose protocol version 0x0100");
    Check(EntryValue(hello_entries, "db_connect_extended_magic") == "MCP1",
          "MCP_HELLO must expose DB_CONNECT extended magic");
    Check(EntryValue(hello_entries, "connect_flag_base_capabilities") == "1",
          "MCP_HELLO must expose base capability flag constant");
    Check(EntryValue(hello_entries, "connect_flag_manager_dbbt") == "64",
          "MCP_HELLO must expose manager DBBT flag constant");
    Check(EntryValue(hello_entries, "direct_native_bypass") == "enabled",
          "MCP_HELLO must report direct-native bypass when no listener control socket is configured");
    Check(EntryValue(hello_entries, "ready") == "true", "MCP_HELLO must report ready when native backend is reachable");

    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0, "direct-native auth must succeed");

    proto::Bytes connect_payload;
    PutLpstr(&connect_payload, "");
    auto connect = SendFrame(fd, proto::SbdbFrame{0x69, 0, connect_payload});
    Check(connect.has_value(), "direct-native DB_CONNECT response required");
    Check(connect && connect->type == 0x02 && !connect->payload.empty() && connect->payload[0] == 0,
          "direct-native DB_CONNECT must succeed without DBBT keyring");
    const auto flags = connect ? ConnectServerFlags(*connect) : 0;
    Check(flags == 0x0001, "direct-native DB_CONNECT must advertise only base capability flags");
    Check((flags & 0x0040) == 0, "direct-native DB_CONNECT must not advertise manager DBBT flag");

    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted", "direct-native test shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  backend.Stop();
  Check(runtime_result.exit_code == 0, "direct-native manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestEnterpriseLiteralSecretRefused() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_literal_secret_refusal";
  std::filesystem::remove_all(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:enterprise-forbidden";

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "enterprise profile must reject literal MCP secrets at startup");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.RELEASE_PROFILE_FORBIDS_LITERAL_SECRET"),
        "enterprise literal MCP secret refusal diagnostic required");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "enterprise literal MCP secret refusal must not create owner token");
  std::filesystem::remove_all(root);
}

void TestEnterpriseLocalTokenStoreRefused() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_local_token_store_refusal";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.security_token_store_path = root / "temporary_auth_tokens.tsv";

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "enterprise profile must reject local TSV token stores at startup");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE"),
        "enterprise local token store refusal diagnostic required");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "enterprise local token store refusal must not create owner token");
  std::filesystem::remove_all(root);
}

void TestEnterpriseSecretFilePermissionsRefused() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_secret_file_permission_refusal";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto secret_path = root / "enterprise-mcp-secret.txt";
  {
    std::ofstream out(secret_path, std::ios::trunc);
    out << "enterprise-secret\n";
  }
  std::filesystem::permissions(secret_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read,
                               std::filesystem::perm_options::replace);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "file:" + secret_path.string();

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "enterprise profile must reject group-readable MCP secret files");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.SECRET_FILE_UNSAFE"),
        "enterprise unsafe MCP secret file diagnostic required");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "enterprise unsafe MCP secret file refusal must not create owner token");
  std::filesystem::remove_all(root);
}

void TestEnterpriseMcpSecretRightsRequired() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_mcp_secret_rights_required";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto secret_path = root / "enterprise-mcp-secret.txt";
  {
    std::ofstream out(secret_path, std::ios::trunc);
    out << "enterprise-secret\n";
  }
  std::filesystem::permissions(secret_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "file:" + secret_path.string();

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "enterprise MCP file secret without rights must fail startup");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.MCP_SECRET_RIGHTS_REQUIRED"),
        "enterprise MCP file secret without rights diagnostic required");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "enterprise MCP file secret rights refusal must not create owner token");
  std::filesystem::remove_all(root);
}

void TestEnterpriseMcpSecretWildcardRightsRefused() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_mcp_secret_wildcard_refusal";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto secret_path = root / "enterprise-mcp-secret.txt";
  {
    std::ofstream out(secret_path, std::ios::trunc);
    out << "enterprise-secret\n";
  }
  std::filesystem::permissions(secret_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "file:" + secret_path.string();
  config.mcp_secret_rights = "manager.*";

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "enterprise MCP file secret wildcard rights must fail startup");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.RELEASE_PROFILE_FORBIDS_WILDCARD_SECRET_RIGHT"),
        "enterprise MCP file secret wildcard diagnostic required");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "enterprise MCP file secret wildcard refusal must not create owner token");
  std::filesystem::remove_all(root);
}

void TestEnterpriseMcpSecretExplicitRightsAuthorization() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_mcp_secret_explicit_rights";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto secret_path = root / "enterprise-mcp-secret.txt";
  const std::string token = "enterprise-scoped-secret";
  {
    std::ofstream out(secret_path, std::ios::trunc);
    out << token << "\n";
  }
  std::filesystem::permissions(secret_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "file:" + secret_path.string();
  config.mcp_secret_rights = "manager.status,manager.lifecycle.shutdown";
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "enterprise scoped-secret manager control socket must become reachable");
  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload(token, "enterprise")});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "enterprise explicit-rights MCP secret auth must succeed");

    auto status = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("manager.status", "")});
    auto status_entries = status ? StatusEntries(*status, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(status_entries, "success") == "true",
          "enterprise explicit-rights MCP secret must allow manager.status");

    auto support = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("support.bundle_generate", "enterprise-support")});
    auto support_entries = support ? StatusEntries(*support, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(support_entries, "success") == "false",
          "enterprise explicit-rights MCP secret must not inherit support export authority");
    Check(EntryValue(support_entries, "diagnostic") == "MANAGER.COMMAND_UNAUTHORIZED",
          "enterprise explicit-rights support denial diagnostic required");
    Check(EntryValue(support_entries, "required_right") == "manager.support.export",
          "enterprise explicit-rights support denial must disclose required right");

    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted",
          "enterprise explicit-rights MCP secret must allow shutdown when named");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "enterprise explicit-rights manager runtime must exit cleanly");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.COMMAND_UNAUTHORIZED",
                           std::chrono::milliseconds(200)),
        "enterprise explicit-rights denial must be audited");
  std::filesystem::remove_all(root);
}

void TestEnterpriseDbbtKeyringPermissionsRefused() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_dbbt_keyring_permission_refusal";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto secret_path = root / "enterprise-mcp-secret.txt";
  const auto keyring_path = root / "dbbt.keyring";
  {
    std::ofstream out(secret_path, std::ios::trunc);
    out << "enterprise-secret\n";
  }
  std::filesystem::permissions(secret_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  {
    std::ofstream out(keyring_path, std::ios::trunc);
    out << "format=SBMN_DBBT_KEYRING_V1\n";
    out << "active_key_id=active\n";
    out << "active_key_hex=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
  }
  std::filesystem::permissions(keyring_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "file:" + secret_path.string();
  config.listener_control_socket_dir = root / "listener-control";
  config.dbbt_keyring_path = keyring_path;
  config.owner_database_uuid_set = true;
  for (std::size_t i = 0; i < config.owner_database_uuid.size(); ++i) {
    config.owner_database_uuid[i] = static_cast<std::uint8_t>(0xb0 + i);
  }

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "enterprise listener-control mode must reject world-readable DBBT keyrings");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.DBBT_KEYRING_FILE_UNSAFE"),
        "enterprise unsafe DBBT keyring diagnostic required");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "enterprise unsafe DBBT keyring refusal must not create owner token");
  std::filesystem::remove_all(root);
}

void TestEnterpriseDirectNativeBypassForbidden() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_enterprise_direct_native_refusal";
  std::filesystem::remove_all(root);

  const auto secret_path = root / "enterprise-mcp-secret.txt";
  const std::string token = "enterprise-management-secret";
  std::filesystem::create_directories(root);
  {
    std::ofstream out(secret_path, std::ios::trunc);
    out << token << "\n";
  }
  std::filesystem::permissions(secret_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "file:" + secret_path.string();
  config.mcp_secret_rights = "database.connect,manager.lifecycle.shutdown,manager.status";
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "enterprise direct-native refusal manager control socket must become reachable");
  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload(token, "enterprise")});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "enterprise file-secret MCP auth must succeed");

    auto hello = SendFrame(fd, proto::SbdbFrame{0x65, 0, HelloPayload()});
    auto hello_entries = hello ? StatusEntries(*hello, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(hello_entries, "release_profile") == "enterprise",
          "enterprise hello must report enterprise profile");
    Check(EntryValue(hello_entries, "direct_native_bypass") == "forbidden",
          "enterprise hello must report direct-native bypass forbidden");

    proto::Bytes connect_payload;
    PutLpstr(&connect_payload, "");
    auto connect = SendFrame(fd, proto::SbdbFrame{0x69, 0, connect_payload});
    Check(connect && connect->type == 0x02 && !connect->payload.empty() && connect->payload[0] == 1,
          "enterprise direct-native DB_CONNECT must fail");
    Check(connect && ConnectErrorText(*connect).find("Direct-native") != std::string::npos,
          "enterprise direct-native DB_CONNECT must explain release-profile refusal");

    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted",
          "enterprise direct-native refusal test shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "enterprise direct-native refusal manager runtime must exit cleanly");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.DIRECT_NATIVE_FORBIDDEN",
                           std::chrono::milliseconds(200)),
        "enterprise direct-native refusal must be audited");
  std::filesystem::remove_all(root);
}

void TestLiveManagerLprefaceAndListenerCommandPath() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_lpreface_integration";
  std::filesystem::remove_all(root);

  TcpBackendStub backend;
  ListenerControlStub listener;
  Check(backend.Start(), "backend stub must start");
  Check(listener.Start(root / "listener-control", 7), "listener control stub must start");

  const auto keyring_path = root / "dbbt.keyring";
  const auto security_token_store_path = root / "temporary_auth_tokens.tsv";
  const std::string security_token = "integration-security-token";
  std::filesystem::create_directories(root);
  {
    std::ofstream out(keyring_path, std::ios::trunc);
    out << "format=SBMN_DBBT_KEYRING_V1\n";
    out << "active_key_id=active\n";
    out << "active_key_hex=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
  }
  {
    std::ofstream out(security_token_store_path, std::ios::trunc);
    out << security_token
        << "\tintegration\t0\tactive\tdatabase.connect,manager.listener.read,manager.listener.control,manager.lifecycle.shutdown\n";
  }

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.release_profile = "developer";
  config.security_token_store_path = security_token_store_path;
  config.listener_id = 7;
  config.listener_control_socket_dir = root / "listener-control";
  config.dbbt_keyring_path = keyring_path;
  config.owner_database_uuid_set = true;
  for (std::size_t i = 0; i < config.owner_database_uuid.size(); ++i) {
    config.owner_database_uuid[i] = static_cast<std::uint8_t>(0xa0 + i);
  }
  config.native_bind = "127.0.0.1";
  config.native_port = backend.port();
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "LPREFACE manager control socket must become reachable");
  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload(security_token)});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0, "LPREFACE auth must succeed");

    auto rejected_profile = SendFrame(fd, proto::SbdbFrame{0x69, 0, ExtendedDbConnectPayload("", "firebirdsql", "SBsql")});
    Check(rejected_profile && rejected_profile->type == 0x02 && !rejected_profile->payload.empty() &&
              rejected_profile->payload[0] == 1,
          "LPREFACE DB_CONNECT must reject non-SBsql connection profiles");

    proto::Bytes connect_payload;
    PutLpstr(&connect_payload, "");
    auto connect = SendFrame(fd, proto::SbdbFrame{0x69, 0, connect_payload});
    Check(connect.has_value(), "LPREFACE DB_CONNECT response required");
    Check(connect && connect->type == 0x02 && !connect->payload.empty() && connect->payload[0] == 0,
          "LPREFACE DB_CONNECT must succeed through listener-control stub and backend stub");
    Check((connect ? ConnectServerFlags(*connect) : 0) == 0x0041,
          "LPREFACE DB_CONNECT must advertise base capability plus manager DBBT flag");
    const auto prelude = backend.WaitForPrelude(std::chrono::milliseconds(2000));
    Check(prelude.has_value(), "LPREFACE manager backend connection must send a handoff claim prelude");
    if (prelude) {
      const auto newline = prelude->find('\n');
      Check(newline != std::string::npos, "LPREFACE handoff claim prelude must be newline bounded");
      std::vector<proto::Diagnostic> claim_diagnostics;
      const auto claim = proto::DecodeLprefaceHandoffClaim(
          newline == std::string::npos ? *prelude : std::string_view(prelude->data(), newline),
          &claim_diagnostics);
      Check(claim.has_value(), "LPREFACE manager backend claim prelude must decode");
      Check(claim && claim->client_nonce.size() >= 16 && claim->client_nonce.size() <= 32,
            "LPREFACE manager backend claim must carry a production-length client nonce");
      Check(claim && claim->server_nonce.size() >= 16 && claim->server_nonce.size() <= 32,
            "LPREFACE manager backend claim must carry a production-length server nonce");
    }

    auto status = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("listener.status", "")});
    auto status_entries = status ? StatusEntries(*status, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(status_entries, "success") == "true", "listener.status must succeed through listener-control stub");
    Check(EntryValue(status_entries, "listener_response").find("parser_pool_ready=true") != std::string::npos,
          "listener.status must include listener response text");

    auto drain = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("listener.drain", "integration-drain")});
    auto drain_entries = drain ? StatusEntries(*drain, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(drain_entries, "success") == "true", "listener.drain must succeed through listener-control stub");

    auto invalid_stop = SendFrame(
        fd,
        proto::SbdbFrame{0x6b, 0,
                         ManagerCommandPayloadWithArgs(
                             "listener.stop",
                             "integration-invalid-stop",
                             {{"force", "maybe"}})});
    auto invalid_stop_entries = invalid_stop ? StatusEntries(*invalid_stop, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(invalid_stop_entries, "success") == "false",
          "listener.stop must reject invalid force argument values");
    Check(EntryValue(invalid_stop_entries, "diagnostic") == "MANAGER.COMMAND_ARGS_INVALID",
          "listener.stop invalid force diagnostic required");
    Check(EntryValue(invalid_stop_entries, "field") == "force",
          "listener.stop invalid force must identify field");
    Check(EntryValue(invalid_stop_entries, "reason") == "invalid_value",
          "listener.stop invalid force must disclose stable reason");

    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted", "LPREFACE test shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  listener.Stop();
  backend.Stop();
  Check(runtime_result.exit_code == 0, "LPREFACE manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestManagementCommandSpecificAuthorization() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_command_authz_integration";
  std::filesystem::remove_all(root);

  const auto security_token_store_path = root / "temporary_auth_tokens.tsv";
  const std::string limited_token = "limited-management-token";
  std::filesystem::create_directories(root);
  {
    std::ofstream out(security_token_store_path, std::ios::trunc);
    out << limited_token << "\tlimited\t0\tactive\tmanager.config.validate,manager.status\n";
  }

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:admin-management-secret";
  config.release_profile = "developer";
  config.security_token_store_path = security_token_store_path;
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int limited_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(limited_fd >= 0, "command-authz limited manager control socket must become reachable");
  if (limited_fd >= 0) {
    auto auth = SendFrame(limited_fd, proto::SbdbFrame{0x66, 0, AuthStartPayload(limited_token, "limited")});
    if (!auth || auth->type != 0x11 || auth->payload.empty() || auth->payload[0] != 0) {
      std::cerr << "limited management token auth error: " << AuthErrorMessage(auth) << '\n';
    }
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "limited management token auth must succeed");

    auto validate_config = SendFrame(limited_fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("manager.validate_config", "")});
    auto validate_entries = validate_config ? StatusEntries(*validate_config, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(validate_entries, "success") == "true",
          "limited management token must allow manager.validate_config");

    auto status = SendFrame(limited_fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("manager.status", "")});
    auto status_entries = status ? StatusEntries(*status, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(status_entries, "success") == "true",
          "limited management token must allow manager.status with explicit status right");
    Check(EntryValue(status_entries, "status_json").find("\"product\":\"sbmn_manager\"") != std::string::npos,
          "manager.status must return structured manager status json");

    auto support = SendFrame(limited_fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("support.bundle_generate", "limited-support")});
    auto support_entries = support ? StatusEntries(*support, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(support_entries, "success") == "false",
          "limited management token must deny support bundle export");
    Check(EntryValue(support_entries, "diagnostic") == "MANAGER.COMMAND_UNAUTHORIZED",
          "limited management token denial diagnostic required");
    Check(EntryValue(support_entries, "required_right") == "manager.support.export",
          "limited management token denial must disclose required right");
    ::close(limited_fd);
  }

  const int admin_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(admin_fd >= 0, "command-authz admin manager control socket must become reachable");
  if (admin_fd >= 0) {
    auto auth = SendFrame(admin_fd, proto::SbdbFrame{0x66, 0, AuthStartPayload("admin-management-secret", "admin")});
    if (!auth || auth->type != 0x11 || auth->payload.empty() || auth->payload[0] != 0) {
      std::cerr << "admin management secret auth error: " << AuthErrorMessage(auth) << '\n';
    }
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "admin management secret auth must succeed");
    auto invalid_shutdown = SendFrame(admin_fd, proto::SbdbFrame{0x60, 0, ShutdownPayload("shutdown key with spaces")});
    Check(invalid_shutdown && invalid_shutdown->type == 0x11 &&
              !invalid_shutdown->payload.empty() && invalid_shutdown->payload[0] != 0,
          "explicit shutdown idempotency key with spaces must fail closed");
    Check(AuthErrorMessage(invalid_shutdown).find("MANAGER.IDEMPOTENCY_KEY_INVALID") != std::string::npos,
          "explicit shutdown invalid idempotency diagnostic required");
    ::close(admin_fd);
  }

  const int shutdown_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(shutdown_fd >= 0, "command-authz shutdown manager control socket must become reachable");
  if (shutdown_fd >= 0) {
    auto auth = SendFrame(shutdown_fd, proto::SbdbFrame{0x66, 0, AuthStartPayload("admin-management-secret", "admin")});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "shutdown management secret auth must succeed");
    auto shutdown = SendFrame(shutdown_fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted",
          "admin management secret must retain shutdown authority");
    ::close(shutdown_fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "command-authz manager runtime must exit cleanly");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.COMMAND_UNAUTHORIZED",
                           std::chrono::milliseconds(200)),
        "command-authz denial must be audited before failure response");
  std::filesystem::remove_all(root);
}

void TestManagementMaxClientsAdmissionLimit() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_management_max_clients_integration";
  std::filesystem::remove_all(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "developer";
  config.management_max_clients = 1;
  config.management_idle_timeout_ms = 5000;
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int held_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(held_fd >= 0, "management max-client held socket must connect");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "\"sb_manager_management_clients_active\",\"value\":1",
                           std::chrono::milliseconds(1000)),
        "management max-client test must observe the held active management client");

  const int rejected_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(rejected_fd >= 0, "management max-client rejected socket must connect before refusal");
  if (rejected_fd >= 0) {
    const auto rejected = RecvFrame(rejected_fd);
    const std::string payload = rejected ? std::string(rejected->payload.begin(), rejected->payload.end()) : std::string{};
    Check(rejected && rejected->type == 0xff,
          "management max-client overflow must return protocol error frame");
    Check(payload.find("MANAGER.CONTROL_MAX_CLIENTS") != std::string::npos,
          "management max-client overflow diagnostic required");
    ::close(rejected_fd);
  }
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.CONTROL_MAX_CLIENTS",
                           std::chrono::milliseconds(1000)),
        "management max-client overflow must be audited");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "\"sb_manager_management_clients_rejected_total\",\"value\":1",
                           std::chrono::milliseconds(1000)),
        "management max-client overflow must increment rejection metric");

  if (held_fd >= 0) {
    auto auth = SendFrame(held_fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "management max-client held socket auth must succeed");
    auto shutdown = SendFrame(held_fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted",
          "management max-client held socket shutdown must be accepted");
    ::close(held_fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "management max-client manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestAuditMetricsFailureEvidence() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_audit_metrics_failure_integration";
  std::filesystem::remove_all(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "developer";
  config.third_party_management_enabled = true;
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "audit/metrics failure manager control socket must become reachable");
  auto status_json = [&]() {
    auto status = SendFrame(fd, proto::SbdbFrame{0x6b, 0, ManagerCommandPayload("thirdparty.status_export", "")});
    auto entries = status ? StatusEntries(*status, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    return EntryValue(entries, "status_json");
  };

  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "audit/metrics failure initial auth must succeed");
    auto initial_status = status_json();
    Check(initial_status.find("\"audit_sequence\":") != std::string::npos,
          "audit/metrics status must expose audit sequence");
    Check(initial_status.find("\"metrics_publish_failures\":") != std::string::npos,
          "audit/metrics status must expose metrics failure counter");

    const auto metrics_path = config.control_dir / "sbmn_manager.metrics.json";
    std::filesystem::remove(metrics_path);
    std::filesystem::create_directory(metrics_path);
    const int metrics_probe_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
    Check(metrics_probe_fd >= 0, "metrics failure probe socket must connect");
    if (metrics_probe_fd >= 0) ::close(metrics_probe_fd);

    bool metrics_failure_seen = false;
    for (int i = 0; i < 100; ++i) {
      const auto after_metrics_failure = status_json();
      if (JsonU64Value(after_metrics_failure, "metrics_publish_failures") > 0) {
        metrics_failure_seen = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(metrics_failure_seen, "metrics publish failure must be visible in status evidence");
    std::filesystem::remove_all(metrics_path);

    const auto audit_path = config.control_dir / "sbmn_manager.audit.jsonl";
    std::filesystem::remove(audit_path);
    std::filesystem::create_directory(audit_path);
    const int audit_probe_fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
    Check(audit_probe_fd >= 0, "audit failure probe socket must connect");
    if (audit_probe_fd >= 0) {
      auto audit_failed_auth = SendFrame(audit_probe_fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
      Check(audit_failed_auth && audit_failed_auth->type == 0x11 &&
                !audit_failed_auth->payload.empty() && audit_failed_auth->payload[0] != 0,
            "audit write failure must fail closed on successful auth attempt");
      Check(AuthErrorMessage(audit_failed_auth).find("MANAGER.AUDIT_WRITE_FAILED") != std::string::npos,
            "audit write failure auth response must surface stable diagnostic");
      ::close(audit_probe_fd);
    }
    std::filesystem::remove_all(audit_path);

    bool audit_failure_seen = false;
    for (int i = 0; i < 100; ++i) {
      const auto after_audit_failure = status_json();
      if (JsonU64Value(after_audit_failure, "audit_write_failures") > 0) {
        audit_failure_seen = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(audit_failure_seen, "audit write failure must be visible in status evidence");

    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted",
          "audit/metrics failure test shutdown must be accepted after evidence restoration");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "audit/metrics failure manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestLiveManagerHeartbeatRestartRefusalPath() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_heartbeat_refusal_integration";
  std::filesystem::remove_all(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "developer";
  config.native_bind = "127.0.0.1";
  config.native_port = 1;
  config.heartbeat_interval_ms = 20;
  config.heartbeat_timeout_ms = 10;
  config.missed_heartbeat_threshold = 1;
  config.restart_enabled = false;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "heartbeat-refusal manager control socket must become reachable");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "sb_manager_server_restart_refused_total",
                           std::chrono::milliseconds(2000)),
        "heartbeat-refusal metrics snapshot must be published");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "unreachable",
                           std::chrono::milliseconds(2000)),
        "heartbeat failure must publish unreachable health state");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.SERVER_RESTART_DISABLED",
                           std::chrono::milliseconds(2000)),
        "restart-disabled heartbeat failure must audit restart refusal");

  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "heartbeat-refusal auth must succeed");
    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted", "heartbeat-refusal shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "heartbeat-refusal manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestLiveManagerRestartQuarantinePath() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_restart_quarantine_integration";
  std::filesystem::remove_all(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "developer";
  config.native_bind = "127.0.0.1";
  config.native_port = 1;
  config.heartbeat_interval_ms = 20;
  config.heartbeat_timeout_ms = 10;
  config.missed_heartbeat_threshold = 1;
  config.restart_enabled = true;
  config.restart_max_attempts = 1;
  config.restart_window_ms = 10000;
  config.restart_initial_backoff_ms = 10;
  config.restart_max_backoff_ms = 10;
  config.restart_executable = "/bin/false";

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "restart-quarantine manager control socket must become reachable");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.SERVER_RESTART_ATTEMPT",
                           std::chrono::milliseconds(3000)),
        "restart-enabled heartbeat failure must audit restart attempt");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.audit.jsonl",
                           "MANAGER.SERVER_RESTART_QUARANTINED",
                           std::chrono::milliseconds(3000)),
        "failing restart descriptor must eventually audit quarantine");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "quarantined",
                           std::chrono::milliseconds(3000)),
        "restart quarantine must publish quarantined metrics state");

  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "restart-quarantine auth must succeed");
    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted", "restart-quarantine shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  Check(runtime_result.exit_code == 0, "restart-quarantine manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestOwnerTokenAmbiguityRefusal() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_owner_ambiguity_integration";
  std::filesystem::remove_all(root);
  const auto control = root / "control";
  std::filesystem::create_directories(control);
  {
    std::ofstream owner(control / "sbmn_manager.owner", std::ios::trunc);
    owner << "format=SBMN_MANAGER_OWNER_V1\n";
    owner << "pid=99999999\n";
  }
  {
    std::ofstream state(control / "sbmn_manager.lifecycle.state", std::ios::trunc);
    state << "state=ready\n";
  }

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.validate_config = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = control;
  config.log_path = "stderr";
  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "ambiguous owner token must fail startup");
  Check(FileContainsWithin(control / "sbmn_manager.lifecycle.state",
                           "state=ready",
                           std::chrono::milliseconds(200)),
        "ambiguous owner refusal must not overwrite existing lifecycle state");
  bool has_ambiguous = false;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == "MANAGER.OWNER_TOKEN_AMBIGUOUS") has_ambiguous = true;
  }
  Check(has_ambiguous, "ambiguous owner token diagnostic required");
  std::filesystem::remove_all(root);
}

void TestLiveManagerHeartbeatRecoveryPath() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_heartbeat_recovery_integration";
  std::filesystem::remove_all(root);
  const auto port = ReserveLoopbackPort();
  Check(port != 0, "heartbeat recovery test must reserve a loopback port");

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";
  config.mcp_secret_ref = "literal:integration-secret";
  config.release_profile = "developer";
  config.native_bind = "127.0.0.1";
  config.native_port = port;
  config.heartbeat_interval_ms = 20;
  config.heartbeat_timeout_ms = 10;
  config.missed_heartbeat_threshold = 1;
  config.restart_enabled = false;

  node::RuntimeResult runtime_result;
  std::thread manager_thread([&]() { runtime_result = node::RunManager(config); });

  const int fd = ConnectControlWhenReady(config.control_dir / "sbmn_manager.control.sock");
  Check(fd >= 0, "heartbeat-recovery manager control socket must become reachable");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "unreachable",
                           std::chrono::milliseconds(2000)),
        "heartbeat recovery test must observe initial unreachable state");

  TcpBackendStub backend;
  Check(backend.StartOnPort(port), "heartbeat recovery backend stub must start on reserved port");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.metrics.json",
                           "healthy",
                           std::chrono::milliseconds(3000)),
        "heartbeat recovery must publish healthy state after backend appears");

  if (fd >= 0) {
    auto auth = SendFrame(fd, proto::SbdbFrame{0x66, 0, AuthStartPayload()});
    Check(auth && auth->type == 0x11 && !auth->payload.empty() && auth->payload[0] == 0,
          "heartbeat-recovery auth must succeed");
    auto shutdown = SendFrame(fd, proto::SbdbFrame{0x60, 0, {}});
    auto shutdown_entries = shutdown ? StatusEntries(*shutdown, nullptr) : std::vector<std::pair<std::string, std::string>>{};
    Check(EntryValue(shutdown_entries, "shutdown") == "accepted", "heartbeat-recovery shutdown must be accepted");
    ::close(fd);
  }

  if (manager_thread.joinable()) manager_thread.join();
  backend.Stop();
  Check(runtime_result.exit_code == 0, "heartbeat-recovery manager runtime must exit cleanly");
  std::filesystem::remove_all(root);
}

void TestSafeStaleOwnerCleanup() {
  const std::vector<std::string> safe_states = {"stopped", "startup_failed", "failed_terminal"};
  for (const auto& safe_state : safe_states) {
    const auto root = std::filesystem::temp_directory_path() / ("sbmn_manager_safe_stale_owner_" + safe_state);
    std::filesystem::remove_all(root);
    const auto control = root / "control";
    std::filesystem::create_directories(control);
    {
      std::ofstream owner(control / "sbmn_manager.owner", std::ios::trunc);
      owner << "format=SBMN_MANAGER_OWNER_V1\n";
      owner << "pid=99999999\n";
    }
    {
      std::ofstream state(control / "sbmn_manager.lifecycle.state", std::ios::trunc);
      state << "state=" << safe_state << "\n";
    }

    node::ManagerConfig config;
    config.proxy_enabled = false;
    config.validate_config = true;
    config.runtime_dir = root / "runtime";
    config.control_dir = control;
    config.log_path = "stderr";
    const auto result = node::RunManager(config);
    Check(result.exit_code == 0, "safe stale owner token must be cleaned up for final state " + safe_state);
    Check(!std::filesystem::exists(control / "sbmn_manager.owner"), "safe stale owner token must be removed after cleanup for " + safe_state);
    Check(!std::filesystem::exists(control / "sbmn_manager.pid"), "safe stale pid file must be removed after cleanup for " + safe_state);
    std::filesystem::remove_all(root);
  }
}

void TestDuplicateOwnerBusyRefusal() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_duplicate_owner_integration";
  std::filesystem::remove_all(root);
  const auto control = root / "control";
  std::filesystem::create_directories(control);
  {
    std::ofstream owner(control / "sbmn_manager.owner", std::ios::trunc);
    owner << "format=SBMN_MANAGER_OWNER_V1\n";
    owner << "pid=" << ::getpid() << "\n";
  }
  {
    std::ofstream state(control / "sbmn_manager.lifecycle.state", std::ios::trunc);
    state << "state=stopped\n";
  }

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.validate_config = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = control;
  config.log_path = "stderr";
  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "live owner token must fail startup as busy");
  Check(FileContainsWithin(control / "sbmn_manager.lifecycle.state",
                           "state=stopped",
                           std::chrono::milliseconds(200)),
        "busy owner refusal must not overwrite existing lifecycle state");
  bool has_busy = false;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == "MANAGER.OWNER_TOKEN_BUSY") has_busy = true;
  }
  Check(has_busy, "busy owner token diagnostic required");
  std::filesystem::remove_all(root);
}

void TestInterruptedStartupControlDirFailure() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_interrupted_startup_integration";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto control_as_file = root / "control-is-file";
  {
    std::ofstream out(control_as_file, std::ios::trunc);
    out << "not a directory\n";
  }

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.validate_config = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = control_as_file;
  config.log_path = "stderr";
  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "control-dir file must fail startup preparation");
  bool has_control_dir_invalid = false;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == "MANAGER.CONTROL_DIR_INVALID") has_control_dir_invalid = true;
  }
  Check(has_control_dir_invalid, "control-dir failure diagnostic required");
  Check(!std::filesystem::exists(control_as_file / "sbmn_manager.owner"),
        "interrupted startup must not create owner token under invalid control dir");
  std::filesystem::remove_all(root);
}

void TestControlSocketPathTooLongFailsClosed() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_control_socket_path_limit_integration";
  std::filesystem::remove_all(root);
  sockaddr_un addr{};
  const auto unix_socket_limit = sizeof(addr.sun_path);
  auto control = root / "control";
  while ((control / "sbmn_manager.control.sock").string().size() < unix_socket_limit) {
    control /= "socket-limit-segment";
  }
  std::filesystem::create_directories(control);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.foreground = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = control;
  config.log_path = "stderr";
  config.heartbeat_interval_ms = 60000;
  config.heartbeat_timeout_ms = 100;

  const auto result = node::RunManager(config);
  Check(result.exit_code == 2, "overlong manager control socket path must fail startup");
  Check(HasDiagnostic(result.diagnostics, "MANAGER.CONTROL_SOCKET_PATH_TOO_LONG"),
        "overlong manager control socket diagnostic required");
  Check(!std::filesystem::exists(control / "sbmn_manager.control.sock"),
        "overlong manager control socket must not be truncated or created");
  Check(!std::filesystem::exists(control / "sbmn_manager.owner"),
        "overlong manager control socket startup failure must clean owner token");
  Check(!std::filesystem::exists(control / "sbmn_manager.pid"),
        "overlong manager control socket startup failure must clean pid file");
  Check(FileContainsWithin(control / "sbmn_manager.lifecycle.state",
                           "state=startup_failed",
                           std::chrono::milliseconds(200)),
        "overlong manager control socket startup failure must publish startup_failed lifecycle state");
  std::filesystem::remove_all(root);
}

void TestServiceValidateConfigHasNoDaemonSideEffects() {
  const auto root = std::filesystem::temp_directory_path() / "sbmn_manager_service_validate_config_integration";
  std::filesystem::remove_all(root);

  node::ManagerConfig config;
  config.proxy_enabled = false;
  config.service = true;
  config.validate_config = true;
  config.runtime_dir = root / "runtime";
  config.control_dir = root / "control";
  config.log_path = "stderr";

  const auto result = node::RunManager(config);
  Check(result.exit_code == 0, "service validate-config must not enter daemon handoff");
  Check(!HasDiagnostic(result.diagnostics, "MANAGER.SERVICE_MODE_UNSUPPORTED"),
        "service validate-config must not emit service-mode unsupported");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.owner"),
        "service validate-config must remove transient owner token");
  Check(!std::filesystem::exists(config.control_dir / "sbmn_manager.pid"),
        "service validate-config must remove transient pid file");
  Check(FileContainsWithin(config.control_dir / "sbmn_manager.lifecycle.state",
                           "state=stopped",
                           std::chrono::milliseconds(200)),
        "service validate-config must publish stopped lifecycle state");
  std::filesystem::remove_all(root);
}
#endif

}  // namespace

int main() {
#ifndef _WIN32
  TestLiveManagerMcpCommandPath();
  TestLiveManagerDirectNativeBypassPath();
  TestEnterpriseLiteralSecretRefused();
  TestEnterpriseLocalTokenStoreRefused();
  TestEnterpriseSecretFilePermissionsRefused();
  TestEnterpriseMcpSecretRightsRequired();
  TestEnterpriseMcpSecretWildcardRightsRefused();
  TestEnterpriseMcpSecretExplicitRightsAuthorization();
  TestEnterpriseDbbtKeyringPermissionsRefused();
  TestEnterpriseDirectNativeBypassForbidden();
  TestLiveManagerLprefaceAndListenerCommandPath();
  TestManagementCommandSpecificAuthorization();
  TestManagementMaxClientsAdmissionLimit();
  TestAuditMetricsFailureEvidence();
  TestLiveManagerHeartbeatRestartRefusalPath();
  TestLiveManagerRestartQuarantinePath();
  TestOwnerTokenAmbiguityRefusal();
  TestLiveManagerHeartbeatRecoveryPath();
  TestSafeStaleOwnerCleanup();
  TestDuplicateOwnerBusyRefusal();
  TestInterruptedStartupControlDirFailure();
  TestControlSocketPathTooLongFailsClosed();
  TestServiceValidateConfigHasNoDaemonSideEffects();
#endif
  if (failures != 0) {
    std::cerr << failures << " manager runtime integration failure(s)\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
