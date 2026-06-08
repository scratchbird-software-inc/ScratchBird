// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_runtime.hpp"

#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "control_plane.hpp"
#include "dbbt_lpreface.hpp"
#include "handoff_claim_reader.hpp"
#include "sbps_preauth.hpp"

namespace scratchbird::listener {
namespace {

std::array<std::uint8_t, 16> First16(const std::array<std::uint8_t, 32>& value) {
  std::array<std::uint8_t, 16> out{};
  std::copy_n(value.begin(), out.size(), out.begin());
  return out;
}

bool IsSbsqlManagerProfile(std::string_view profile) {
  return profile.empty() || profile == "SBsql" || profile == "native_v3";
}

std::string ErrnoString() {
  return std::strerror(errno);
}

std::string SocketErrorString(int error_code) {
#ifdef _WIN32
  return "WSA error " + std::to_string(error_code);
#else
  return std::strerror(error_code);
#endif
}

bool ValidLprefaceNonceLength(const proto::Bytes& nonce) {
  return nonce.size() >= 16u && nonce.size() <= 32u;
}

std::string UpperCommand(std::string value) {
  for (char& ch : value) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

#ifdef _WIN32
bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}

void CloseFd(ListenerRuntimeSocketHandle fd) {
  if (fd >= 0) {
    ::closesocket(static_cast<SOCKET>(fd));
  }
}
#else
void CloseFd(ListenerRuntimeSocketHandle fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}
#endif

std::string ClientAddrString(const sockaddr_storage& addr) {
  if (addr.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&addr);
    char buffer[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) == nullptr) {
      return "0.0.0.0";
    }
    return buffer;
  }
  if (addr.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    char buffer[INET6_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer)) == nullptr) {
      return "::";
    }
    return buffer;
  }
  return "unknown";
}

std::uint16_t ClientPort(const sockaddr_storage& addr) {
  if (addr.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&addr);
    return ntohs(ipv4->sin_port);
  }
  if (addr.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    return ntohs(ipv6->sin6_port);
  }
  return 0;
}

bool RequiresDualStack(const std::string& bind_address) {
  return bind_address.empty() || bind_address == "::";
}

int SendFlagsNoSignal() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

