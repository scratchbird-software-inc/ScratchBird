// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_LISTENER_CONTROL_MODULE

#include "manager_listener_control.hpp"

#include "control_plane.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <afunix.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace scratchbird::manager::node {
namespace {

constexpr std::uint16_t kManagementCommand = 0x0060;
constexpr std::uint16_t kManagementResponse = 0x0061;

#ifdef _WIN32
using ManagementSocketHandle = std::intptr_t;
#else
using ManagementSocketHandle = int;
#endif

#ifdef _WIN32
bool TruthyEnvironmentFlag(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return false;
  return std::string_view(value) == "1" ||
         std::string_view(value) == "true" ||
         std::string_view(value) == "TRUE" ||
         std::string_view(value) == "yes" ||
         std::string_view(value) == "YES";
}

proto::Bytes WindowsManagementHmacKey(std::string* source_name) {
  if (const char* key_hex = std::getenv("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX");
      key_hex != nullptr && *key_hex != '\0') {
    auto key = proto::FromHex(key_hex);
    if (!key.empty()) {
      if (source_name != nullptr) *source_name = "SCRATCHBIRD_LISTENER_DBBT_KEY_HEX";
      return key;
    }
  }
  if (const char* dev_key = std::getenv("SCRATCHBIRD_DEV_DBBT_KEY");
      dev_key != nullptr && *dev_key != '\0') {
    if (source_name != nullptr) *source_name = "SCRATCHBIRD_DEV_DBBT_KEY";
    return proto::Bytes(dev_key, dev_key + std::strlen(dev_key));
  }
  if (TruthyEnvironmentFlag("SCRATCHBIRD_ALLOW_TEST_DBBT_BUILTIN")) {
    static constexpr std::string_view kTestKey = "scratchbird-listener-test-dbbt-key-v1";
    if (source_name != nullptr) *source_name = "test_builtin";
    return proto::Bytes(kTestKey.begin(), kTestKey.end());
  }
  return {};
}
#endif

#ifdef _WIN32
bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
#endif

void CloseManagementSocket(ManagementSocketHandle fd) {
  if (fd < 0) return;
#ifdef _WIN32
  ::closesocket(static_cast<SOCKET>(fd));
#else
  ::close(fd);
#endif
}

std::string NormalizeManagementSocketPath(const std::string& socket_path) {
#ifdef _WIN32
  return std::filesystem::absolute(socket_path).string();
#else
  return socket_path;
#endif
}

bool ConnectManagementSocket(const std::string& socket_path,
                             ManagementSocketHandle* out_fd,
                             std::vector<proto::Diagnostic>* diagnostics) {
  *out_fd = -1;
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_SOCKET_STACK_UNAVAILABLE",
                                                 "Winsock initialization failed for listener management client."));
    return false;
  }
  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
#else
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
#endif
    diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_UNAVAILABLE",
                                                 "Could not create listener management socket client."));
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto path = NormalizeManagementSocketPath(socket_path);
  if (path.size() >= sizeof(addr.sun_path)) {
    CloseManagementSocket(static_cast<ManagementSocketHandle>(fd));
    diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_SOCKET_PATH_TOO_LONG",
                                                 "Listener management socket path is too long."));
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseManagementSocket(static_cast<ManagementSocketHandle>(fd));
    diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_UNAVAILABLE",
                                                 "Could not connect to listener management socket."));
    return false;
  }
  *out_fd = static_cast<ManagementSocketHandle>(fd);
  return true;
}

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& key) {
  for (const auto& field : fields) {
    if (field.first == key) return field.second;
  }
  return {};
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

} // namespace

std::string ListenerControlSocketPath(const ManagerConfig& config) {
  const auto exact = config.listener_control_socket_dir /
                     ("listener-" + std::to_string(config.listener_id) + ".sock");
  std::error_code ec;
  if (std::filesystem::exists(exact, ec)) return exact.string();

  std::filesystem::path discovered;
  std::size_t matches = 0;
  for (const auto& entry : std::filesystem::directory_iterator(config.listener_control_socket_dir, ec)) {
    if (ec) break;
    const auto name = entry.path().filename().string();
    if (!name.ends_with(".management.sock")) continue;
    discovered = entry.path();
    ++matches;
    if (matches > 1) break;
  }
  if (matches == 1) return discovered.string();
  return exact.string();
}

bool BuildListenerManagementPayload(const std::string& command,
                                    std::uint64_t request_id,
                                    proto::Bytes* payload,
                                    std::vector<proto::Diagnostic>* diagnostics);

ListenerControlMapping MapListenerControlOperation(
    const std::string& operation,
    const std::vector<std::pair<std::string, std::string>>& args) {
  ListenerControlMapping mapping;
  mapping.mutating = operation != "listener.list" && operation != "listener.status";

  if (operation == "listener.list" || operation == "listener.status") {
    mapping.supported = true;
    mapping.listener_command = "STATUS";
  } else if (operation == "listener.start") {
    mapping.supported = true;
    mapping.listener_command = "POOL RESTART";
  } else if (operation == "listener.stop") {
    mapping.supported = true;
    mapping.listener_command = FieldValue(args, "force") == "true" ? "STOP FORCE" : "STOP GRACEFUL";
  } else if (operation == "listener.restart") {
    mapping.supported = true;
    mapping.listener_command = "POOL RESTART";
  } else if (operation == "listener.drain") {
    mapping.supported = true;
    mapping.listener_command = "DRAIN";
  } else if (operation == "listener.undrain") {
    mapping.supported = true;
    mapping.listener_command = "UNDRAIN";
  } else if (operation == "listener.reload") {
    mapping.supported = true;
    mapping.listener_command = "RELOAD";
  } else {
    mapping.supported = false;
    mapping.diagnostic_code = "MANAGER.COMMAND_UNSUPPORTED";
  }
  return mapping;
}

