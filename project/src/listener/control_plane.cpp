// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace scratchbird::listener {
namespace {

constexpr std::size_t kHelloFixedSize = 38;
constexpr std::size_t kHelloTextMax = 1024;
constexpr std::size_t kHelloAckMinSize = 11;
constexpr std::size_t kHandoffSocketSize = 193;
constexpr std::size_t kHandoffAckMinSize = 9;
constexpr std::size_t kWindowsSocketHandoffHeaderSize = 12;
constexpr std::size_t kWindowsSocketProtocolInfoMax = 4096;
constexpr std::size_t kHealthReportSize = 15;
constexpr std::size_t kSessionBindingMinSize = 138;
constexpr std::size_t kMaxSessionBindingGroups = 256;
constexpr std::size_t kTakeoverRequestMinSize = 4;
constexpr std::size_t kTakeoverDecisionMinSize = 3;
constexpr std::size_t kSbctHeaderSize = 28;
constexpr std::size_t kSbctMaxPayloadSize = 64u * 1024u;
constexpr std::size_t kManagementEnvelopeMinSize = 56;
constexpr std::size_t kManagementTextMax = 16384;
constexpr std::size_t kManagementFieldMax = 1024;
constexpr std::size_t kManagementNonceMax = 64;
constexpr std::size_t kManagementAuthenticatorMax = 128;
constexpr std::size_t kManagementArgumentMax = 32;
constexpr std::uint8_t kManagementEnvelopeMagic[4] = {'S', 'B', 'M', 'E'};

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    out |= static_cast<std::uint32_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

std::array<std::uint8_t, 16> ReadUuid(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = data[off + i];
  }
  return uuid;
}

void PutFixedString(std::vector<std::uint8_t>* out, std::string value, std::size_t width) {
  if (value.size() > width) {
    value.resize(width);
  }
  const auto written = value.size();
  out->insert(out->end(), value.begin(), value.end());
  for (std::size_t i = written; i < width; ++i) {
    out->push_back(0);
  }
}

std::string ReadFixedString(const std::vector<std::uint8_t>& data, std::size_t off, std::size_t width) {
  std::size_t len = 0;
  while (len < width && data[off + len] != 0) {
    ++len;
  }
  return std::string(reinterpret_cast<const char*>(data.data() + off), len);
}

void AddDecodeError(proto::MessageVectorSet* messages, std::string code, std::string message) {
  if (messages == nullptr) return;
  messages->diagnostics.push_back(MakeDiagnostic(std::move(code), "ERROR", std::move(message), "sb_listener.control"));
}