bool WaitForReadableSocket(ListenerRuntimeSocketHandle fd, std::uint32_t timeout_ms) {
#ifdef _WIN32
  fd_set read_set;
  FD_ZERO(&read_set);
  SOCKET socket = static_cast<SOCKET>(fd);
  FD_SET(socket, &read_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
  const int rc = ::select(0, &read_set, nullptr, nullptr, &timeout);
  return rc > 0 && FD_ISSET(socket, &read_set);
#else
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
#endif
}

int LastSocketErrorCode() {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

std::string GetAddrInfoErrorString(int gai_error) {
#if !defined(_WIN32) && defined(EAI_SYSTEM)
  if (gai_error == EAI_SYSTEM) return SocketErrorString(LastSocketErrorCode());
#endif
  return ::gai_strerror(gai_error);
}

int SendSocket(ListenerRuntimeSocketHandle fd, const char* data, std::size_t size) {
#ifdef _WIN32
  return ::send(static_cast<SOCKET>(fd),
                data,
                static_cast<int>(std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                0);
#else
  return static_cast<int>(::send(fd, data, size, SendFlagsNoSignal()));
#endif
}

int RecvPeekSocket(ListenerRuntimeSocketHandle fd, std::uint8_t* data, std::size_t size) {
#ifdef _WIN32
  return ::recv(static_cast<SOCKET>(fd),
                reinterpret_cast<char*>(data),
                static_cast<int>(std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                MSG_PEEK);
#else
  return static_cast<int>(::recv(fd, data, size, MSG_PEEK));
#endif
}

std::vector<std::string> SplitCommandWords(const std::string& command) {
  std::istringstream in(command);
  std::vector<std::string> words;
  std::string word;
  while (in >> word) {
    words.push_back(std::move(word));
  }
  return words;
}

bool ParseU32Text(const std::string& value, std::uint32_t* out) {
  if (out == nullptr || value.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  *out = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseU64Text(const std::string& value, std::uint64_t* out) {
  if (out == nullptr || value.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') {
    return false;
  }
  *out = static_cast<std::uint64_t>(parsed);
  return true;
}

proto::MessageVectorSet KillArgumentInvalid(const std::string& command) {
  return MakeMessageVectorSet({MakeDiagnostic("LISTENER.POOL.KILL_ARGUMENT_INVALID",
                                              "ERROR",
                                              "KILL requires connection_id=<id>; POOL_KILL also accepts worker_id=<id>",
                                              "sb_listener.pool",
                                              {{"command", command}})});
}

std::string FirstDiagnosticCode(const proto::MessageVectorSet& messages) {
  if (messages.diagnostics.empty()) return "OK";
  return messages.diagnostics.front().code;
}

void WriteDrainRefusalDiagnostic(ListenerRuntimeSocketHandle client_fd) {
  const auto messages = MakeMessageVectorSet({MakeDiagnostic("LISTENER.POOL_DRAIN_REJECT",
                                                            "ERROR",
                                                            "listener is draining and refuses new client connections",
                                                            "sb_listener.accept",
                                                            {{"retryable", "true"}, {"finality", "none"}})});
  const std::string line = MessageVectorSetJson(messages) + "\n";
  std::size_t sent = 0;
  while (sent < line.size()) {
    const auto rc = SendSocket(client_fd, line.data() + sent, line.size() - sent);
    if (rc > 0) {
      sent += static_cast<std::size_t>(rc);
      continue;
    }
#ifndef _WIN32
    if (rc < 0 && errno == EINTR) continue;
#endif
    break;
  }
}

std::uint32_t EffectiveAcceptRateBurst(const ListenerConfig& config) {
  if (config.accept_rate_limit_per_second == 0) return 0;
  if (config.accept_rate_limit_burst != 0) return config.accept_rate_limit_burst;
  const auto doubled = static_cast<std::uint64_t>(config.accept_rate_limit_per_second) * 2u;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(doubled, std::numeric_limits<std::uint32_t>::max()));
}

bool LooksLikeTlsClientHello(ListenerRuntimeSocketHandle fd, std::uint32_t timeout_ms) {
  if (!WaitForReadableSocket(fd, timeout_ms)) return false;
  std::array<std::uint8_t, 5> header{};
  const int got = RecvPeekSocket(fd, header.data(), header.size());
  if (got < 3) return false;
  if (header[0] != 0x16) return false;  // TLS handshake record.
  if (header[1] != 0x03) return false;  // SSLv3/TLS record-version family.
  return header[2] >= 0x01 && header[2] <= 0x04;
}

bool LifecycleTransitionAllowed(std::string_view from, std::string_view to) {
  if (from == "created") return to == "starting" || to == "failed";
  if (from == "starting") return to == "running" || to == "failed" || to == "stopped";
  if (from == "running") return to == "draining" || to == "stopped" || to == "failed";
  if (from == "draining") return to == "running" || to == "stopped" || to == "failed";
  if (from == "failed") return to == "stopped";
  if (from == "stopped") return false;
  return false;
}

std::string_view AcceptLoopStageName(AcceptLoopStage stage) {
  switch (stage) {
    case AcceptLoopStage::kIdle: return "idle";
    case AcceptLoopStage::kAccepted: return "accepted";
    case AcceptLoopStage::kMetricsRecorded: return "metrics_recorded";
    case AcceptLoopStage::kAcquireBegin: return "acquire_begin";
    case AcceptLoopStage::kWorkerAcquired: return "worker_acquired";
    case AcceptLoopStage::kHandoffBegin: return "handoff_begin";
    case AcceptLoopStage::kHandoffComplete: return "handoff_complete";
    case AcceptLoopStage::kClientClosed: return "client_closed";
  }
  return "unknown";
}

bool HasClientEvidence(const ParserHandoffClientEvidence& evidence) {
  return !evidence.client_nonce.empty() && !evidence.server_nonce.empty();
}

bool BytesEqualConstantTime(const proto::Bytes& lhs, const proto::Bytes& rhs) {
  if (lhs.size() != rhs.size()) return false;
  if (lhs.empty()) return true;
  return proto::ConstantTimeEqual(lhs.data(), rhs.data(), lhs.size());
}

bool HandoffBindingRequiresEvidence(const ParserHandoffBinding& binding) {
  return ValidLprefaceNonceLength(binding.client_nonce) &&
         ValidLprefaceNonceLength(binding.server_nonce);
}

bool HandoffBindingMatchesEvidence(const ParserHandoffBinding& binding,
                                   const ParserHandoffClientEvidence& evidence) {
  if (!binding.present || !HandoffBindingRequiresEvidence(binding)) return false;
  if (!HasClientEvidence(evidence)) return false;
  if (!BytesEqualConstantTime(binding.client_nonce, evidence.client_nonce) ||
      !BytesEqualConstantTime(binding.server_nonce, evidence.server_nonce)) {
    return false;
  }
  if (binding.has_expected_client_endpoint &&
      (!evidence.has_client_endpoint ||
       binding.expected_client_addr != evidence.client_addr ||
       binding.expected_client_port != evidence.client_port)) {
    return false;
  }
  return true;
}

std::uint32_t ManagementRequiredRight(std::string_view operation) {
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

std::uint32_t ManagementRoleRights(std::string_view role) {
  if (role == "reader" || role == "monitor") return kListenerManagementRightRead;
  if (role == "operator") {
    return kListenerManagementRightRead |
           kListenerManagementRightLifecycle |
           kListenerManagementRightPool;
  }
  if (role == "manager" || role == "owner") return kListenerManagementRightsAll;
  return 0;
}

bool ManagementAuthorityClassValid(std::string_view role,
                                   std::string_view authority_class) {
  if (authority_class == "listener_manager") {
    return role == "manager" || role == "owner" || role == "operator" ||
           role == "reader" || role == "monitor";
  }
  if (authority_class == "listener_operator") {
    return role == "operator";
  }
  if (authority_class == "listener_reader") {
    return role == "reader" || role == "monitor";
  }
  return false;
}

const std::string* ManagementArg(const ListenerManagementEnvelope& envelope,
                                 std::string_view key) {
  for (const auto& arg : envelope.arguments) {
    if (arg.key == key) return &arg.value;
  }
  return nullptr;
}

std::string ManagementEnvelopeCommandText(const ListenerManagementEnvelope& envelope) {
  if (envelope.operation == "PING" ||
      envelope.operation == "STATUS" ||
      envelope.operation == "HEALTH" ||
      envelope.operation == "POOL_STATUS" ||
      envelope.operation == "SUPPORT_BUNDLE" ||
      envelope.operation == "DRAIN" ||
      envelope.operation == "UNDRAIN" ||
      envelope.operation == "RELOAD") {
    return envelope.operation;
  }
  if (envelope.operation == "STOP") {
    const auto* force = ManagementArg(envelope, "force");
    return force != nullptr && *force == "true" ? "STOP FORCE" : "STOP GRACEFUL";
  }
  if (envelope.operation == "POOL_RESIZE") {
    const auto* min_workers = ManagementArg(envelope, "min_workers");
    const auto* max_workers = ManagementArg(envelope, "max_workers");
    if (min_workers == nullptr || max_workers == nullptr) return "POOL_RESIZE";
    return "POOL_RESIZE " + *min_workers + " " + *max_workers;
  }
  if (envelope.operation == "POOL_RECYCLE") {
    const auto* worker_id = ManagementArg(envelope, "worker_id");
    return worker_id == nullptr ? "POOL_RECYCLE" : "POOL_RECYCLE " + *worker_id;
  }
  if (envelope.operation == "POOL_KILL") {
    if (const auto* worker_id = ManagementArg(envelope, "worker_id")) {
      return "POOL_KILL worker_id=" + *worker_id;
    }
    if (const auto* connection_id = ManagementArg(envelope, "connection_id")) {
      return "POOL_KILL connection_id=" + *connection_id;
    }
    return "POOL_KILL";
  }
  if (envelope.operation == "POOL_RESTART") {
    return "POOL RESTART";
  }
  if (envelope.operation == "DBBT_VALIDATE") {
    const auto* token_hex = ManagementArg(envelope, "token_hex");
    return token_hex == nullptr ? "DBBT_VALIDATE" : "DBBT_VALIDATE " + *token_hex;
  }
  if (envelope.operation == "LPREFACE_VALIDATE") {
    const auto* preface_hex = ManagementArg(envelope, "preface_hex");
    return preface_hex == nullptr ? "LPREFACE_VALIDATE" : "LPREFACE_VALIDATE " + *preface_hex;
  }
  return envelope.operation;
}

ListenerRuntimeResult EnvelopeRequiredResult(const std::string& command) {
  return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT.ENVELOPE_REQUIRED",
                                                  "ERROR",
                                                  "privileged listener management command requires a structured SBME envelope",
                                                  "sb_listener.management",
                                                  {{"command", command}})})};
}

proto::Sha256Digest ManagementReplayEvidenceId(const ListenerManagementEnvelope& envelope,
                                               const std::vector<std::uint8_t>& encoded_payload) {
  proto::Bytes body;
  const auto payload_digest = proto::Sha256(encoded_payload);
  const auto append_text = [&](std::string_view text) {
    body.insert(body.end(), text.begin(), text.end());
    body.push_back(0);
  };
  append_text("listener-management-replay-v1");
  append_text(envelope.request_id);
  body.insert(body.end(), envelope.nonce.begin(), envelope.nonce.end());
  body.push_back(0);
  append_text(envelope.authenticator_scheme);
  body.insert(body.end(), envelope.authenticator.begin(), envelope.authenticator.end());
  body.push_back(0);
  body.insert(body.end(), payload_digest.begin(), payload_digest.end());
  return proto::Sha256(body);
}

bool VerifyPeerOwnerEvidence(ListenerRuntimeSocketHandle peer_fd,
                             proto::MessageVectorSet* messages) {
#if !defined(_WIN32) && defined(SO_PEERCRED)
  if (peer_fd < 0) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_UNAVAILABLE",
                                                     "ERROR",
                                                     "peer-owner management authentication requires a live Unix peer socket",
                                                     "sb_listener.management"));
    }
    return false;
  }
  struct ucred cred {};
  socklen_t len = sizeof(cred);
  if (::getsockopt(peer_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
      len != sizeof(cred)) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_UNAVAILABLE",
                                                     "ERROR",
                                                     "Unix peer credentials were unavailable for management authentication",
                                                     "sb_listener.management",
                                                     {{"error", ErrnoString()}}));
    }
    return false;
  }
  const uid_t owner_uid = ::getuid();
  if (cred.uid != owner_uid) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_INVALID",
                                                     "ERROR",
                                                     "management peer owner does not match listener owner",
                                                     "sb_listener.management",
                                                     {{"peer_uid", std::to_string(static_cast<unsigned long long>(cred.uid))},
                                                      {"owner_uid", std::to_string(static_cast<unsigned long long>(owner_uid))},
                                                      {"peer_pid", std::to_string(static_cast<long long>(cred.pid))}}));
    }
    return false;
  }
  return true;
#elif !defined(_WIN32) && (defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__))
  if (peer_fd < 0) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_UNAVAILABLE",
                                                     "ERROR",
                                                     "peer-owner management authentication requires a live Unix peer socket",
                                                     "sb_listener.management"));
    }
    return false;
  }
  uid_t peer_uid = 0;
  gid_t peer_gid = 0;
  if (::getpeereid(peer_fd, &peer_uid, &peer_gid) != 0) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_UNAVAILABLE",
                                                     "ERROR",
                                                     "BSD Unix peer credentials were unavailable for management authentication",
                                                     "sb_listener.management",
                                                     {{"error", ErrnoString()}}));
    }
    return false;
  }
  const uid_t owner_uid = ::getuid();
  if (peer_uid != owner_uid) {
    if (messages != nullptr) {
      messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_INVALID",
                                                     "ERROR",
                                                     "management peer owner does not match listener owner",
                                                     "sb_listener.management",
                                                     {{"peer_uid", std::to_string(static_cast<unsigned long long>(peer_uid))},
                                                      {"owner_uid", std::to_string(static_cast<unsigned long long>(owner_uid))},
                                                      {"peer_gid", std::to_string(static_cast<unsigned long long>(peer_gid))}}));
    }
    return false;
  }
  return true;
#else
  (void)peer_fd;
  if (messages != nullptr) {
    messages->diagnostics.push_back(MakeDiagnostic("LISTENER.MANAGEMENT.PEER_OWNER_UNAVAILABLE",
                                                   "ERROR",
                                                   "peer-owner management authentication is unavailable on this platform",
                                                   "sb_listener.management"));
  }
  return false;