proto::Bytes EncodeListenerManagementCommand(const std::string& command,
                                             std::uint64_t request_id) {
  proto::ControlPlaneMessage message;
  message.message_type = kManagementCommand;
  message.flags = 0;
  message.request_id = request_id;
  if (!BuildListenerManagementPayload(command, request_id, &message.payload, nullptr)) return {};
  return proto::EncodeControlPlaneMessage(message);
}

std::string DecodeListenerManagementResponseText(const proto::Bytes& encoded,
                                                 std::vector<proto::Diagnostic>* diagnostics) {
  auto message = proto::DecodeControlPlaneMessage(encoded, diagnostics);
  if (!message) return {};
  if (message->message_type != kManagementResponse) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_RESPONSE_INVALID",
                                                  "Listener returned a non-management response frame."));
    }
    return {};
  }
  if (message->payload.empty()) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_RESPONSE_INVALID",
                                                  "Listener returned an empty management response."));
    }
    return {};
  }
  const auto status = message->payload[0];
  std::string text(message->payload.begin() + 1, message->payload.end());
  if (const auto ack_hex = ExtractJsonStringField(text, "ack_hex"); !ack_hex.empty()) {
    text = "lpreface_ack:" + ack_hex;
  }
  if (status != 0 && diagnostics != nullptr) {
    diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_COMMAND_REFUSED",
                                                text.empty() ? "Listener management command was refused." : text));
  }
  return text;
}

bool BuildListenerManagementPayload(const std::string& command,
                                    std::uint64_t request_id,
                                    proto::Bytes* payload,
                                    std::vector<proto::Diagnostic>* diagnostics) {
  if (payload == nullptr) return false;
  payload->clear();
  const auto now_ms = proto::CurrentEpochMilliseconds();
  auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
      command,
      request_id,
      now_ms,
      now_ms + 30000,
      "manager",
      "listener_manager",
#ifdef _WIN32
      scratchbird::listener::kListenerManagementAuthHmacSha256
#else
      scratchbird::listener::kListenerManagementAuthPeerOwner
#endif
      );
  if (!envelope) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_COMMAND_ENCODE_FAILED",
                                                  "Could not encode listener management command."));
    }
    return false;
  }
#ifdef _WIN32
  std::string key_source;
  auto key = WindowsManagementHmacKey(&key_source);
  if (key.empty()) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(proto::MakeDiagnostic(
          "MANAGER.LISTENER_MANAGEMENT_HMAC_KEY_MISSING",
          "Windows x64 listener management requires DBBT HMAC key material for privileged commands."));
    }
    return false;
  }
  scratchbird::listener::SignListenerManagementEnvelopeHmacSha256(&*envelope, key);
#endif
  *payload = scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
  if (payload->empty()) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(proto::MakeDiagnostic("MANAGER.LISTENER_COMMAND_ENCODE_FAILED",
                                                  "Could not encode listener management command."));
    }
    return false;
  }
  return true;
}

ListenerManagementCallResult SendListenerManagementCommand(const std::string& socket_path,
                                                           const std::string& command,
                                                           std::uint64_t request_id,
                                                           std::uint32_t timeout_ms) {
  ListenerManagementCallResult result;
  ManagementSocketHandle fd = -1;
  if (!ConnectManagementSocket(socket_path, &fd, &result.diagnostics)) {
    return result;
  }

  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = request_id;
  if (!BuildListenerManagementPayload(command, request_id, &frame.payload, &result.diagnostics)) {
    CloseManagementSocket(fd);
    return result;
  }
  if (!scratchbird::listener::SendControlFrame(fd, frame)) {
    CloseManagementSocket(fd);
    result.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.LISTENER_COMMAND_SEND_FAILED",
                                                       "Could not send listener management command."));
    return result;
  }

  scratchbird::listener::ListenerControlDecodeResult decoded;
  ManagementSocketHandle received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, timeout_ms)) {
    CloseManagementSocket(received_fd);
    CloseManagementSocket(fd);
    result.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.LISTENER_RESPONSE_TIMEOUT",
                                                       "Timed out waiting for listener management response."));
    return result;
  }
  CloseManagementSocket(received_fd);
  CloseManagementSocket(fd);
  if (decoded.frame.opcode != scratchbird::listener::ListenerControlOpcode::kManagementResponse) {
    result.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.LISTENER_RESPONSE_INVALID",
                                                       "Listener returned a non-management response frame."));
    return result;
  }
  if (decoded.frame.payload.empty()) {
    result.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.LISTENER_RESPONSE_INVALID",
                                                       "Listener returned an empty management response."));
    return result;
  }
  const auto status = decoded.frame.payload[0];
  result.response_text.assign(decoded.frame.payload.begin() + 1, decoded.frame.payload.end());
  if (status != 0) {
    result.diagnostics.push_back(proto::MakeDiagnostic("MANAGER.LISTENER_COMMAND_REFUSED",
                                                       result.response_text.empty()
                                                           ? "Listener management command was refused."
                                                           : result.response_text));
  }
  result.ok = result.diagnostics.empty();
  return result;
}

} // namespace scratchbird::manager::node