bool AppendBoundedText(std::vector<std::uint8_t>* out, const std::string& value) {
  if (value.size() > kHelloTextMax || value.size() > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

bool AppendManagementText(std::vector<std::uint8_t>* out,
                          const std::string& value,
                          std::size_t max_size = kManagementTextMax) {
  if (value.size() > max_size || value.size() > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

bool AppendManagementBytes(std::vector<std::uint8_t>* out,
                           const std::vector<std::uint8_t>& value,
                           std::size_t max_size) {
  if (value.size() > max_size || value.size() > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

std::optional<std::string> ReadManagementText(const std::vector<std::uint8_t>& payload,
                                              std::size_t* off,
                                              proto::MessageVectorSet* messages,
                                              const std::string& field,
                                              std::size_t max_size = kManagementTextMax) {
  if (off == nullptr || *off + 2 > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_TRUNCATED",
                   "management envelope text field is truncated");
    return std::nullopt;
  }
  const auto len = ReadU16(payload, *off);
  *off += 2;
  if (len > max_size || *off + len > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_FIELD_INVALID",
                   "management envelope text field length is invalid: " + field);
    return std::nullopt;
  }
  std::string value(reinterpret_cast<const char*>(payload.data() + *off), len);
  *off += len;
  return value;
}

std::optional<std::vector<std::uint8_t>> ReadManagementBytes(
    const std::vector<std::uint8_t>& payload,
    std::size_t* off,
    proto::MessageVectorSet* messages,
    const std::string& field,
    std::size_t max_size) {
  if (off == nullptr || *off + 2 > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_TRUNCATED",
                   "management envelope byte field is truncated");
    return std::nullopt;
  }
  const auto len = ReadU16(payload, *off);
  *off += 2;
  if (len > max_size || *off + len > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_FIELD_INVALID",
                   "management envelope byte field length is invalid: " + field);
    return std::nullopt;
  }
  std::vector<std::uint8_t> value(payload.begin() + static_cast<std::ptrdiff_t>(*off),
                                  payload.begin() + static_cast<std::ptrdiff_t>(*off + len));
  *off += len;
  return value;
}

std::string UpperManagementCommand(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> SplitManagementCommandWords(const std::string& command) {
  std::istringstream in(command);
  std::vector<std::string> words;
  std::string word;
  while (in >> word) {
    words.push_back(std::move(word));
  }
  return words;
}

std::vector<std::uint8_t> ManagementNonceFromRequestId(std::uint64_t request_id,
                                                       std::uint64_t issued_at_ms) {
  const std::string body = std::to_string(request_id) + "|" + std::to_string(issued_at_ms);
  proto::Bytes bytes(body.begin(), body.end());
  const auto digest = proto::Sha256(bytes);
  return std::vector<std::uint8_t>(digest.begin(), digest.begin() + 16);
}

std::uint32_t RequiredRightForOperation(const std::string& operation) {
  if (operation == "PING" || operation == "STATUS" || operation == "HEALTH" ||
      operation == "POOL_STATUS") {
    return kListenerManagementRightRead;
  }
  if (operation == "DRAIN" || operation == "UNDRAIN" || operation == "STOP" ||
      operation == "RELOAD") {
    return kListenerManagementRightLifecycle;
  }
  if (operation == "POOL_RESIZE" || operation == "POOL_RECYCLE" ||
      operation == "POOL_KILL" || operation == "POOL_RESTART") {
    return kListenerManagementRightPool;
  }
  if (operation == "DBBT_VALIDATE") return kListenerManagementRightDbbt;
  if (operation == "LPREFACE_VALIDATE") return kListenerManagementRightLpreface;
  if (operation == "SUPPORT_BUNDLE") return kListenerManagementRightSupport;
  return 0;
}

void AddArg(ListenerManagementEnvelope* envelope, std::string key, std::string value) {
  if (envelope == nullptr) return;
  envelope->arguments.push_back(ListenerManagementArgument{std::move(key), std::move(value)});
}

std::optional<ListenerManagementEnvelope> BuildEnvelopeFromNormalizedCommand(
    const std::string& original,
    const std::string& normalized) {
  ListenerManagementEnvelope envelope;
  const auto words = SplitManagementCommandWords(normalized);
  const auto raw_words = SplitManagementCommandWords(original);
  if (normalized == "PING") {
    envelope.operation = "PING";
  } else if (normalized == "STATUS") {
    envelope.operation = "STATUS";
  } else if (normalized == "HEALTH") {
    envelope.operation = "HEALTH";
  } else if (normalized == "POOL_STATUS" || normalized == "POOL STATUS") {
    envelope.operation = "POOL_STATUS";
  } else if (normalized == "DRAIN") {
    envelope.operation = "DRAIN";
  } else if (normalized == "UNDRAIN") {
    envelope.operation = "UNDRAIN";
  } else if (normalized == "RELOAD") {
    envelope.operation = "RELOAD";
  } else if (normalized == "STOP" || normalized == "STOP GRACEFUL" || normalized == "STOP FORCE") {
    envelope.operation = "STOP";
    AddArg(&envelope, "force", normalized == "STOP FORCE" ? "true" : "false");
  } else if (normalized == "POOL RESTART") {
    envelope.operation = "POOL_RESTART";
  } else if (normalized == "SUPPORT_BUNDLE" || normalized == "SUPPORT BUNDLE") {
    envelope.operation = "SUPPORT_BUNDLE";
  } else if (normalized.rfind("POOL_RESIZE ", 0) == 0 ||
             normalized.rfind("POOL RESIZE ", 0) == 0) {
    const std::size_t first_arg =
        (words.size() == 4 && words[0] == "POOL" && words[1] == "RESIZE") ? 2 : 1;
    if (words.size() != first_arg + 2) return std::nullopt;
    envelope.operation = "POOL_RESIZE";
    AddArg(&envelope, "min_workers", words[first_arg]);
    AddArg(&envelope, "max_workers", words[first_arg + 1]);
  } else if (normalized.rfind("POOL_RECYCLE ", 0) == 0 ||
             normalized.rfind("POOL RECYCLE ", 0) == 0) {
    const std::size_t worker_arg =
        (words.size() == 3 && words[0] == "POOL" && words[1] == "RECYCLE") ? 2 : 1;
    if (words.size() != worker_arg + 1 || raw_words.size() != words.size()) return std::nullopt;
    envelope.operation = "POOL_RECYCLE";
    AddArg(&envelope, "worker_id", raw_words[worker_arg]);
  } else if (normalized.rfind("KILL ", 0) == 0 ||
             normalized.rfind("POOL_KILL ", 0) == 0 ||
             normalized.rfind("POOL KILL ", 0) == 0) {
    std::size_t kill_arg = 1;
    if (words.size() == 3 && words[0] == "POOL" && words[1] == "KILL") {
      kill_arg = 2;
    }
    if (words.size() != kill_arg + 1 || raw_words.size() != words.size()) return std::nullopt;
    envelope.operation = "POOL_KILL";
    const auto& normalized_arg = words[kill_arg];
    const auto& raw_arg = raw_words[kill_arg];
    if (normalized_arg.rfind("WORKER_ID=", 0) == 0) {
      const auto eq = raw_arg.find('=');
      if (eq == std::string::npos || eq + 1 >= raw_arg.size()) return std::nullopt;
      AddArg(&envelope, "worker_id", raw_arg.substr(eq + 1));
    } else if (normalized_arg.rfind("CONNECTION_ID=", 0) == 0) {
      const auto eq = raw_arg.find('=');
      if (eq == std::string::npos || eq + 1 >= raw_arg.size()) return std::nullopt;
      AddArg(&envelope, "connection_id", raw_arg.substr(eq + 1));
    } else {
      AddArg(&envelope, "connection_id", raw_arg);
    }
  } else if (normalized.rfind("DBBT_VALIDATE ", 0) == 0) {
    envelope.operation = "DBBT_VALIDATE";
    const auto prefix = std::string("DBBT_VALIDATE ");
    AddArg(&envelope, "token_hex", original.substr(prefix.size()));
  } else if (normalized.rfind("LPREFACE_VALIDATE ", 0) == 0) {
    envelope.operation = "LPREFACE_VALIDATE";
    const auto prefix = std::string("LPREFACE_VALIDATE ");
    AddArg(&envelope, "preface_hex", original.substr(prefix.size()));
  } else {
    return std::nullopt;
  }
  return envelope;
}

#ifdef _WIN32
bool WaitReadable(std::intptr_t fd, std::uint32_t timeout_ms) {
  SOCKET socket = static_cast<SOCKET>(fd);
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket, &read_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
  const int rc = ::select(0, &read_set, nullptr, nullptr, &timeout);
  return rc > 0 && FD_ISSET(socket, &read_set);
}

bool ReadAll(std::intptr_t fd, std::uint8_t* data, std::size_t size) {
  SOCKET socket = static_cast<SOCKET>(fd);
  std::size_t read_total = 0;
  while (read_total < size) {
    const int want = static_cast<int>(std::min<std::size_t>(
        size - read_total, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int rc = ::recv(socket, reinterpret_cast<char*>(data + read_total), want, 0);
    if (rc <= 0) return false;
    read_total += static_cast<std::size_t>(rc);
  }
  return true;
}

bool WriteAll(std::intptr_t fd, const std::uint8_t* data, std::size_t size) {
  SOCKET socket = static_cast<SOCKET>(fd);
  std::size_t written = 0;
  while (written < size) {
    const int want = static_cast<int>(std::min<std::size_t>(
        size - written, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int rc = ::send(socket, reinterpret_cast<const char*>(data + written), want, 0);
    if (rc <= 0) return false;
    written += static_cast<std::size_t>(rc);
  }
  return true;
}
#else
bool WaitReadable(int fd, std::uint32_t timeout_ms) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  for (;;) {
    const int rc = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (rc > 0) return (pfd.revents & POLLIN) != 0;
    if (rc == 0) return false;
    if (errno == EINTR) continue;
    return false;
  }
}

bool ReadAll(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t read_total = 0;
  while (read_total < size) {
    const ssize_t rc = ::read(fd, data + read_total, size - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WriteAll(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t rc = ::write(fd, data + written, size - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}
#endif

} // namespace

std::vector<std::uint8_t> EncodeControlFrame(const ListenerControlFrame& frame) {
  proto::ControlPlaneMessage message;
  message.message_type = static_cast<std::uint16_t>(frame.opcode);
  message.flags = frame.flags;
  message.request_id = frame.sequence;
  if (!frame.payload.empty()) {
    message.payload = frame.payload;
  } else {
    message.payload.assign(frame.payload_json.begin(), frame.payload_json.end());
  }
  return proto::EncodeControlPlaneMessage(message);
}

ListenerControlDecodeResult DecodeControlFrame(const std::vector<std::uint8_t>& bytes) {
  std::vector<proto::Diagnostic> diagnostics;
  auto decoded = proto::DecodeControlPlaneMessage(bytes, &diagnostics);
  if (!decoded) {
    return {{}, MakeMessageVectorSet(std::move(diagnostics)), false};
  }
  ListenerControlFrame frame;
  frame.opcode = static_cast<ListenerControlOpcode>(decoded->message_type);
  frame.flags = decoded->flags;
  frame.sequence = decoded->request_id;
  frame.payload = std::move(decoded->payload);
  frame.payload_json.assign(frame.payload.begin(), frame.payload.end());
  return {std::move(frame), MakeMessageVectorSet({}), true};
}

std::string ControlOpcodeName(ListenerControlOpcode opcode) {
  switch (opcode) {
    case ListenerControlOpcode::kHello: return "HELLO";
    case ListenerControlOpcode::kHelloAck: return "HELLO_ACK";
    case ListenerControlOpcode::kSpawnRequest: return "SPAWN_REQUEST";
    case ListenerControlOpcode::kSpawnReady: return "SPAWN_READY";
    case ListenerControlOpcode::kHandoffSocket: return "HANDOFF_SOCKET";
    case ListenerControlOpcode::kHandoffAck: return "HANDOFF_ACK";
    case ListenerControlOpcode::kHealthCheck: return "HEALTH_CHECK";
    case ListenerControlOpcode::kHealthReport: return "HEALTH_REPORT";
    case ListenerControlOpcode::kSessionBindingReport: return "SESSION_BINDING_REPORT";
    case ListenerControlOpcode::kSessionBindingClear: return "SESSION_BINDING_CLEAR";
    case ListenerControlOpcode::kTakeoverRequest: return "TAKEOVER_REQUEST";
    case ListenerControlOpcode::kTakeoverDecision: return "TAKEOVER_DECISION";
    case ListenerControlOpcode::kTakeoverProbe: return "TAKEOVER_PROBE";
    case ListenerControlOpcode::kTakeoverProbeResult: return "TAKEOVER_PROBE_RESULT";
    case ListenerControlOpcode::kRecycle: return "RECYCLE";
    case ListenerControlOpcode::kShutdown: return "SHUTDOWN";
    case ListenerControlOpcode::kManagementCommand: return "MANAGEMENT_COMMAND";
    case ListenerControlOpcode::kManagementResponse: return "MANAGEMENT_RESPONSE";
    case ListenerControlOpcode::kErrorMessage: return "ERROR_MESSAGE";
  }
  return "UNKNOWN";
}

bool IsListenerManagementEnvelopePayload(const std::vector<std::uint8_t>& payload) {
  return payload.size() >= 4 &&
         payload[0] == kManagementEnvelopeMagic[0] &&
         payload[1] == kManagementEnvelopeMagic[1] &&
         payload[2] == kManagementEnvelopeMagic[2] &&
         payload[3] == kManagementEnvelopeMagic[3];
}

std::vector<std::uint8_t> ListenerManagementEnvelopeSigningBody(
    const ListenerManagementEnvelope& envelope) {
  std::vector<std::uint8_t> out;
  out.reserve(256 + envelope.arguments.size() * 32);
  out.insert(out.end(), std::begin(kManagementEnvelopeMagic), std::end(kManagementEnvelopeMagic));
  PutU16(&out, envelope.version);
  if (!AppendManagementText(&out, envelope.operation, kManagementFieldMax) ||
      !AppendManagementText(&out, envelope.role, kManagementFieldMax) ||
      !AppendManagementText(&out, envelope.authority_class, kManagementFieldMax) ||
      !AppendManagementText(&out, envelope.request_id, kManagementFieldMax) ||
      !AppendManagementBytes(&out, envelope.nonce, kManagementNonceMax)) {
    return {};
  }
  PutU64(&out, envelope.issued_at_ms);
  PutU64(&out, envelope.expires_at_ms);
  PutU32(&out, envelope.rights);
  if (envelope.arguments.size() > kManagementArgumentMax ||
      envelope.arguments.size() > std::numeric_limits<std::uint16_t>::max()) {
    return {};
  }
  PutU16(&out, static_cast<std::uint16_t>(envelope.arguments.size()));
  for (const auto& argument : envelope.arguments) {
    if (!AppendManagementText(&out, argument.key, kManagementFieldMax) ||
        !AppendManagementText(&out, argument.value, kManagementTextMax)) {
      return {};
    }
  }
  if (!AppendManagementText(&out, envelope.authenticator_scheme, kManagementFieldMax)) {
    return {};
  }
  return out;
}

std::vector<std::uint8_t> EncodeListenerManagementEnvelope(
    const ListenerManagementEnvelope& envelope) {
  auto out = ListenerManagementEnvelopeSigningBody(envelope);
  if (out.empty()) return {};
  if (!AppendManagementBytes(&out, envelope.authenticator, kManagementAuthenticatorMax)) {
    return {};
  }
  return out;
}

void SignListenerManagementEnvelopeHmacSha256(ListenerManagementEnvelope* envelope,
                                              const proto::Bytes& key) {
  if (envelope == nullptr) return;
  envelope->authenticator_scheme = kListenerManagementAuthHmacSha256;
  envelope->authenticator.clear();
  const auto body = ListenerManagementEnvelopeSigningBody(*envelope);
  const auto signature = proto::HmacSha256(key, body);
  envelope->authenticator.assign(signature.begin(), signature.end());
}

std::optional<ListenerManagementEnvelope> DecodeListenerManagementEnvelope(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages) {
  if (!IsListenerManagementEnvelopePayload(payload) ||
      payload.size() < kManagementEnvelopeMinSize) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_MALFORMED",
                   "management command payload is not a structured management envelope");
    return std::nullopt;
  }
  std::size_t off = 4;
  if (off + 2 > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_TRUNCATED",
                   "management envelope version is truncated");
    return std::nullopt;
  }
  ListenerManagementEnvelope envelope;
  envelope.version = ReadU16(payload, off);
  off += 2;
  auto operation = ReadManagementText(payload, &off, messages, "operation", kManagementFieldMax);
  auto role = operation ? ReadManagementText(payload, &off, messages, "role", kManagementFieldMax) : std::nullopt;
  auto authority = role ? ReadManagementText(payload, &off, messages, "authority_class", kManagementFieldMax) : std::nullopt;
  auto request_id = authority ? ReadManagementText(payload, &off, messages, "request_id", kManagementFieldMax) : std::nullopt;
  auto nonce = request_id ? ReadManagementBytes(payload, &off, messages, "nonce", kManagementNonceMax) : std::nullopt;
  if (!operation || !role || !authority || !request_id || !nonce) return std::nullopt;
  if (off + 20 > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_TRUNCATED",
                   "management envelope time window or rights are truncated");
    return std::nullopt;
  }
  envelope.operation = std::move(*operation);
  envelope.role = std::move(*role);
  envelope.authority_class = std::move(*authority);
  envelope.request_id = std::move(*request_id);
  envelope.nonce = std::move(*nonce);
  envelope.issued_at_ms = ReadU64(payload, off);
  off += 8;
  envelope.expires_at_ms = ReadU64(payload, off);
  off += 8;
  envelope.rights = ReadU32(payload, off);
  off += 4;
  if (off + 2 > payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_TRUNCATED",
                   "management envelope argument count is truncated");
    return std::nullopt;
  }
  const auto arg_count = ReadU16(payload, off);
  off += 2;
  if (arg_count > kManagementArgumentMax) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_ARGUMENTS_INVALID",
                   "management envelope has too many command arguments");
    return std::nullopt;
  }
  envelope.arguments.reserve(arg_count);
  for (std::uint16_t i = 0; i < arg_count; ++i) {
    auto key = ReadManagementText(payload, &off, messages, "argument.key", kManagementFieldMax);
    auto value = key ? ReadManagementText(payload, &off, messages, "argument.value", kManagementTextMax) : std::nullopt;
    if (!key || !value) return std::nullopt;
    envelope.arguments.push_back(ListenerManagementArgument{std::move(*key), std::move(*value)});
  }
  auto scheme = ReadManagementText(payload, &off, messages, "authenticator_scheme", kManagementFieldMax);
  auto authenticator = scheme ? ReadManagementBytes(payload,
                                                    &off,
                                                    messages,
                                                    "authenticator",
                                                    kManagementAuthenticatorMax) : std::nullopt;
  if (!scheme || !authenticator) return std::nullopt;
  if (off != payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.MANAGEMENT.ENVELOPE_TRAILING_BYTES",
                   "management envelope has trailing bytes");
    return std::nullopt;
  }
  envelope.authenticator_scheme = std::move(*scheme);
  envelope.authenticator = std::move(*authenticator);
  return envelope;
}

std::optional<ListenerManagementEnvelope> BuildListenerManagementEnvelopeFromCommand(
    const std::string& command,
    std::uint64_t request_id,
    std::uint64_t issued_at_ms,
    std::uint64_t expires_at_ms,
    const std::string& role,
    const std::string& authority_class,
    const std::string& authenticator_scheme) {
  auto envelope = BuildEnvelopeFromNormalizedCommand(command, UpperManagementCommand(command));
  if (!envelope) return std::nullopt;
  envelope->version = 1;
  envelope->role = role;
  envelope->authority_class = authority_class;
  envelope->request_id = std::to_string(request_id);
  envelope->nonce = ManagementNonceFromRequestId(request_id, issued_at_ms);
  envelope->issued_at_ms = issued_at_ms;
  envelope->expires_at_ms = expires_at_ms;
  envelope->rights = RequiredRightForOperation(envelope->operation);
  envelope->authenticator_scheme = authenticator_scheme;
  envelope->authenticator.clear();
  return envelope;
}

std::vector<std::uint8_t> EncodeHelloPayload(const ParserHelloPayload& hello) {
  std::vector<std::uint8_t> out;
  out.reserve(kHelloFixedSize + hello.profile_id.size() + hello.bundle_contract_id.size() + 4);
  PutFixedString(&out, hello.protocol, 16);
  PutU32(&out, hello.pid);
  PutU64(&out, hello.worker_id);
  PutU32(&out, hello.dialect_protocol_version);
  PutU32(&out, hello.parser_api_major);
  if (!AppendBoundedText(&out, hello.profile_id)) return {};
  if (!AppendBoundedText(&out, hello.bundle_contract_id)) return {};
  return out;
}

std::optional<ParserHelloPayload> DecodeHelloPayload(const std::vector<std::uint8_t>& payload,
                                                     proto::MessageVectorSet* messages) {
  if (payload.size() < kHelloFixedSize) {
    AddDecodeError(messages, "LISTENER.HELLO_TRUNCATED", "HELLO payload is shorter than the fixed header.");
    return std::nullopt;
  }
  ParserHelloPayload hello;
  hello.protocol = ReadFixedString(payload, 0, 16);
  hello.pid = ReadU32(payload, 16);
  hello.worker_id = ReadU64(payload, 20);
  hello.dialect_protocol_version = ReadU32(payload, 28);
  hello.parser_api_major = ReadU32(payload, 32);
  std::size_t off = 36;
  const auto profile_len = ReadU16(payload, off);
  off += 2;
  if (profile_len > kHelloTextMax || off + profile_len + 2 > payload.size()) {
    AddDecodeError(messages, "LISTENER.HELLO_TEXT_TOO_LARGE", "HELLO profile text length is invalid.");
    return std::nullopt;
  }
  hello.profile_id.assign(reinterpret_cast<const char*>(payload.data() + off), profile_len);
  off += profile_len;
  const auto bundle_len = ReadU16(payload, off);
  off += 2;
  if (bundle_len > kHelloTextMax || off + bundle_len != payload.size()) {
    AddDecodeError(messages, "LISTENER.HELLO_TRAILING_BYTES", "HELLO bundle text length or trailing bytes are invalid.");
    return std::nullopt;
  }
  hello.bundle_contract_id.assign(reinterpret_cast<const char*>(payload.data() + off), bundle_len);
  return hello;
}

std::vector<std::uint8_t> EncodeHelloAckPayload(const HelloAckPayload& ack) {
  std::vector<std::uint8_t> out;
  out.reserve(kHelloAckMinSize + ack.reason.size());
  out.push_back(ack.accepted ? 1 : 0);
  PutU32(&out, ack.supported_parser_api_floor);
  PutU32(&out, ack.supported_parser_api_ceiling);
  if (!AppendBoundedText(&out, ack.reason)) return {};
  return out;
}

std::optional<HelloAckPayload> DecodeHelloAckPayload(const std::vector<std::uint8_t>& payload,
                                                     proto::MessageVectorSet* messages) {
  if (payload.size() < kHelloAckMinSize) {
    AddDecodeError(messages, "LISTENER.HELLO_ACK_TRUNCATED", "HELLO_ACK payload is truncated.");
    return std::nullopt;
  }
  HelloAckPayload ack;
  ack.accepted = payload[0] == 1;
  ack.supported_parser_api_floor = ReadU32(payload, 1);
  ack.supported_parser_api_ceiling = ReadU32(payload, 5);
  const auto reason_len = ReadU16(payload, 9);
  if (reason_len > kHelloTextMax || 11u + reason_len != payload.size()) {
    AddDecodeError(messages, "LISTENER.HELLO_ACK_TRAILING_BYTES", "HELLO_ACK reason length or trailing bytes are invalid.");
    return std::nullopt;
  }
  ack.reason.assign(reinterpret_cast<const char*>(payload.data() + 11), reason_len);
  return ack;
}

std::vector<std::uint8_t> EncodeHandoffSocketPayload(const HandoffSocketPayload& handoff) {
  std::vector<std::uint8_t> out;
  out.reserve(kHandoffSocketSize + handoff.auth_provider_family.size() +
              handoff.auth_principal.size() + handoff.auth_token.size() + 6);
  PutU64(&out, handoff.connection_id);
  PutFixedString(&out, handoff.protocol, 16);
  PutFixedString(&out, handoff.client_addr, 48);
  PutU16(&out, handoff.client_port);
  out.push_back(handoff.tls_active ? 1 : 0);
  PutU16(&out, 0);
  out.insert(out.end(), handoff.tls_state.begin(), handoff.tls_state.end());
  out.insert(out.end(), handoff.db_uuid.begin(), handoff.db_uuid.end());
  out.insert(out.end(), handoff.dbbt_id.begin(), handoff.dbbt_id.end());
  out.insert(out.end(), handoff.manager_session_id.begin(), handoff.manager_session_id.end());
  PutU32(&out, handoff.listener_id);
  if (!handoff.auth_provider_family.empty() || !handoff.auth_principal.empty() ||
      !handoff.auth_token.empty()) {
    if (!AppendBoundedText(&out, handoff.auth_provider_family)) return {};
    if (!AppendBoundedText(&out, handoff.auth_principal)) return {};
    if (!AppendBoundedText(&out, handoff.auth_token)) return {};
  }
  return out;
}

std::optional<HandoffSocketPayload> DecodeHandoffSocketPayload(const std::vector<std::uint8_t>& payload,
                                                               proto::MessageVectorSet* messages) {
  if (payload.size() < kHandoffSocketSize) {
    AddDecodeError(messages, "LISTENER.HANDOFF_SOCKET_TRUNCATED", "HANDOFF_SOCKET payload is truncated.");
    return std::nullopt;
  }
  HandoffSocketPayload handoff;
  handoff.connection_id = ReadU64(payload, 0);
  handoff.protocol = ReadFixedString(payload, 8, 16);
  handoff.client_addr = ReadFixedString(payload, 24, 48);
  handoff.client_port = ReadU16(payload, 72);
  handoff.tls_active = payload[74] == 1;
  std::copy(payload.begin() + 77, payload.begin() + 141, handoff.tls_state.begin());
  handoff.db_uuid = ReadUuid(payload, 141);
  handoff.dbbt_id = ReadUuid(payload, 157);
  handoff.manager_session_id = ReadUuid(payload, 173);
  handoff.listener_id = ReadU32(payload, 189);
  std::size_t off = kHandoffSocketSize;
  if (off == payload.size()) return handoff;
  auto read_text = [&](std::string* out) -> bool {
    if (off + 2 > payload.size()) return false;
    const auto len = ReadU16(payload, off);
    off += 2;
    if (len > kHelloTextMax || off + len > payload.size()) return false;
    out->assign(reinterpret_cast<const char*>(payload.data() + off), len);
    off += len;
    return true;
  };
  if (!read_text(&handoff.auth_provider_family) ||
      !read_text(&handoff.auth_principal) ||
      !read_text(&handoff.auth_token) ||
      off != payload.size()) {
    AddDecodeError(messages, "LISTENER.HANDOFF_SOCKET_AUTH_INVALID",
                   "HANDOFF_SOCKET auth extension is invalid.");
    return std::nullopt;
  }
  return handoff;
}

std::vector<std::uint8_t> EncodeWindowsSocketHandoffPayload(
    const HandoffSocketPayload& handoff,
    const std::uint8_t* socket_protocol_info,
    std::size_t socket_protocol_info_size) {
  if (socket_protocol_info == nullptr ||
      socket_protocol_info_size == 0 ||
      socket_protocol_info_size > kWindowsSocketProtocolInfoMax ||
      socket_protocol_info_size > std::numeric_limits<std::uint16_t>::max()) {
    return {};
  }
  const auto base = EncodeHandoffSocketPayload(handoff);
  if (base.empty() || base.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  std::vector<std::uint8_t> out;
  out.reserve(kWindowsSocketHandoffHeaderSize + base.size() + socket_protocol_info_size);
  out.push_back('S');
  out.push_back('B');
  out.push_back('W');
  out.push_back('S');
  PutU16(&out, 1);
  PutU16(&out, static_cast<std::uint16_t>(socket_protocol_info_size));
  PutU32(&out, static_cast<std::uint32_t>(base.size()));
  out.insert(out.end(), base.begin(), base.end());
  out.insert(out.end(), socket_protocol_info, socket_protocol_info + socket_protocol_info_size);
  return out;
}

std::optional<WindowsSocketHandoffPayload> DecodeWindowsSocketHandoffPayload(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages) {
  if (payload.size() < kWindowsSocketHandoffHeaderSize ||
      payload[0] != 'S' || payload[1] != 'B' || payload[2] != 'W' || payload[3] != 'S') {
    AddDecodeError(messages,
                   "LISTENER.WINDOWS_HANDOFF_BAD_MAGIC",
                   "Windows HANDOFF_SOCKET payload magic is invalid.");
    return std::nullopt;
  }
  const auto version = ReadU16(payload, 4);
  const auto info_len = ReadU16(payload, 6);
  const auto base_len = ReadU32(payload, 8);
  if (version != 1) {
    AddDecodeError(messages,
                   "LISTENER.WINDOWS_HANDOFF_VERSION_UNSUPPORTED",
                   "Windows HANDOFF_SOCKET payload version is not supported.");
    return std::nullopt;
  }
  if (info_len == 0 || info_len > kWindowsSocketProtocolInfoMax ||
      base_len < kHandoffSocketSize ||
      kWindowsSocketHandoffHeaderSize + static_cast<std::size_t>(base_len) +
              static_cast<std::size_t>(info_len) !=
          payload.size()) {
    AddDecodeError(messages,
                   "LISTENER.WINDOWS_HANDOFF_LENGTH_INVALID",
                   "Windows HANDOFF_SOCKET payload length is invalid.");
    return std::nullopt;
  }
  std::vector<std::uint8_t> base(payload.begin() + kWindowsSocketHandoffHeaderSize,
                                payload.begin() + kWindowsSocketHandoffHeaderSize +
                                    static_cast<std::ptrdiff_t>(base_len));
  auto handoff = DecodeHandoffSocketPayload(base, messages);
  if (!handoff) return std::nullopt;
  WindowsSocketHandoffPayload decoded;
  decoded.handoff = *handoff;
  decoded.socket_protocol_info.assign(
      payload.begin() + kWindowsSocketHandoffHeaderSize + static_cast<std::ptrdiff_t>(base_len),
      payload.end());
  return decoded;
}

std::optional<HandoffAckPayload> DecodeHandoffAckPayload(const std::vector<std::uint8_t>& payload,
                                                         proto::MessageVectorSet* messages) {
  if (payload.size() != kHandoffAckMinSize && payload.size() < kHandoffAckMinSize + 2) {
    AddDecodeError(messages, "LISTENER.HANDOFF_ACK_TRUNCATED", "HANDOFF_ACK payload is truncated.");
    return std::nullopt;
  }
  HandoffAckPayload ack;
  ack.connection_id_echo = ReadU64(payload, 0);
  ack.accepted = payload[8] == 0;
  if (payload.size() == kHandoffAckMinSize) return ack;
  const auto reason_len = ReadU16(payload, 9);
  if (11u + reason_len != payload.size() || reason_len > kHelloTextMax) {
    AddDecodeError(messages, "LISTENER.HANDOFF_ACK_TRAILING_BYTES", "HANDOFF_ACK reason length or trailing bytes are invalid.");
    return std::nullopt;
  }
  ack.reason.assign(reinterpret_cast<const char*>(payload.data() + 11), reason_len);
  return ack;
}

std::vector<std::uint8_t> EncodeHandoffAckPayload(const HandoffAckPayload& ack) {
  std::vector<std::uint8_t> out;
  out.reserve(kHandoffAckMinSize + 2 + ack.reason.size());
  PutU64(&out, ack.connection_id_echo);
  out.push_back(ack.accepted ? 0 : 1);
  if (!ack.reason.empty()) {
    AppendBoundedText(&out, ack.reason);
  }
  return out;
}

std::vector<std::uint8_t> EncodeHealthReportPayload(const HealthReportPayload& report) {
  std::vector<std::uint8_t> out;
  out.reserve(kHealthReportSize);
  PutU64(&out, report.request_id_echo);
  out.push_back(report.state);
  PutU16(&out, 0);
  PutU32(&out, report.last_error);
  return out;
}

std::optional<HealthReportPayload> DecodeHealthReportPayload(const std::vector<std::uint8_t>& payload,
                                                             proto::MessageVectorSet* messages) {
  if (payload.size() != kHealthReportSize) {
    AddDecodeError(messages, "LISTENER.HEALTH_REPORT_LENGTH_INVALID", "HEALTH_REPORT payload length is invalid.");
    return std::nullopt;
  }
  HealthReportPayload report;
  report.request_id_echo = ReadU64(payload, 0);
  report.state = payload[8];
  report.last_error = ReadU32(payload, 11);
  return report;
}

std::vector<std::uint8_t> EncodeSessionBindingReportPayload(const SessionBindingReportPayload& report) {
  if (report.effective_group_ids.size() > kMaxSessionBindingGroups) return {};
  std::vector<std::uint8_t> out;
  out.reserve(kSessionBindingMinSize + (report.effective_group_ids.size() * 16u));
  PutUuid(&out, report.attachment_id);
  PutUuid(&out, report.catalog_session_id);
  PutUuid(&out, report.transaction_uuid);
  PutUuid(&out, report.protocol_session_id);
  PutUuid(&out, report.authenticated_principal_id);
  PutUuid(&out, report.session_user_id);
  PutUuid(&out, report.active_role_id);
  PutUuid(&out, report.authkey_id);
  PutU64(&out, report.current_txn_id);
  PutU16(&out, static_cast<std::uint16_t>(report.effective_group_ids.size()));
  for (const auto& group : report.effective_group_ids) {
    PutUuid(&out, group);
  }
  return out;
}

std::optional<SessionBindingReportPayload> DecodeSessionBindingReportPayload(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages) {
  if (payload.size() < kSessionBindingMinSize) {
    AddDecodeError(messages, "LISTENER.SESSION_BINDING_TRUNCATED", "SESSION_BINDING_REPORT payload is truncated.");
    return std::nullopt;
  }
  SessionBindingReportPayload report;
  report.attachment_id = ReadUuid(payload, 0);
  report.catalog_session_id = ReadUuid(payload, 16);
  report.transaction_uuid = ReadUuid(payload, 32);
  report.protocol_session_id = ReadUuid(payload, 48);
  report.authenticated_principal_id = ReadUuid(payload, 64);
  report.session_user_id = ReadUuid(payload, 80);
  report.active_role_id = ReadUuid(payload, 96);
  report.authkey_id = ReadUuid(payload, 112);
  report.current_txn_id = ReadU64(payload, 128);
  const auto group_count = ReadU16(payload, 136);
  if (group_count > kMaxSessionBindingGroups) {
    AddDecodeError(messages, "LISTENER.SESSION_BINDING_GROUPS_TOO_LARGE", "SESSION_BINDING_REPORT group count is too large.");
    return std::nullopt;
  }
  const std::size_t expected = kSessionBindingMinSize + (static_cast<std::size_t>(group_count) * 16u);
  if (expected != payload.size()) {
    AddDecodeError(messages, "LISTENER.SESSION_BINDING_TRAILING_BYTES", "SESSION_BINDING_REPORT trailing bytes are invalid.");
    return std::nullopt;
  }
  report.effective_group_ids.reserve(group_count);
  std::size_t off = kSessionBindingMinSize;
  for (std::uint16_t i = 0; i < group_count; ++i) {
    report.effective_group_ids.push_back(ReadUuid(payload, off));
    off += 16;
  }
  return report;
}

std::vector<std::uint8_t> EncodeTakeoverRequestPayload(const TakeoverRequestPayload& request) {
  if (request.group_ids.size() > kMaxSessionBindingGroups) return {};
  std::vector<std::uint8_t> out;
  PutU16(&out, request.mask);
  PutU16(&out, static_cast<std::uint16_t>(request.group_ids.size()));
  if (request.mask & kTakeoverClaimAttachmentId) PutUuid(&out, request.attachment_id);
  if (request.mask & kTakeoverClaimCatalogSessionId) PutUuid(&out, request.catalog_session_id);
  if (request.mask & kTakeoverClaimProtocolSessionId) PutUuid(&out, request.protocol_session_id);
  if (request.mask & kTakeoverClaimAuthkeyId) PutUuid(&out, request.authkey_id);
  if (request.mask & kTakeoverClaimAuthenticatedPrincipalId) PutUuid(&out, request.authenticated_principal_id);
  if (request.mask & kTakeoverClaimSessionUserId) PutUuid(&out, request.session_user_id);
  if (request.mask & kTakeoverClaimActiveRoleId) PutUuid(&out, request.active_role_id);
  if (request.mask & kTakeoverClaimCurrentTxnId) PutU64(&out, request.current_txn_id);
  for (const auto& group : request.group_ids) {
    PutUuid(&out, group);
  }
  return out;
}

std::optional<TakeoverRequestPayload> DecodeTakeoverRequestPayload(const std::vector<std::uint8_t>& payload,
                                                                   proto::MessageVectorSet* messages) {
  if (payload.size() < kTakeoverRequestMinSize) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST payload is truncated.");
    return std::nullopt;
  }
  TakeoverRequestPayload request;
  request.mask = ReadU16(payload, 0);
  const auto group_count = ReadU16(payload, 2);
  if (request.mask == 0 && group_count == 0) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_EMPTY", "TAKEOVER_REQUEST has no claims.");
    return std::nullopt;
  }
  std::size_t off = 4;
  auto read_claim_uuid = [&](std::array<std::uint8_t, 16>* target) -> bool {
    if (payload.size() < off + 16) return false;
    *target = ReadUuid(payload, off);
    off += 16;
    return true;
  };
  if ((request.mask & kTakeoverClaimAttachmentId) && !read_claim_uuid(&request.attachment_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST attachment_id is truncated.");
    return std::nullopt;
  }
  if ((request.mask & kTakeoverClaimCatalogSessionId) && !read_claim_uuid(&request.catalog_session_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST catalog_session_id is truncated.");
    return std::nullopt;
  }
  if ((request.mask & kTakeoverClaimProtocolSessionId) && !read_claim_uuid(&request.protocol_session_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST protocol_session_id is truncated.");
    return std::nullopt;
  }
  if ((request.mask & kTakeoverClaimAuthkeyId) && !read_claim_uuid(&request.authkey_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST authkey_id is truncated.");
    return std::nullopt;
  }
  if ((request.mask & kTakeoverClaimAuthenticatedPrincipalId) && !read_claim_uuid(&request.authenticated_principal_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST authenticated_principal_id is truncated.");
    return std::nullopt;
  }
  if ((request.mask & kTakeoverClaimSessionUserId) && !read_claim_uuid(&request.session_user_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST session_user_id is truncated.");
    return std::nullopt;
  }
  if ((request.mask & kTakeoverClaimActiveRoleId) && !read_claim_uuid(&request.active_role_id)) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_REQUEST active_role_id is truncated.");
    return std::nullopt;
  }
  if (request.mask & kTakeoverClaimCurrentTxnId) {
    if (payload.size() < off + 8) {
      AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TXN_ID_TRUNCATED", "TAKEOVER_REQUEST current_txn_id is truncated.");
      return std::nullopt;
    }
    request.current_txn_id = ReadU64(payload, off);
    off += 8;
  }
  if (group_count > kMaxSessionBindingGroups) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRAILING_BYTES", "TAKEOVER_REQUEST group count is too large.");
    return std::nullopt;
  }
  request.group_ids.reserve(group_count);
  for (std::uint16_t i = 0; i < group_count; ++i) {
    if (payload.size() < off + 16) {
      AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRAILING_BYTES", "TAKEOVER_REQUEST group list is truncated.");
      return std::nullopt;
    }
    request.group_ids.push_back(ReadUuid(payload, off));
    off += 16;
  }
  if (off != payload.size()) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRAILING_BYTES", "TAKEOVER_REQUEST trailing bytes are invalid.");
    return std::nullopt;
  }
  return request;
}

std::vector<std::uint8_t> EncodeTakeoverDecisionPayload(const TakeoverDecisionPayload& decision) {
  if (decision.reason.size() > std::numeric_limits<std::uint16_t>::max()) return {};
  std::vector<std::uint8_t> out;
  out.reserve(kTakeoverDecisionMinSize + decision.reason.size());
  out.push_back(decision.allowed ? 1 : 0);
  PutU16(&out, static_cast<std::uint16_t>(decision.reason.size()));
  out.insert(out.end(), decision.reason.begin(), decision.reason.end());
  return out;
}

std::optional<TakeoverDecisionPayload> DecodeTakeoverDecisionPayload(const std::vector<std::uint8_t>& payload,
                                                                     proto::MessageVectorSet* messages) {
  if (payload.size() < kTakeoverDecisionMinSize) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRUNCATED", "TAKEOVER_DECISION payload is truncated.");
    return std::nullopt;
  }
  TakeoverDecisionPayload decision;
  decision.allowed = payload[0] == 1;
  const auto reason_len = ReadU16(payload, 1);
  if (static_cast<std::size_t>(reason_len) + kTakeoverDecisionMinSize != payload.size()) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_REQUEST_TRAILING_BYTES", "TAKEOVER_DECISION trailing bytes are invalid.");
    return std::nullopt;
  }
  decision.reason.assign(reinterpret_cast<const char*>(payload.data() + kTakeoverDecisionMinSize), reason_len);
  return decision;
}

std::vector<std::uint8_t> EncodeTakeoverProbeResultPayload(const TakeoverProbeResultPayload& result) {
  return {result.flags};
}

std::optional<TakeoverProbeResultPayload> DecodeTakeoverProbeResultPayload(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages) {
  if (payload.size() != 1) {
    AddDecodeError(messages, "LISTENER.TAKEOVER_PROBE_RESULT_LENGTH_INVALID", "TAKEOVER_PROBE_RESULT payload length is invalid.");
    return std::nullopt;
  }
  return TakeoverProbeResultPayload{payload[0]};
}

std::vector<std::uint8_t> EncodeRecyclePayload(std::uint16_t reason_code) {
  std::vector<std::uint8_t> out;
  PutU16(&out, reason_code);
  return out;
}

std::optional<std::uint16_t> DecodeRecyclePayload(const std::vector<std::uint8_t>& payload,
                                                  proto::MessageVectorSet* messages) {
  if (payload.size() != 2) {
    AddDecodeError(messages, "LISTENER.RECYCLE_LENGTH_INVALID", "RECYCLE payload length is invalid.");
    return std::nullopt;
  }
  const auto reason_code = ReadU16(payload, 0);
  if (reason_code >= 5) {
    AddDecodeError(messages, "LISTENER.RECYCLE_REASON_RESERVED", "RECYCLE reason code is reserved.");
    return std::nullopt;
  }
  return reason_code;
}

std::vector<std::uint8_t> EncodeErrorMessagePayload(const ErrorMessagePayload& error) {
  return std::vector<std::uint8_t>(error.reason.begin(), error.reason.end());
}

std::optional<ErrorMessagePayload> DecodeErrorMessagePayload(const std::vector<std::uint8_t>& payload,
                                                             proto::MessageVectorSet* messages) {
  if (payload.size() > kSbctMaxPayloadSize) {
    AddDecodeError(messages, "CONTROL.PAYLOAD_TOO_LARGE", "ERROR_MESSAGE payload is too large.");
    return std::nullopt;
  }
  ErrorMessagePayload error;
  error.reason.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
  return error;
}

#ifdef _WIN32
bool SendControlFrame(std::intptr_t fd, const ListenerControlFrame& frame, std::intptr_t fd_to_transfer) {
  if (fd_to_transfer >= 0) return false;
  const auto encoded = EncodeControlFrame(frame);
  if (encoded.empty()) return false;
  return WriteAll(fd, encoded.data(), encoded.size());
}

bool ReadControlFrame(std::intptr_t fd,
                      ListenerControlDecodeResult* decoded,
                      std::intptr_t* received_fd,
                      std::uint32_t timeout_ms) {
  if (received_fd != nullptr) *received_fd = -1;
  if (decoded == nullptr) return false;
  if (!WaitReadable(fd, timeout_ms)) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.READ_TIMEOUT", "ERROR", "control-plane read timed out", "sb_listener.control")}), false};
    return false;
  }

  std::vector<std::uint8_t> bytes(kSbctHeaderSize);
  if (!ReadAll(fd, bytes.data(), bytes.size())) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.FRAME_TRUNCATED", "ERROR", "control-plane header is truncated", "sb_listener.control")}), false};
    return false;
  }
  const auto payload_len = ReadU64(bytes, 20);
  if (payload_len > kSbctMaxPayloadSize) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.PAYLOAD_TOO_LARGE", "ERROR", "control-plane payload is too large", "sb_listener.control")}), false};
    return false;
  }
  bytes.resize(kSbctHeaderSize + static_cast<std::size_t>(payload_len));
  if (payload_len != 0 && !ReadAll(fd, bytes.data() + kSbctHeaderSize, static_cast<std::size_t>(payload_len))) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.FRAME_TRUNCATED", "ERROR", "control-plane payload is truncated", "sb_listener.control")}), false};
    return false;
  }

  *decoded = DecodeControlFrame(bytes);
  return decoded->ok;
}