#endif
}

} // namespace

ListenerRuntime::ListenerRuntime(ListenerConfig config)
    : config_(std::move(config)), identity_(BuildSocketIdentity(config_)), parser_pool_(config_, &metrics_) {}

ListenerRuntimeResult ListenerRuntime::Run() {
  if (config_.mode == ListenerMode::kHelp) {
    std::cout << HelpText();
    return {0, MakeMessageVectorSet({})};
  }
  if (config_.mode == ListenerMode::kVersion) {
    std::cout << VersionText();
    return {0, MakeMessageVectorSet({})};
  }
  if (config_.mode == ListenerMode::kValidateConfig) {
    return {0, MakeMessageVectorSet({})};
  }

  auto prep = PrepareRuntimeFiles();
  if (prep.exit_code != 0) {
    return prep;
  }

  DbbtKeyMaterial key;
  auto key_result = LoadDbbtKeyMaterial(config_, &key);
  if (!key_result.ok && (config_.managed_by_server || config_.managed_by_manager)) {
    return {2, key_result.messages};
  }

  parser_pool_.Start();
  metrics_.Increment("sys.metrics.listener.starts_total");
  metrics_.SetGauge("sys.metrics.listener.parser_pool.warm_min", static_cast<double>(config_.warm_pool_min));
  metrics_.SetGauge("sys.metrics.listener.parser_pool.warm_max", static_cast<double>(config_.warm_pool_max));

  ListenerRuntimeSocketHandle management_fd = -1;
  auto management_result = BindManagementSocket(&management_fd);
  if (management_result.exit_code != 0) {
    parser_pool_.Stop(true);
    return management_result;
  }

  ListenerRuntimeSocketHandle listen_fd = -1;
  auto bind_result = BindNetworkSocket(&listen_fd);
  if (bind_result.exit_code != 0) {
    parser_pool_.Stop(true);
    CloseFd(management_fd);
    return bind_result;
  }
  WriteLifecycleState("running");
  std::thread management_thread;
  if (management_fd >= 0) {
    management_thread = std::thread([this, management_fd] { ManagementLoop(management_fd); });
  }
  if (listen_fd >= 0 && config_.enable_accept_loop) {
    AcceptLoop(listen_fd);
  }
  CloseFd(listen_fd);
  CloseFd(management_fd);
  if (management_thread.joinable()) {
    management_thread.join();
  }
  parser_pool_.Stop(false);
  WriteLifecycleState("stopped");
  return {0, MakeMessageVectorSet({})};
}

ListenerRuntimeResult ListenerRuntime::PrepareRuntimeFiles() {
  std::error_code ec;
  std::filesystem::create_directories(config_.control_dir, ec);
  if (ec) {
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RUNTIME.CONTROL_DIR_FAILED", "ERROR", "control directory could not be created", "sb_listener.runtime", {{"path", config_.control_dir}, {"error", ec.message()}})})};
  }
  std::filesystem::create_directories(config_.runtime_dir, ec);
  if (ec) {
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RUNTIME.RUNTIME_DIR_FAILED", "ERROR", "runtime directory could not be created", "sb_listener.runtime", {{"path", config_.runtime_dir}, {"error", ec.message()}})})};
  }
  std::string owner_error;
  if (!WriteOwnerToken(identity_, &owner_error)) {
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RUNTIME.OWNER_TOKEN_FAILED", "ERROR", "owner token could not be written", "sb_listener.runtime", {{"error", owner_error}})})};
  }
  WriteLifecycleState("starting");
  return {0, MakeMessageVectorSet({})};
}

ListenerRuntimeResult ListenerRuntime::BindManagementSocket(ListenerRuntimeSocketHandle* out_fd) {
  *out_fd = -1;
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT_SOCKET_FAILED", "ERROR", "Winsock initialization failed for listener management socket", "sb_listener.management", {{"error", SocketErrorString(LastSocketErrorCode())}})})};
  }
#endif
  std::error_code ec;
  std::filesystem::remove(identity_.management_socket, ec);
#ifdef _WIN32
  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
#else
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
#endif
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT_SOCKET_FAILED", "ERROR", "management socket could not be created", "sb_listener.management", {{"error", SocketErrorString(LastSocketErrorCode())}})})};
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto path =
#ifdef _WIN32
      std::filesystem::absolute(identity_.management_socket).string();
#else
      identity_.management_socket.string();
#endif
  if (path.size() >= sizeof(addr.sun_path)) {
    CloseFd(fd);
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT_SOCKET_PATH_TOO_LONG", "ERROR", "management socket path is too long", "sb_listener.management", {{"path", path}})})};
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseFd(fd);
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT_SOCKET_BIND_FAILED", "ERROR", "management socket bind failed", "sb_listener.management", {{"path", path}, {"error", SocketErrorString(LastSocketErrorCode())}})})};
  }
  if (::listen(fd, 16) != 0) {
    CloseFd(fd);
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT_SOCKET_LISTEN_FAILED", "ERROR", "management socket listen failed", "sb_listener.management", {{"error", SocketErrorString(LastSocketErrorCode())}})})};
  }
  *out_fd = static_cast<ListenerRuntimeSocketHandle>(fd);
  metrics_.Increment("sys.metrics.listener.management_socket_binds_total");
  return {0, MakeMessageVectorSet({})};
}

ListenerRuntimeResult ListenerRuntime::BindNetworkSocket(ListenerRuntimeSocketHandle* out_fd) {
  *out_fd = -1;
  if (config_.port == 0) {
    metrics_.Increment("sys.metrics.listener.embedded_profile_no_socket_total");
    return {0, MakeMessageVectorSet({})};
  }
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RUNTIME.SOCKET_STACK_UNAVAILABLE", "ERROR", "Winsock initialization failed for listener network bind", "sb_listener.runtime", {{"error", SocketErrorString(LastSocketErrorCode())}})})};
  }
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(config_.port);
  const char* host = config_.bind_address.empty() ? nullptr : config_.bind_address.c_str();
  const int gai = ::getaddrinfo(host, service.c_str(), &hints, &resolved);
  if (gai != 0) {
    return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RUNTIME.INVALID_BIND_ADDRESS", "ERROR", "bind_address must resolve to an IPv4 or IPv6 TCP endpoint", "sb_listener.runtime", {{"bind_address", config_.bind_address}, {"error", GetAddrInfoErrorString(gai)}})})};
  }

  int last_errno = 0;
  std::string last_step = "resolve";
  const bool require_dual_stack = RequiresDualStack(config_.bind_address);
  for (addrinfo* ai = resolved; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
      continue;
    }
#ifdef _WIN32
    SOCKET fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == INVALID_SOCKET) {
#else
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
#endif
      last_errno = LastSocketErrorCode();
      last_step = "socket";
      continue;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    if (ai->ai_family == AF_INET6) {
      int v6only = require_dual_stack ? 0 : 1;
      if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) != 0) {
        last_errno = LastSocketErrorCode();
        last_step = require_dual_stack ? "dual_stack" : "ipv6_only";
        CloseFd(fd);
        continue;
      }
    }
    if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      last_errno = LastSocketErrorCode();
      last_step = "bind";
      CloseFd(fd);
      continue;
    }
    if (::listen(fd, static_cast<int>(config_.accept_backlog)) != 0) {
      last_errno = LastSocketErrorCode();
      last_step = "listen";
      CloseFd(fd);
      continue;
    }
    const bool bound_ipv6_dual_stack = ai->ai_family == AF_INET6 && require_dual_stack;
    ::freeaddrinfo(resolved);
    *out_fd = static_cast<ListenerRuntimeSocketHandle>(fd);
    metrics_.Increment("sys.metrics.listener.socket_binds_total");
    if (bound_ipv6_dual_stack) {
      metrics_.Increment("sys.metrics.listener.socket_binds_total.ipv6_dual_stack");
    }
    return {0, MakeMessageVectorSet({})};
  }
  ::freeaddrinfo(resolved);
  return {2, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RUNTIME.BIND_FAILED", "ERROR", "network bind failed", "sb_listener.runtime", {{"bind_address", config_.bind_address}, {"port", std::to_string(config_.port)}, {"step", last_step}, {"error", last_errno == 0 ? "no_supported_address" : SocketErrorString(last_errno)}})})};
}

void ListenerRuntime::AcceptLoop(ListenerRuntimeSocketHandle listen_fd) {
  accept_rate_tokens_ = static_cast<double>(EffectiveAcceptRateBurst(config_));
  accept_rate_last_refill_ = std::chrono::steady_clock::now();
  auto reject_connection = [&](ListenerRuntimeSocketHandle client_fd, const std::string& reason) {
    reject_total_.fetch_add(1, std::memory_order_relaxed);
    metrics_.Increment("sys.metrics.listener.network.reject_total." + reason);
    metrics_.Increment("scratchbird_listener_reject_total." + reason);
    if (reason == "rate_limited") {
      metrics_.Increment("sys.metrics.listener.network.rate_limit_drops_total");
      metrics_.Increment("scratchbird_listener_rate_limit_drops_total");
    }
    if (reason == "client_cap_exceeded") {
      metrics_.Increment("sys.metrics.listener.network.client_cap_drops_total");
      metrics_.Increment("scratchbird_listener_client_cap_drops_total");
    }
    if (reason == "draining" || reason == "draining_active") {
      WriteDrainRefusalDiagnostic(client_fd);
    }
    CloseFd(client_fd);
    open_connections_.fetch_sub(1, std::memory_order_relaxed);
    metrics_.SetGauge("sys.metrics.listener.network.open_connections", static_cast<double>(open_connections_.load()));
    last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kClientClosed), std::memory_order_release);
  };
  auto active_session_total = [&]() {
    std::uint64_t total = 0;
    for (const auto& entry : per_client_active_) total += entry.second;
    return total;
  };
  auto release_completed_parser_sessions = [&]() {
    for (const auto& client_addr : parser_pool_.CollectCompletedClientSessions()) {
      auto it = per_client_active_.find(client_addr);
      if (it != per_client_active_.end() && it->second > 0) {
        --it->second;
        if (it->second == 0) per_client_active_.erase(it);
      }
    }
    metrics_.SetGauge("sys.metrics.listener.network.per_client_active_sessions", static_cast<double>(active_session_total()));
  };
  auto release_active_for_client = [&](const std::string& client_addr) {
    auto it = per_client_active_.find(client_addr);
    if (it != per_client_active_.end() && it->second > 0) {
      --it->second;
      if (it->second == 0) per_client_active_.erase(it);
    }
    metrics_.SetGauge("sys.metrics.listener.network.per_client_active_sessions", static_cast<double>(active_session_total()));
  };
  auto rate_limit_allows = [&]() {
    if (config_.accept_rate_limit_per_second == 0) return true;
    const auto burst = EffectiveAcceptRateBurst(config_);
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - accept_rate_last_refill_;
    accept_rate_last_refill_ = now;
    accept_rate_tokens_ = std::min<double>(
        static_cast<double>(burst),
        accept_rate_tokens_ + (elapsed.count() * static_cast<double>(config_.accept_rate_limit_per_second)));
    if (accept_rate_tokens_ < 1.0) return false;
    accept_rate_tokens_ -= 1.0;
    return true;
  };
  while (!stop_requested_.load()) {
    release_completed_parser_sessions();
    last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kIdle), std::memory_order_release);
    if (!WaitForReadableSocket(listen_fd, config_.idle_poll_ms)) {
      continue;
    }
    {
      sockaddr_storage client{};
      socklen_t len = sizeof(client);
#ifdef _WIN32
      SOCKET accepted = ::accept(static_cast<SOCKET>(listen_fd), reinterpret_cast<sockaddr*>(&client), &len);
      ListenerRuntimeSocketHandle client_fd = accepted == INVALID_SOCKET ? -1 : static_cast<ListenerRuntimeSocketHandle>(accepted);
#else
      ListenerRuntimeSocketHandle client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client), &len);
#endif
      if (client_fd >= 0) {
        const auto connection_id = next_connection_id_.fetch_add(1, std::memory_order_relaxed);
        last_accept_sequence_.store(connection_id, std::memory_order_release);
        last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kAccepted), std::memory_order_release);
        open_connections_.fetch_add(1, std::memory_order_relaxed);
        metrics_.SetGauge("sys.metrics.listener.network.open_connections", static_cast<double>(open_connections_.load()));
        metrics_.Increment("sys.metrics.listener.network.connections_total");
        metrics_.Increment("sys.metrics.listener.network.accept_total");
        metrics_.Increment("scratchbird_listener_connections_total");
        metrics_.Increment("scratchbird_listener_accept_total");
        last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kMetricsRecorded), std::memory_order_release);
        const std::string client_addr = ClientAddrString(client);
        if (config_.tls_required && !LooksLikeTlsClientHello(client_fd, config_.preauth_timeout_ms)) {
          last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kAcquireBegin), std::memory_order_release);
          reject_connection(client_fd, "tls_required_plaintext");
          continue;
        }
        if (!rate_limit_allows()) {
          last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kAcquireBegin), std::memory_order_release);
          reject_connection(client_fd, "rate_limited");
          continue;
        }
        const bool had_existing_active_sessions = active_session_total() > 0;
        auto& active_for_client = per_client_active_[client_addr];
        if (config_.per_client_max_connections != 0 && active_for_client >= config_.per_client_max_connections) {
          last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kAcquireBegin), std::memory_order_release);
          reject_connection(client_fd, "client_cap_exceeded");
          continue;
        }
        active_for_client += 1;
        metrics_.SetGauge("sys.metrics.listener.network.per_client_active_sessions", static_cast<double>(active_session_total()));
        if (draining_.load()) {
          last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kAcquireBegin), std::memory_order_release);
          if (active_for_client > 0) --active_for_client;
          if (active_for_client == 0) per_client_active_.erase(client_addr);
          metrics_.SetGauge("sys.metrics.listener.network.per_client_active_sessions", static_cast<double>(active_session_total()));
          reject_connection(client_fd, had_existing_active_sessions ? "draining_active" : "draining");
          continue;
        }
        last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kAcquireBegin), std::memory_order_release);
        queue_depth_.fetch_add(1, std::memory_order_relaxed);
        metrics_.SetGauge("sys.metrics.listener.queue.depth", static_cast<double>(queue_depth_.load()));
        ParserHandoffClientEvidence evidence;
        evidence.has_client_endpoint = true;
        evidence.client_addr = client_addr;
        evidence.client_port = ClientPort(client);
#ifndef _WIN32
        if (PendingHandoffBindingCount() != 0) {
          auto claim = ReadOptionalLprefaceHandoffClaimFromSocket(client_fd, evidence, config_.preauth_timeout_ms);
          evidence = std::move(claim.evidence);
          if (claim.recognized && claim.malformed) {
            metrics_.Increment("sys.metrics.listener.handoff_binding.claim_invalid_total");
            if (!claim.diagnostic_code.empty()) {
              metrics_.Increment("sys.metrics.listener.handoff_binding.claim_invalid." + claim.diagnostic_code);
            }
            release_active_for_client(client_addr);
            reject_connection(client_fd, "lpreface_claim_invalid");
            queue_depth_.fetch_sub(1, std::memory_order_relaxed);
            metrics_.SetGauge("sys.metrics.listener.queue.depth", static_cast<double>(queue_depth_.load()));
            continue;
          }
          if (claim.recognized && !claim.consumed) {
            metrics_.Increment("sys.metrics.listener.handoff_binding.claim_consume_failed_total");
            release_active_for_client(client_addr);
            reject_connection(client_fd, "lpreface_claim_consume_failed");
            queue_depth_.fetch_sub(1, std::memory_order_relaxed);
            metrics_.SetGauge("sys.metrics.listener.queue.depth", static_cast<double>(queue_depth_.load()));
            continue;
          }
          if (claim.consumed) {
            metrics_.Increment("sys.metrics.listener.handoff_binding.claim_consumed_total");
          }
        }