bool ReadControlFrame(std::intptr_t fd,
                      ListenerControlDecodeResult* decoded,
                      int* received_fd,
                      std::uint32_t timeout_ms) {
  std::intptr_t native_received = -1;
  const bool ok = ReadControlFrame(fd, decoded, &native_received, timeout_ms);
  if (received_fd != nullptr) *received_fd = static_cast<int>(native_received);
  return ok;
}
#else
bool SendControlFrame(int fd, const ListenerControlFrame& frame, int fd_to_transfer) {
  const auto encoded = EncodeControlFrame(frame);
  if (encoded.empty()) return false;
  if (fd_to_transfer < 0) {
    return WriteAll(fd, encoded.data(), encoded.size());
  }

  iovec iov{};
  iov.iov_base = const_cast<std::uint8_t*>(encoded.data());
  iov.iov_len = encoded.size();

  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  auto* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &fd_to_transfer, sizeof(int));
  msg.msg_controllen = CMSG_SPACE(sizeof(int));

  for (;;) {
    const ssize_t rc = ::sendmsg(fd, &msg, 0);
    if (rc == static_cast<ssize_t>(encoded.size())) return true;
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
}

bool ReadControlFrame(int fd,
                      ListenerControlDecodeResult* decoded,
                      int* received_fd,
                      std::uint32_t timeout_ms) {
  if (received_fd != nullptr) *received_fd = -1;
  if (decoded == nullptr) return false;
  if (!WaitReadable(fd, timeout_ms)) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.READ_TIMEOUT", "ERROR", "control-plane read timed out", "sb_listener.control")}), false};
    return false;
  }

  std::vector<std::uint8_t> bytes(kSbctHeaderSize);
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  iovec iov{};
  iov.iov_base = bytes.data();
  iov.iov_len = bytes.size();
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  ssize_t rc = -1;
  for (;;) {
    rc = ::recvmsg(fd, &msg, MSG_WAITALL);
    if (rc >= 0 || errno != EINTR) break;
  }
  if (rc != static_cast<ssize_t>(kSbctHeaderSize)) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.FRAME_TRUNCATED", "ERROR", "control-plane header is truncated", "sb_listener.control")}), false};
    return false;
  }

  for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && received_fd != nullptr) {
      std::memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));
    }
  }

  const auto payload_len = ReadU64(bytes, 20);
  if (payload_len > kSbctMaxPayloadSize) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.PAYLOAD_TOO_LARGE", "ERROR", "control-plane payload is too large", "sb_listener.control")}), false};
    return false;
  }
  bytes.resize(kSbctHeaderSize + static_cast<std::size_t>(payload_len));
  if (payload_len != 0 && !ReadAll(fd, bytes.data() + kSbctHeaderSize, static_cast<std::size_t>(payload_len))) {
    *decoded = {{}, MakeMessageVectorSet({MakeDiagnostic("CONTROL.FRAME_TRUNCATED", "ERROR", "control-plane payload is truncated", "sb_listener.control")}), false};
    return false;
  }

  *decoded = DecodeControlFrame(bytes);
  return decoded->ok;
}
#endif

} // namespace scratchbird::listener