#endif
        ParserHandoffBinding handoff_binding = TakePendingHandoffBinding(evidence);
        const auto handoff = parser_pool_.HandoffClient(client_fd,
                                                        client_addr,
                                                        evidence.client_port,
                                                        connection_id,
                                                        handoff_binding);
        queue_depth_.fetch_sub(1, std::memory_order_relaxed);
        metrics_.SetGauge("sys.metrics.listener.queue.depth", static_cast<double>(queue_depth_.load()));
        last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kWorkerAcquired), std::memory_order_release);
        last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kHandoffBegin), std::memory_order_release);
        CloseFd(client_fd);
        if (!handoff.ok) {
          auto fail_it = per_client_active_.find(client_addr);
          if (fail_it != per_client_active_.end() && fail_it->second > 0) {
            --fail_it->second;
            if (fail_it->second == 0) per_client_active_.erase(fail_it);
          }
          const std::string reason =
              handoff.reason == "no_idle_parser" ? "queue_full" :
              handoff.reason == "handoff_send_failed" ? "worker_acquired_but_handoff_failed" :
              handoff.reason == "handoff_ack_failed" ? "worker_acquired_but_handoff_failed" :
              handoff.reason == "handoff_rejected" ? "handoff_rejected_by_worker" :
              handoff.reason == "parser_error_message" ? "worker_acquired_but_handoff_failed" :
              handoff.reason.empty() ? "error" : handoff.reason;
          reject_total_.fetch_add(1, std::memory_order_relaxed);
          metrics_.Increment("sys.metrics.listener.network.reject_total." + reason);
          metrics_.Increment("scratchbird_listener_reject_total." + reason);
          RecordSupportEvent("handoff_failure",
                             "PARSER_HANDOFF",
                             "refused",
                             "LISTENER.HANDOFF_FAILURE",
                             "parser handoff failed for a redacted client endpoint");
        } else {
          handoff_complete_total_.fetch_add(1, std::memory_order_relaxed);
          metrics_.Increment("sys.metrics.listener.network.handoff_complete_total");
          metrics_.Increment("scratchbird_listener_handoff_complete_total");
          last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kHandoffComplete), std::memory_order_release);
        }
        metrics_.SetGauge("sys.metrics.listener.network.per_client_active_sessions", static_cast<double>(active_session_total()));
        open_connections_.fetch_sub(1, std::memory_order_relaxed);
        metrics_.SetGauge("sys.metrics.listener.network.open_connections", static_cast<double>(open_connections_.load()));
        last_accept_stage_.store(static_cast<std::uint32_t>(AcceptLoopStage::kClientClosed), std::memory_order_release);
      } else {
        metrics_.Increment("sys.metrics.listener.accept_errors_total");
        metrics_.Increment("sys.metrics.listener.network.reject_total.error");
        metrics_.Increment("scratchbird_listener_reject_total.error");
      }
    }
  }
}

void ListenerRuntime::ManagementLoop(ListenerRuntimeSocketHandle management_fd) {
  while (!stop_requested_.load()) {
#ifdef _WIN32
    if (!WaitForReadableSocket(management_fd, config_.management_poll_ms)) continue;
    const SOCKET accepted = ::accept(static_cast<SOCKET>(management_fd), nullptr, nullptr);
    const ListenerRuntimeSocketHandle client_fd =
        accepted == INVALID_SOCKET ? -1 : static_cast<ListenerRuntimeSocketHandle>(accepted);
#else
    if (!WaitForReadableSocket(management_fd, config_.management_poll_ms)) continue;
    const ListenerRuntimeSocketHandle client_fd = ::accept(management_fd, nullptr, nullptr);
#endif
    if (client_fd < 0) {
      metrics_.Increment("sys.metrics.listener.management_errors_total");
      continue;
    }
    ListenerControlDecodeResult decoded;
    ListenerRuntimeSocketHandle received_fd = -1;
    if (!ReadControlFrame(client_fd, &decoded, &received_fd, config_.preauth_timeout_ms)) {
      CloseFd(received_fd);
      CloseFd(client_fd);
      metrics_.Increment("sys.metrics.listener.management_decode_errors_total");
      continue;
    }
    CloseFd(received_fd);
    ListenerRuntimeResult handled;
    if (decoded.frame.opcode != ListenerControlOpcode::kManagementCommand) {
      handled = {1, MakeMessageVectorSet({MakeDiagnostic("CONTROL.MESSAGE_TYPE_INVALID", "ERROR", "listener management socket expects MANAGEMENT_COMMAND", "sb_listener.management")})};
    } else {
      handled = HandleManagementPayload(decoded.frame.payload, client_fd);
    }
    ListenerControlFrame response;
    response.opcode = ListenerControlOpcode::kManagementResponse;
    response.sequence = decoded.frame.sequence;
    std::string body = handled.response_json.empty()
                           ? MessageVectorSetJson(handled.messages)
                           : handled.response_json;
    if (handled.response_json.empty() && handled.messages.diagnostics.empty()) {
      body = "{\"ok\":true,\"status\":" + StatusJson() + "}";
    }
    response.payload.reserve(body.size() + 1);
    response.payload.push_back(handled.exit_code == 0 ? 0 : 1);
    response.payload.insert(response.payload.end(), body.begin(), body.end());
    SendControlFrame(client_fd, response);
    CloseFd(client_fd);
  }
}

ListenerRuntimeResult ListenerRuntime::HandleManagementPayload(const std::vector<std::uint8_t>& payload,
                                                               ListenerRuntimeSocketHandle peer_fd) {
  if (IsListenerManagementEnvelopePayload(payload)) {
    proto::MessageVectorSet messages;
    auto envelope = DecodeListenerManagementEnvelope(payload, &messages);
    if (!envelope) {
      return {1, std::move(messages)};
    }
    return HandleManagementEnvelope(*envelope, peer_fd, payload);
  }
  const std::string raw(payload.begin(), payload.end());
  return HandleManagementCommand(raw);
}

ListenerRuntimeResult ListenerRuntime::HandleManagementEnvelope(
    const ListenerManagementEnvelope& envelope,
    ListenerRuntimeSocketHandle peer_fd,
    const std::vector<std::uint8_t>& encoded_payload) {
  const auto fail = [&](std::string code,
                        std::string message,
                        std::vector<proto::Field> fields = {}) -> ListenerRuntimeResult {
    RecordSupportEvent("management_decision",
                       envelope.operation.empty() ? "UNKNOWN" : envelope.operation,
                       "refused",
                       code,
                       message);
    return {1, MakeMessageVectorSet({MakeDiagnostic(std::move(code),
                                                    "ERROR",
                                                    std::move(message),
                                                    "sb_listener.management",
                                                    std::move(fields))},
                                    config_.language,
                                    config_.dialect)};
  };

  if (envelope.version != 1) {
    return fail("LISTENER.MANAGEMENT.ENVELOPE_VERSION_UNSUPPORTED",
                "management envelope version is not supported",
                {{"version", std::to_string(envelope.version)}});
  }
  if (envelope.operation.empty() || envelope.request_id.empty() || envelope.nonce.empty()) {
    return fail("LISTENER.MANAGEMENT.ENVELOPE_MALFORMED",
                "management envelope is missing operation, request id, or nonce");
  }
  const auto now_ms = proto::CurrentEpochMilliseconds();
  constexpr std::uint64_t kManagementClockSkewMs = 2000;
  if (envelope.expires_at_ms == 0 || envelope.expires_at_ms <= envelope.issued_at_ms) {
    return fail("LISTENER.MANAGEMENT.WINDOW_INVALID",
                "management envelope time window is invalid",
                {{"request_id", envelope.request_id}});
  }
  if (envelope.issued_at_ms > now_ms + kManagementClockSkewMs) {
    return fail("LISTENER.MANAGEMENT.WINDOW_NOT_YET_VALID",
                "management envelope is not yet valid",
                {{"request_id", envelope.request_id}});
  }
  if (envelope.expires_at_ms + kManagementClockSkewMs < now_ms) {
    return fail("LISTENER.MANAGEMENT.WINDOW_EXPIRED",
                "management envelope has expired",
                {{"request_id", envelope.request_id}});
  }
  const std::uint32_t required_right = ManagementRequiredRight(envelope.operation);
  if (required_right == 0) {
    return fail("LISTENER.MANAGEMENT.UNKNOWN_COMMAND",
                "management envelope operation is not recognized",
                {{"operation", envelope.operation}});
  }
  const std::uint32_t role_rights = ManagementRoleRights(envelope.role);
  if (role_rights == 0 ||
      !ManagementAuthorityClassValid(envelope.role, envelope.authority_class)) {
    return fail("LISTENER.MANAGEMENT.ROLE_UNKNOWN",
                "management envelope role or authority class is not recognized",
                {{"role", envelope.role}, {"authority_class", envelope.authority_class}});
  }
  if (envelope.rights == 0 ||
      (envelope.rights & ~kListenerManagementRightsAll) != 0 ||
      (envelope.rights & ~role_rights) != 0) {
    return fail("LISTENER.MANAGEMENT.RIGHTS_INVALID",
                "management envelope rights are invalid for the role",
                {{"role", envelope.role}, {"rights", std::to_string(envelope.rights)}});
  }
  if ((envelope.rights & required_right) == 0 ||
      (role_rights & required_right) == 0) {
    return fail("LISTENER.MANAGEMENT.RIGHT_DENIED",
                "management envelope lacks the command-specific right",
                {{"operation", envelope.operation},
                 {"role", envelope.role},
                 {"required_right", std::to_string(required_right)}});
  }

  proto::MessageVectorSet auth_messages = MakeMessageVectorSet({}, config_.language, config_.dialect);
  if (envelope.authenticator_scheme == kListenerManagementAuthPeerOwner) {
    if (!VerifyPeerOwnerEvidence(peer_fd, &auth_messages)) {
      return {1, std::move(auth_messages)};
    }
  } else if (envelope.authenticator_scheme == kListenerManagementAuthHmacSha256) {
    DbbtKeyMaterial key;
    auto key_result = LoadDbbtKeyMaterial(config_, &key);
    if (!key_result.ok) return {1, key_result.messages};
    if (envelope.authenticator.size() != 32) {
      return fail("LISTENER.MANAGEMENT.AUTHENTICATOR_MALFORMED",
                  "management envelope HMAC authenticator length is invalid",
                  {{"request_id", envelope.request_id}});
    }
    auto expected_envelope = envelope;
    expected_envelope.authenticator.clear();
    expected_envelope.authenticator_scheme = kListenerManagementAuthHmacSha256;
    const auto signing_body = ListenerManagementEnvelopeSigningBody(expected_envelope);
    const auto expected = proto::HmacSha256(key.bytes, signing_body);
    if (!proto::ConstantTimeEqual(expected.data(),
                                  envelope.authenticator.data(),
                                  expected.size())) {
      return fail("LISTENER.MANAGEMENT.AUTHENTICATOR_INVALID",
                  "management envelope authenticator verification failed",
                  {{"request_id", envelope.request_id}});
    }
  } else {
    return fail("LISTENER.MANAGEMENT.AUTHENTICATOR_SCHEME_UNKNOWN",
                "management envelope authenticator scheme is not recognized",
                {{"authenticator_scheme", envelope.authenticator_scheme}});
  }

  const auto replay_id = ManagementReplayEvidenceId(envelope, encoded_payload);
  if (!management_replay_cache_.CheckAndInsert(replay_id, envelope.expires_at_ms, now_ms)) {
    return fail("LISTENER.MANAGEMENT.REPLAY_DETECTED",
                "management envelope replay was detected",
                {{"request_id", envelope.request_id}});
  }
  return ExecuteManagementOperation(envelope);
}

ListenerRuntimeResult ListenerRuntime::ExecuteManagementOperation(
    const ListenerManagementEnvelope& envelope) {
  auto result = ExecuteManagementCommandText(ManagementEnvelopeCommandText(envelope));
  const auto outcome = result.exit_code == 0 ? "accepted" : "refused";
  const auto diagnostic_code = FirstDiagnosticCode(result.messages);
  RecordSupportEvent("management_decision",
                     envelope.operation,
                     outcome,
                     diagnostic_code,
                     result.exit_code == 0 ? "management operation completed"
                                           : "management operation refused");
  if ((envelope.operation == "DBBT_VALIDATE" ||
       envelope.operation == "LPREFACE_VALIDATE") &&
      result.exit_code != 0) {
    RecordSupportEvent("auth_refusal",
                       envelope.operation,
                       "refused",
                       diagnostic_code,
                       "parser handoff authentication evidence was refused");
  }
  return result;
}

ListenerRuntimeResult ListenerRuntime::HandleManagementCommand(const std::string& command_json) {
  const std::string command = UpperCommand(command_json);
  if (command == "PING" || command == "STATUS" ||
      command == "POOL_STATUS" || command == "POOL STATUS" ||
      command == "HEALTH") {
    return ExecuteManagementCommandText(command_json);
  }
  auto result = EnvelopeRequiredResult(command_json);
  RecordSupportEvent("management_decision",
                     "RAW_COMMAND",
                     "refused",
                     FirstDiagnosticCode(result.messages),
                     "raw privileged management command refused");
  return result;
}

ListenerRuntimeResult ListenerRuntime::ExecuteManagementCommandText(const std::string& command_json) {
  const std::string command = UpperCommand(command_json);
  const auto raw_words = SplitCommandWords(command_json);
  if (command == "PING") {
    return {0, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT.PONG", "INFO", "listener management ping succeeded", "sb_listener.management")})};
  }
  if (command == "STATUS") {
    return {0, MakeMessageVectorSet({})};
  }
  if (command == "DRAIN") {
    draining_.store(true);
    WriteLifecycleState("draining");
    return {0, parser_pool_.Drain()};
  }
  if (command == "UNDRAIN") {
    draining_.store(false);
    WriteLifecycleState("running");
    return {0, parser_pool_.Undrain()};
  }
  if (command == "STOP" || command == "STOP GRACEFUL") {
    draining_.store(true);
    WriteLifecycleState("draining");
    auto drained = parser_pool_.DrainAndWait(config_.graceful_drain_timeout_ms);
    if (!drained.drained) {
      metrics_.Increment("sys.metrics.listener.graceful_stop_timeouts_total");
      return {1, std::move(drained.messages)};
    }
    RequestStop();
    auto stopped = parser_pool_.Stop(false);
    if (!stopped.diagnostics.empty()) return {1, std::move(stopped)};
    return {0, std::move(drained.messages)};
  }
  if (command == "STOP FORCE") {
    RequestStop();
    return {0, parser_pool_.Stop(true)};
  }
  if (command == "POOL_STATUS" || command == "POOL STATUS") {
    return {0, MakeMessageVectorSet({})};
  }
  if (command == "SUPPORT_BUNDLE" || command == "SUPPORT BUNDLE") {
    metrics_.Increment("sys.metrics.listener.support_bundle.export_total");
    auto messages = MakeMessageVectorSet(
        {MakeDiagnostic("LISTENER.SUPPORT_BUNDLE.EXPORTED",
                        "INFO",
                        "listener support bundle exported",
                        "sb_listener.support_bundle",
                        {{"schema", "SB_LISTENER_SUPPORT_BUNDLE_V1"},
                         {"redaction_profile", "listener.support_bundle.default_redaction.v1"}})},
        config_.language,
        config_.dialect);
    return {0, std::move(messages), BuildSupportBundleJson()};
  }
  if (command == "HEALTH") {
    return {0, MakeMessageVectorSet({MakeDiagnostic("LISTENER.HEALTH.OK", "INFO", "listener health check succeeded", "sb_listener.management")})};
  }
  if (command == "RELOAD") {
    draining_.store(false);
    WriteLifecycleState("running");
    metrics_.Increment("sys.metrics.listener.reload_requests_total");
    return {0, MakeMessageVectorSet({MakeDiagnostic("LISTENER.RELOAD_ACCEPTED", "INFO", "listener reload request accepted", "sb_listener.management")})};
  }
  if (command.rfind("POOL_RESIZE ", 0) == 0 || command.rfind("POOL RESIZE ", 0) == 0) {
    const auto words = SplitCommandWords(command);
    const std::size_t first_arg = (words.size() == 4 && words[0] == "POOL" && words[1] == "RESIZE") ? 2 : 1;
    if (words.size() != first_arg + 2) {
      return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.POOL.INVALID_RESIZE", "ERROR", "POOL_RESIZE requires min and max worker counts", "sb_listener.pool")})};
    }
    std::uint32_t min_workers = 0;
    std::uint32_t max_workers = 0;
    if (!ParseU32Text(words[first_arg], &min_workers) || !ParseU32Text(words[first_arg + 1], &max_workers)) {
      return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.POOL.INVALID_RESIZE", "ERROR", "POOL_RESIZE worker counts must be unsigned integers", "sb_listener.pool")})};
    }
    auto resized = parser_pool_.Resize(min_workers, max_workers);
    return {resized.diagnostics.empty() ? 0 : 1, std::move(resized)};
  }
  if (command.rfind("POOL_RECYCLE ", 0) == 0 || command.rfind("POOL RECYCLE ", 0) == 0) {
    const auto words = SplitCommandWords(command);
    const std::size_t worker_arg = (words.size() == 3 && words[0] == "POOL" && words[1] == "RECYCLE") ? 2 : 1;
    if (words.size() != worker_arg + 1 || raw_words.size() != words.size()) {
      return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.POOL.WORKER_NOT_FOUND", "ERROR", "POOL_RECYCLE requires a worker id", "sb_listener.pool")})};
    }
    auto restarted = parser_pool_.RestartWorker(raw_words[worker_arg]);
    return {restarted.diagnostics.empty() ? 0 : 1, std::move(restarted)};
  }
  if (command.rfind("KILL ", 0) == 0) {
    const auto words = SplitCommandWords(command);
    if (words.size() != 2) {
      return {1, KillArgumentInvalid(command_json)};
    }
    std::uint64_t connection_id = 0;
    if (!ParseU64Text(words[1], &connection_id)) {
      return {1, KillArgumentInvalid(command_json)};
    }
    auto killed = parser_pool_.KillConnection(connection_id);
    return {killed.diagnostics.empty() ? 0 : 1, std::move(killed)};
  }
  if (command.rfind("POOL_KILL ", 0) == 0 || command.rfind("POOL KILL ", 0) == 0) {
    const auto words = SplitCommandWords(command);
    const std::size_t kill_arg = (words.size() == 3 && words[0] == "POOL" && words[1] == "KILL") ? 2 : 1;
    if (words.size() != kill_arg + 1 || raw_words.size() != words.size()) {
      return {1, KillArgumentInvalid(command_json)};
    }
    const auto& normalized_arg = words[kill_arg];
    const auto& raw_arg = raw_words[kill_arg];
    if (normalized_arg.rfind("WORKER_ID=", 0) == 0) {
      const auto eq = raw_arg.find('=');
      if (eq == std::string::npos || eq + 1 >= raw_arg.size()) {
        return {1, KillArgumentInvalid(command_json)};
      }
      auto killed = parser_pool_.KillWorker(raw_arg.substr(eq + 1));
      return {killed.diagnostics.empty() ? 0 : 1, std::move(killed)};
    }
    std::string connection_text = raw_arg;
    if (normalized_arg.rfind("CONNECTION_ID=", 0) == 0) {
      const auto eq = raw_arg.find('=');
      if (eq == std::string::npos || eq + 1 >= raw_arg.size()) {
        return {1, KillArgumentInvalid(command_json)};
      }
      connection_text = raw_arg.substr(eq + 1);
    }
    std::uint64_t connection_id = 0;
    if (!ParseU64Text(connection_text, &connection_id)) {
      return {1, KillArgumentInvalid(command_json)};
    }
    auto killed = parser_pool_.KillConnection(connection_id);
    return {killed.diagnostics.empty() ? 0 : 1, std::move(killed)};
  }
  if (command == "POOL RESTART") {
    auto stop = parser_pool_.Stop(true);
    if (!stop.diagnostics.empty()) return {1, std::move(stop)};
    return {0, parser_pool_.Start()};
  }
  if (command.rfind("DBBT_VALIDATE ", 0) == 0) {
    const auto result = ValidateDbbtHexToken(config_,
                                             command_json.substr(std::string("DBBT_VALIDATE ").size()),
                                             &dbbt_replay_cache_);
    return {result.ok ? 0 : 1, result.messages};
  }
  if (command.rfind("LPREFACE_VALIDATE ", 0) == 0) {
    std::vector<proto::Diagnostic> diagnostics;
    auto preface = proto::DecodeLpreface(proto::FromHex(command_json.substr(std::string("LPREFACE_VALIDATE ").size())), &diagnostics);
    if (!preface) {
      return {1, MakeMessageVectorSet(std::move(diagnostics), config_.language, config_.dialect)};
    }
    if (!IsSbsqlManagerProfile(preface->requested_profile) ||
        config_.protocol_family != "sbsql") {
      return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.LPREFACE_PROFILE_UNSUPPORTED",
                                                       "ERROR",
                                                       "LPREFACE manager-mediated authentication is limited to SBsql listeners",
                                                       "sb_listener.management",
                                                       {{"requested_profile", preface->requested_profile},
                                                        {"protocol_family", config_.protocol_family}})},
                                      config_.language,
                                      config_.dialect)};
    }
    if (preface->auth_token.empty() || preface->auth_principal.empty()) {
      return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.LPREFACE_AUTH_TOKEN_REQUIRED",
                                                       "ERROR",
                                                       "LPREFACE manager-mediated authentication requires a security-database temporary token",
                                                       "sb_listener.management")},
                                      config_.language,
                                      config_.dialect)};
    }
    DbbtKeyMaterial key;
    auto key_result = LoadDbbtKeyMaterial(config_, &key);
    if (!key_result.ok) return {1, key_result.messages};
    proto::DbbtValidationOptions options;
    options.expected_listener_id = 1;
    options.now_ms = proto::CurrentEpochMilliseconds();
    proto::DbbtToken token;
    auto validation = proto::ValidateDbbt(preface->dbbt, key.bytes, options, &token);
    if (validation.ok) {
      if (!ValidLprefaceNonceLength(token.client_nonce) ||
          !ValidLprefaceNonceLength(token.server_nonce)) {
        validation.ok = false;
        validation.diagnostics.push_back(proto::MakeDiagnostic("MCP.DBBT_NONCE_LENGTH",
                                                               "DBBT LPREFACE handoff requires 16..32 byte client and server nonces."));
      }
    }
    if (validation.ok) {
      const auto token_id = proto::DbbtTokenId(token, key.bytes);
      if (!dbbt_replay_cache_.CheckAndInsert(token_id, token.expires_at_ms, options.now_ms)) {
        validation.ok = false;
        validation.diagnostics.push_back(proto::MakeDiagnostic("MCP.DBBT_REPLAY_DETECTED", "DBBT replay was detected."));
      }
    }
    proto::LprefaceAck ack;
    ack.accepted = validation.ok;
    ack.nack_code = validation.ok ? 0 : 1;
    ack.message = validation.ok ? "accepted" : "rejected";
    proto::Bytes encoded_ack;
    auto encoded = proto::EncodeLprefaceAck(ack, &encoded_ack);
    if (!encoded.ok) {
      return {1, MakeMessageVectorSet(std::move(encoded.diagnostics), config_.language, config_.dialect)};
    }
    if (!validation.ok) {
      return {1, MakeMessageVectorSet(std::move(validation.diagnostics), config_.language, config_.dialect)};
    }
    QueuePendingHandoffBinding(token, *preface);
    return {0, MakeMessageVectorSet({MakeDiagnostic("LISTENER.LPREFACE_ACCEPTED", "INFO", "LPREFACE validation accepted", "sb_listener.management", {{"ack_hex", proto::Hex(encoded_ack)}})})};
  }
  return {1, MakeMessageVectorSet({MakeDiagnostic("LISTENER.MANAGEMENT.UNKNOWN_COMMAND", "ERROR", "management command is not recognized", "sb_listener.management", {{"command", command_json}})})};
}

void ListenerRuntime::RequestStop() {
  stop_requested_.store(true);
}

void ListenerRuntime::WriteLifecycleState(const std::string& state) {
  std::string effective_state = state;
  if (!LifecycleTransitionAllowed(lifecycle_state_, state)) {
    metrics_.Increment("sys.metrics.listener.lifecycle_invalid_transitions_total");
    effective_state = "failed";
  }
  lifecycle_state_ = effective_state;
  std::string error;
  if (!WriteLifecycleStateToken(identity_,
                                effective_state,
                                state,
                                SocketIdentityJson(identity_),
                                parser_pool_.StatusJson(),
                                &error)) {
    metrics_.Increment("sys.metrics.listener.lifecycle_write_failures_total");
  }
}

std::string ListenerRuntime::StatusJson() const {
  const auto stage = static_cast<AcceptLoopStage>(last_accept_stage_.load(std::memory_order_acquire));
  std::ostringstream out;
  out << "{\"mode\":\"" << ListenerModeName(config_.mode) << "\","
      << "\"protocol_family\":\"" << QuoteJson(config_.protocol_family) << "\","
      << "\"database_selector\":\"" << QuoteJson(config_.database_selector) << "\","
      << "\"server_endpoint\":\"" << QuoteJson(config_.server_endpoint) << "\","
      << "\"lifecycle_state\":\"" << QuoteJson(lifecycle_state_) << "\","
      << "\"draining\":" << (draining_.load(std::memory_order_acquire) ? "true" : "false") << ','
      << "\"stop_requested\":" << (stop_requested_.load(std::memory_order_acquire) ? "true" : "false") << ','
      << "\"accepting_new_connections\":"
      << (!stop_requested_.load(std::memory_order_acquire) &&
          !draining_.load(std::memory_order_acquire) ? "true" : "false")
      << ','
      << "\"graceful_drain_timeout_ms\":" << config_.graceful_drain_timeout_ms << ','
      << "\"last_accept_sequence\":" << last_accept_sequence_.load(std::memory_order_acquire) << ','
      << "\"last_accept_stage\":\"" << AcceptLoopStageName(stage) << "\","
      << "\"open_connections\":" << open_connections_.load(std::memory_order_acquire) << ','
      << "\"queue_depth\":" << queue_depth_.load(std::memory_order_acquire) << ','
      << "\"pending_handoff_bindings\":" << PendingHandoffBindingCount() << ','
      << "\"handoff_complete_total\":" << handoff_complete_total_.load(std::memory_order_acquire) << ','
      << "\"reject_total\":" << reject_total_.load(std::memory_order_acquire) << ','
      << "\"identity\":" << SocketIdentityJson(identity_) << ','
      << "\"pool\":" << parser_pool_.StatusJson() << ','
      << "\"metrics\":" << metrics_.ToJson() << '}';
  return out.str();
}

std::string ListenerRuntime::BuildSupportBundleJson() const {
  ListenerSupportBundleSnapshot snapshot;
  snapshot.config = config_;
  snapshot.identity = identity_;
  snapshot.lifecycle_state = lifecycle_state_;
  snapshot.draining = draining_.load(std::memory_order_acquire);
  snapshot.stop_requested = stop_requested_.load(std::memory_order_acquire);
  snapshot.accepting_new_connections = !snapshot.stop_requested && !snapshot.draining;
  snapshot.last_accept_sequence = last_accept_sequence_.load(std::memory_order_acquire);
  snapshot.open_connections = open_connections_.load(std::memory_order_acquire);
  snapshot.queue_depth = queue_depth_.load(std::memory_order_acquire);
  snapshot.pending_handoff_bindings = PendingHandoffBindingCount();
  snapshot.handoff_complete_total = handoff_complete_total_.load(std::memory_order_acquire);
  snapshot.reject_total = reject_total_.load(std::memory_order_acquire);
  snapshot.pool_status = parser_pool_.Status();
  snapshot.metrics_json = metrics_.ToJson();
  {
    std::lock_guard<std::mutex> lock(support_event_mutex_);
    snapshot.management_decisions.assign(management_decisions_.begin(),
                                         management_decisions_.end());
    snapshot.runtime_events.assign(runtime_events_.begin(), runtime_events_.end());
  }
  return BuildListenerSupportBundleJson(snapshot);
}

void ListenerRuntime::RecordSupportEvent(std::string event_type,
                                         std::string operation,
                                         std::string outcome,
                                         std::string diagnostic_code,
                                         std::string safe_detail) {
  ListenerSupportBundleEvent event;
  event.timestamp_ms = proto::CurrentEpochMilliseconds();
  event.event_type = std::move(event_type);
  event.operation = std::move(operation);
  event.outcome = std::move(outcome);
  event.diagnostic_code = std::move(diagnostic_code);
  event.safe_detail = std::move(safe_detail);
  std::lock_guard<std::mutex> lock(support_event_mutex_);
  auto& target = event.event_type == "management_decision" ? management_decisions_ : runtime_events_;
  target.push_back(std::move(event));
  while (target.size() > kListenerSupportBundleHistoryMax) {
    target.pop_front();
  }
}

ParserHandoffBinding ListenerRuntime::TakePendingHandoffBinding(const ParserHandoffClientEvidence& evidence) {
  std::lock_guard<std::mutex> lock(pending_handoff_mutex_);
  const auto now_ms = proto::CurrentEpochMilliseconds();
  bool saw_live_binding = false;
  bool saw_binding_requiring_evidence = false;
  std::uint64_t expired_count = 0;
  for (auto it = pending_handoff_bindings_.begin(); it != pending_handoff_bindings_.end();) {
    if (it->expires_at_ms != 0 && it->expires_at_ms < now_ms) {
      it = pending_handoff_bindings_.erase(it);
      ++expired_count;
      continue;
    }
    saw_live_binding = true;
    saw_binding_requiring_evidence = saw_binding_requiring_evidence || HandoffBindingRequiresEvidence(*it);
    if (HandoffBindingMatchesEvidence(*it, evidence)) {
      ParserHandoffBinding binding = *it;
      pending_handoff_bindings_.erase(it);
      if (expired_count != 0) {
        metrics_.Increment("sys.metrics.listener.handoff_binding.expired_total", expired_count);
      }
      metrics_.Increment("sys.metrics.listener.handoff_binding.matched_total");
      metrics_.SetGauge("sys.metrics.listener.handoff_binding.pending", static_cast<double>(pending_handoff_bindings_.size()));
      return binding;
    }
    ++it;
  }
  if (expired_count != 0) {
    metrics_.Increment("sys.metrics.listener.handoff_binding.expired_total", expired_count);
  }
  if (!saw_live_binding) {
    metrics_.Increment("sys.metrics.listener.handoff_binding.no_binding_total");
  } else if (saw_binding_requiring_evidence && !HasClientEvidence(evidence)) {
    metrics_.Increment("sys.metrics.listener.handoff_binding.missing_evidence_total");
  } else {
    metrics_.Increment("sys.metrics.listener.handoff_binding.mismatch_total");
  }
  metrics_.SetGauge("sys.metrics.listener.handoff_binding.pending", static_cast<double>(pending_handoff_bindings_.size()));
  return ParserHandoffBinding{};
}

void ListenerRuntime::QueuePendingHandoffBinding(const proto::DbbtToken& token,
                                                 const proto::Lpreface& preface) {
  ParserHandoffBinding binding;
  binding.present = true;
  binding.db_uuid = token.db_uuid;
  binding.dbbt_id = First16(token.mac);
  binding.manager_session_id = token.manager_session_id;
  binding.client_nonce = token.client_nonce;
  binding.server_nonce = token.server_nonce;
  binding.listener_id = token.listener_id == 0 ? 1 : token.listener_id;
  binding.expires_at_ms = token.expires_at_ms;
  binding.auth_provider_family = preface.auth_provider_family.empty()
                                     ? "security_database_temporary_token"
                                     : preface.auth_provider_family;
  binding.auth_principal = preface.auth_principal;
  binding.auth_token = preface.auth_token;
  std::lock_guard<std::mutex> lock(pending_handoff_mutex_);
  pending_handoff_bindings_.push_back(binding);
  metrics_.SetGauge("sys.metrics.listener.handoff_binding.pending", static_cast<double>(pending_handoff_bindings_.size()));
}

std::size_t ListenerRuntime::PendingHandoffBindingCount() const {
  std::lock_guard<std::mutex> lock(pending_handoff_mutex_);
  const auto now_ms = proto::CurrentEpochMilliseconds();
  return static_cast<std::size_t>(std::count_if(pending_handoff_bindings_.begin(),
                                                pending_handoff_bindings_.end(),
                                                [now_ms](const ParserHandoffBinding& binding) {
                                                  return binding.expires_at_ms == 0 ||
                                                         binding.expires_at_ms >= now_ms;
                                                }));
}

void ListenerRuntime::QueuePendingHandoffBindingForTest(const proto::DbbtToken& token,
                                                        const proto::Lpreface& preface) {
  QueuePendingHandoffBinding(token, preface);
}

ParserHandoffBinding ListenerRuntime::TakePendingHandoffBindingForTest(
    const ParserHandoffClientEvidence& evidence) {
  return TakePendingHandoffBinding(evidence);
}

std::size_t ListenerRuntime::PendingHandoffBindingCountForTest() const {
  return PendingHandoffBindingCount();
}

ListenerRuntimeResult RunListenerFromArgs(int argc, char** argv) {
  auto cfg_result = LoadListenerConfigFromArgs(argc, argv);
  if (!cfg_result.ok) {
    return {2, cfg_result.messages};
  }
  ListenerRuntime runtime(std::move(cfg_result.config));
  return runtime.Run();
}

} // namespace scratchbird::listener
