// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_FOUNDATION_ENDPOINT

#include "ipc_server.hpp"

#include "parser_server_event_frame_dispatcher.hpp"
#include "listener_orchestrator.hpp"
#include "manager_control.hpp"
#include "maintenance_coordinator.hpp"
#include "parser_package_registry.hpp"
#include "sbps.hpp"
#include "server_daemon_lifecycle.hpp"
#include "server_agent_runtime.hpp"
#include "server_ipc_lifecycle.hpp"
#include "server_observability.hpp"
#include "session_registry.hpp"
#include "sblr_dispatch_server.hpp"

#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/sys_information_projection.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <malloc.h>
#endif

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
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace scratchbird::server {

namespace {

namespace engine_api = scratchbird::engine::internal_api;

std::atomic_bool g_stop_requested{false};

std::string CurrentUtcTimestampText() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string CurrentMonotonicNsText() {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}

void HandleStopSignal(int) {
  g_stop_requested.store(true);
}

ServerDiagnostic EndpointDiagnostic(std::string code,
                                    std::string message,
                                    std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

std::string StatePath(const ServerBootstrapConfig& config) {
  return config.lifecycle_state_file.string();
}

bool WriteEndpointDescriptor(const ServerBootstrapConfig& config,
                             const HostedEngineState& engine_state,
                             const ServerLifecycleArtifacts& artifacts,
                             std::vector<ServerDiagnostic>* diagnostics) {
  return WriteServerIpcEndpointDescriptor(
      BuildParserServerEndpointDescriptor(config, artifacts, engine_state),
      diagnostics);
}

void WriteServingState(const ServerBootstrapConfig& config,
                       const ServerLifecycleArtifacts& artifacts,
                       const ServerDaemonLifecycleSnapshot& daemon_lifecycle) {
  std::error_code ec;
  std::filesystem::create_directories(config.lifecycle_state_file.parent_path(), ec);
  std::ofstream out(StatePath(config), std::ios::trunc);
  if (!out) return;
  out << "format=SB_SERVER_LIFECYCLE_STATE_V1\n";
  out << "state_file_format_version=" << kServerLifecycleStateFileFormatCurrent << "\n";
  out << "state_file_supported_min=" << kServerLifecycleStateFileFormatMinSupported << "\n";
  out << "state_file_supported_max=" << kServerLifecycleStateFileFormatMaxSupported << "\n";
  out << "generation=" << artifacts.generation << "\n";
  out << "config_source_epoch=" << config.config_source_epoch << "\n";
  out << "config_reload_generation=" << config.config_reload_generation << "\n";
  out << "capability_policy_generation=" << config.capability_policy_generation << "\n";
  out << "policy_generation=" << config.security_policy_generation << "\n";
  out << "security_epoch=" << config.security_epoch << "\n";
  out << "cache_invalidation_epoch=" << config.cache_invalidation_epoch << "\n";
  out << "state=service_ready\n";
  out << "service_ready=" << (daemon_lifecycle.service_ready ? "true" : "false") << "\n";
  out << "daemon_scope=" << daemon_lifecycle.daemon_scope << "\n";
  out << "hosted_database_count=" << daemon_lifecycle.hosted_database_count << "\n";
  out << "open_database_count=" << daemon_lifecycle.open_database_count << "\n";
  out << "sbps_endpoint=" << config.sbps_endpoint.string() << "\n";
  out.close();
#ifndef _WIN32
  (void)::chmod(StatePath(config).c_str(), S_IRUSR | S_IWUSR);
#endif
}

#ifdef _WIN32
using IpcSocketHandle = SOCKET;
constexpr IpcSocketHandle kInvalidIpcSocket = INVALID_SOCKET;

bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}

std::string LastIpcSocketErrorString() {
  return "WSA error " + std::to_string(::WSAGetLastError());
}

bool IpcSocketInterrupted() {
  return ::WSAGetLastError() == WSAEINTR;
}

void CloseIpcSocket(IpcSocketHandle fd) {
  if (fd != kInvalidIpcSocket) {
    ::closesocket(fd);
  }
}

int SendIpcSocket(IpcSocketHandle fd, const std::uint8_t* data, std::size_t size) {
  const auto chunk = static_cast<int>(
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return ::send(fd, reinterpret_cast<const char*>(data), chunk, 0);
}

int RecvIpcSocket(IpcSocketHandle fd, std::uint8_t* data, std::size_t size) {
  const auto chunk = static_cast<int>(
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return ::recv(fd, reinterpret_cast<char*>(data), chunk, 0);
}
#else
using IpcSocketHandle = int;
constexpr IpcSocketHandle kInvalidIpcSocket = -1;

std::string LastIpcSocketErrorString() {
  return std::strerror(errno);
}

bool IpcSocketInterrupted() {
  return errno == EINTR;
}

void CloseIpcSocket(IpcSocketHandle fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

int SendIpcSocket(IpcSocketHandle fd, const std::uint8_t* data, std::size_t size) {
  const auto chunk =
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#ifdef MSG_NOSIGNAL
  return static_cast<int>(::send(fd, data, chunk, MSG_NOSIGNAL));
#else
  return static_cast<int>(::send(fd, data, chunk, 0));
#endif
}

int RecvIpcSocket(IpcSocketHandle fd, std::uint8_t* data, std::size_t size) {
  const auto chunk =
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return static_cast<int>(::recv(fd, data, chunk, 0));
}
#endif

std::string PlatformEndpointPath(const std::filesystem::path& path) {
#ifdef _WIN32
  return std::filesystem::absolute(path).string();
#else
  return path.string();
#endif
}

void RemoveEndpointPath(const std::string& endpoint) {
  std::error_code ec;
  std::filesystem::remove(endpoint, ec);
}

void ReleaseIdleConnectionHeap(const ServerBootstrapConfig& config,
                               ServerObservabilityState* observability) {
  if (!config.memory_trim_heap_on_disconnect) {
    IncrementServerMetric(observability,
                          "sys.metrics.server.memory.heap_trim_skipped_total",
                          1,
                          {{"reason", "policy_retains_adaptive_cache"}});
    return;
  }
#if defined(__GLIBC__)
  const int trimmed = ::malloc_trim(0);
  if (trimmed != 0) {
    IncrementServerMetric(observability,
                          "sys.metrics.server.memory.heap_trim_total",
                          1,
                          {{"reason", "disconnect"}});
  }
#else
  (void)config;
  (void)observability;
#endif
}

bool WriteRawAll(IpcSocketHandle fd, const std::vector<std::uint8_t>& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto rc = SendIpcSocket(fd, data.data() + sent, data.size() - sent);
    if (rc < 0) {
      if (IpcSocketInterrupted()) continue;
      return false;
    }
    if (rc == 0) return false;
    sent += static_cast<std::size_t>(rc);
  }
  return true;
}

bool WriteAll(IpcSocketHandle fd, const std::vector<std::uint8_t>& data) {
  const auto decoded =
      sbps::DecodeFrameBytes(data, std::numeric_limits<std::uint32_t>::max());
  if (!decoded.ok() || !decoded.frame.has_value()) {
    return WriteRawAll(fd, data);
  }
  constexpr std::uint64_t kDefaultPhysicalFrameLimit = 1024 * 1024;
  for (const auto& physical :
       sbps::EncodeFrameSequence(decoded.frame->header,
                                 decoded.frame->payload,
                                 kDefaultPhysicalFrameLimit)) {
    if (!WriteRawAll(fd, physical)) return false;
  }
  return true;
}

bool ReadExact(IpcSocketHandle fd, std::vector<std::uint8_t>* data, std::size_t bytes) {
  data->resize(bytes);
  std::size_t received = 0;
  while (received < bytes) {
    const auto rc = RecvIpcSocket(fd, data->data() + received, bytes - received);
    if (rc < 0) {
      if (IpcSocketInterrupted()) continue;
      return false;
    }
    if (rc == 0) return false;
    received += static_cast<std::size_t>(rc);
  }
  return true;
}

std::uint32_t PhysicalFrameLimit(const ServerBootstrapConfig& config) {
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(config.sbps_max_frame_bytes,
                              std::numeric_limits<std::uint32_t>::max()));
}

bool ReadPhysicalFrame(IpcSocketHandle client_fd,
                       const ServerBootstrapConfig& config,
                       sbps::Frame* frame,
                       std::vector<ServerDiagnostic>* diagnostics) {
  std::vector<std::uint8_t> header_bytes;
  if (!ReadExact(client_fd, &header_bytes, sbps::kHeaderBytes)) {
    diagnostics->push_back(sbps::IpcDiagnostic(
        "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID",
        "parser_server_ipc.frame_length_invalid",
        "The SBPS frame header is incomplete."));
    return false;
  }
  const auto payload_len = sbps::PayloadLengthFromHeader(header_bytes).value_or(0);
  if (payload_len > config.sbps_max_frame_bytes) {
    diagnostics->push_back(sbps::IpcDiagnostic(
        "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID",
        "parser_server_ipc.frame_length_invalid",
        "The SBPS physical frame exceeds the negotiated frame limit."));
    return false;
  }
  std::vector<std::uint8_t> payload;
  if (payload_len > 0 && !ReadExact(client_fd, &payload, payload_len)) {
    diagnostics->push_back(sbps::IpcDiagnostic(
        "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID",
        "parser_server_ipc.frame_length_invalid",
        "The SBPS payload is incomplete."));
    return false;
  }
  std::vector<std::uint8_t> full = header_bytes;
  full.insert(full.end(), payload.begin(), payload.end());
  auto decoded = sbps::DecodeFrameBytes(full, PhysicalFrameLimit(config));
  if (!decoded.ok()) {
    *diagnostics = std::move(decoded.diagnostics);
    return false;
  }
  *frame = std::move(*decoded.frame);
  return true;
}

bool CompatibleChunk(const sbps::Frame& first,
                     const sbps::Frame& next,
                     std::uint64_t expected_sequence) {
  return (next.header.flags & sbps::kFlagPayloadChunk) != 0 &&
         next.header.message_type == first.header.message_type &&
         next.header.payload_schema_id == first.header.payload_schema_id &&
         next.header.stream_id == first.header.stream_id &&
         next.header.sequence_number == expected_sequence &&
         next.header.request_uuid == first.header.request_uuid &&
         next.header.connection_uuid == first.header.connection_uuid &&
         next.header.session_uuid == first.header.session_uuid;
}

bool AssembleChunkedFrame(IpcSocketHandle client_fd,
                          const ServerBootstrapConfig& config,
                          sbps::Frame* frame,
                          std::vector<ServerDiagnostic>* diagnostics) {
  if ((frame->header.flags & sbps::kFlagPayloadChunk) == 0) return true;
  if (frame->header.stream_id == 0 || frame->header.sequence_number == 0) {
    diagnostics->push_back(sbps::IpcDiagnostic(
        "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
        "parser_server_ipc.chunk_sequence_invalid",
        "The SBPS chunk sequence header is invalid."));
    return false;
  }
  const auto max_chunks = std::max<std::uint64_t>(1, config.sbps_max_streams);
  const auto max_total = config.sbps_max_frame_bytes >
                                 std::numeric_limits<std::uint64_t>::max() / max_chunks
                             ? std::numeric_limits<std::uint64_t>::max()
                             : config.sbps_max_frame_bytes * max_chunks;
  std::vector<sbps::Frame> chunks{*frame};
  sbps::Frame last = chunks.front();
  std::uint64_t expected_sequence = frame->header.sequence_number + 1;
  while ((last.header.flags & sbps::kFlagFinal) == 0) {
    if (expected_sequence > frame->header.sequence_number + max_chunks) {
      diagnostics->push_back(sbps::IpcDiagnostic(
          "PARSER_SERVER_IPC.PAYLOAD_TOO_LARGE",
          "parser_server_ipc.payload_too_large",
          "The SBPS chunk sequence exceeds the configured stream limit."));
      return false;
    }
    sbps::Frame next;
    if (!ReadPhysicalFrame(client_fd, config, &next, diagnostics)) return false;
    if (!CompatibleChunk(chunks.front(), next, expected_sequence)) {
      diagnostics->push_back(sbps::IpcDiagnostic(
          "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
          "parser_server_ipc.chunk_sequence_invalid",
          "The SBPS chunk sequence is not contiguous."));
      return false;
    }
    chunks.push_back(std::move(next));
    last = chunks.back();
    ++expected_sequence;
  }
  const auto assembled = sbps::AssembleDecodedChunkSequence(chunks, max_total);
  if (!assembled.ok()) {
    *diagnostics = assembled.diagnostics;
    return false;
  }
  *frame = std::move(*assembled.frame);
  return true;
}

bool ClientSocketReady(IpcSocketHandle client_fd) {
#ifdef _WIN32
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(client_fd, &read_set);
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;
  const int rc = ::select(0, &read_set, nullptr, nullptr, &timeout);
  return rc > 0 && FD_ISSET(client_fd, &read_set);
#else
  pollfd client{};
  client.fd = client_fd;
  client.events = POLLIN;
  const int rc = ::poll(&client, 1, 100);
  if (rc <= 0) return false;
  if ((client.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return true;
  return (client.revents & POLLIN) != 0;
#endif
}

std::vector<std::uint8_t> ErrorFrame(const std::vector<ServerDiagnostic>& diagnostics,
                                     const std::array<std::uint8_t, 16>& request_uuid,
                                     std::uint64_t sequence_number,
                                     std::uint16_t message_type =
                                         static_cast<std::uint16_t>(sbps::MessageType::kDiagnostic)) {
  const auto payload = sbps::EncodeMessageVectorSet(diagnostics, request_uuid);
  sbps::FrameHeader header;
  header.message_type = message_type;
  header.flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
  header.payload_schema_id = sbps::kSchemaMessageVectorSetV1;
  header.sequence_number = sequence_number;
  header.request_uuid = request_uuid;
  return sbps::EncodeFrame(header, payload);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  const auto length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::vector<std::uint8_t> AcceptFrame(const sbps::Frame& request,
                                      const ServerBootstrapConfig& config) {
  sbps::HelloAccept accept;
  accept.server_uuid = sbps::MakeUuidV7Bytes();
  accept.channel_uuid = sbps::MakeUuidV7Bytes();
  accept.max_frame_bytes = static_cast<std::uint32_t>(config.sbps_max_frame_bytes);
  accept.max_streams = static_cast<std::uint32_t>(config.sbps_max_streams);
  accept.accepted_capability_bitmap[0] = 1;
  accept.registry_snapshot_uuid = sbps::MakeUuidV7Bytes();
  const auto payload = sbps::EncodeHelloAccept(accept);
  sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kHelloAccept);
  header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
  header.payload_schema_id = sbps::kSchemaHelloAcceptV1;
  header.stream_id = request.header.stream_id;
  header.sequence_number = request.header.sequence_number;
  header.request_uuid = request.header.request_uuid;
  return sbps::EncodeFrame(header, payload);
}

std::uint32_t EventSchemaFor(std::uint16_t message_type) {
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventSubscribeRequest)) return sbps::kSchemaEventSubscribeRequestV1;
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventSubscribeResult)) return sbps::kSchemaEventSubscribeResultV1;
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventUnsubscribeRequest)) return sbps::kSchemaEventUnsubscribeRequestV1;
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventUnsubscribeResult)) return sbps::kSchemaEventUnsubscribeResultV1;
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventNotification)) return sbps::kSchemaEventNotificationV1;
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventAck)) return sbps::kSchemaEventAckV1;
  if (message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventBackpressure)) return sbps::kSchemaEventBackpressureV1;
  return sbps::kSchemaNone;
}

std::vector<std::pair<std::string, std::string>> DecodeEventFieldPayload(
    const std::vector<std::uint8_t>& payload) {
  std::vector<std::pair<std::string, std::string>> fields;
  if (payload.size() < 2) return fields;
  std::size_t offset = 0;
  const auto count = GetU16(payload, offset);
  offset += 2;
  for (std::uint16_t i = 0; i < count; ++i) {
    std::string key;
    std::string value;
    if (!ReadString(payload, &offset, &key) || !ReadString(payload, &offset, &value)) {
      fields.clear();
      return fields;
    }
    fields.push_back({std::move(key), std::move(value)});
  }
  return fields;
}

std::vector<std::uint8_t> EncodeEventFieldPayload(
    const std::vector<std::pair<std::string, std::string>>& fields,
    const std::vector<ParserServerMessageVector>& vectors = {}) {
  std::vector<std::pair<std::string, std::string>> all = fields;
  for (const auto& vector : vectors) {
    all.push_back({"message_class", vector.message_class});
    all.push_back({"diagnostic_code", vector.diagnostic_code});
    all.push_back({"safe_message_key", vector.safe_message_key});
    all.push_back({"detail", vector.detail});
    for (const auto& field : vector.fields) {
      all.push_back({"message_vector." + field.first, field.second});
    }
  }
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(all.size()));
  for (const auto& field : all) {
    PutString(&out, field.first);
    PutString(&out, field.second);
  }
  return out;
}

std::optional<ServerSessionRecord> FindServerSession(ServerSessionRegistry* registry,
                                                     const std::array<std::uint8_t, 16>& session_uuid) {
  const auto found = registry->sessions_by_uuid.find(UuidBytesToText(session_uuid));
  if (found == registry->sessions_by_uuid.end()) return std::nullopt;
  return found->second;
}

ParserServerEventEngineContext EventEngineContextFromSession(
    const ServerSessionRecord& session,
    const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  ParserServerEventEngineContext context;
  context.trust_mode = session.embedded_in_process
                           ? ParserServerEventTrustMode::embedded_in_process
                           : ParserServerEventTrustMode::server_isolated;
  context.request_id = UuidBytesToText(request.header.request_uuid);
  context.database_path = session.database_path;
  context.database_uuid.canonical = session.database_uuid;
  if (context.database_path.empty()) {
    for (const auto& database : engine_state.databases) {
      if (database.database_open) {
        context.database_path = database.database_path;
        context.database_uuid.canonical = database.database_uuid;
        context.database_page_size_bytes = database.page_size_bytes;
        break;
      }
    }
  } else {
    for (const auto& database : engine_state.databases) {
      if (!database.database_open) continue;
      const bool path_matches = database.database_path == context.database_path;
      const bool uuid_matches = !context.database_uuid.canonical.empty() &&
                                database.database_uuid == context.database_uuid.canonical;
      if (path_matches || uuid_matches) {
        context.database_page_size_bytes = database.page_size_bytes;
        break;
      }
    }
  }
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.statement_uuid.canonical = UuidBytesToText(request.header.request_uuid);
  context.local_transaction_id = session.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id = session.snapshot_visible_through_local_transaction_id;
  context.statement_timestamp = CurrentUtcTimestampText();
  context.transaction_timestamp = session.transaction_timestamp;
  context.current_timestamp = context.statement_timestamp;
  context.current_monotonic_ns = CurrentMonotonicNsText();
  context.application_name = session.application_name;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  const auto language = ServerLanguageContextForSession(session);
  context.language_context.language_profile_id = language.language_profile_id;
  context.language_context.language_tag = language.language_tag;
  context.language_context.default_language_tag = language.default_language_tag;
  context.language_context.input_syntax_profile = language.input_syntax_profile;
  context.language_context.input_language_fallback_tag =
      language.input_language_fallback_tag;
  context.language_context.common_resource_hash = language.common_resource_hash;
  context.language_context.language_resource_epoch =
      language.language_resource_epoch;
  context.language_context.localized_name_epoch = language.localized_name_epoch;
  context.language_context.message_resource_epoch = language.message_resource_epoch;
  context.language_context.resource_compatibility_identity =
      language.resource_compatibility_identity;
  context.language_context.resource_version_identity =
      language.resource_version_identity;
  context.trace_tags.push_back("sb_server.event_notification");
  return context;
}

std::optional<ParserServerEventSession> EventSessionFromFrame(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request) {
  if (sbps::IsZeroUuid(request.header.session_uuid)) return std::nullopt;
  const auto session = FindServerSession(registry, request.header.session_uuid);
  if (!session) return std::nullopt;
  ParserServerEventSession event_session;
  event_session.parser_channel_uuid = sbps::IsZeroUuid(request.header.connection_uuid)
                                          ? UuidBytesToText(request.header.session_uuid)
                                          : UuidBytesToText(request.header.connection_uuid);
  event_session.engine_context = EventEngineContextFromSession(*session, engine_state, request);
  event_session.session_bound = true;
  event_session.draining = registry->channel_state == ServerChannelState::kDraining;
  return event_session;
}

std::vector<std::uint8_t> EventOutboundFrameBytes(const sbps::Frame& request,
                                                  const ParserServerEventOutboundFrame& outbound) {
  const auto message_type = static_cast<std::uint16_t>(outbound.message_type);
  const auto payload = EncodeEventFieldPayload(outbound.fields, outbound.message_vector_set);
  sbps::FrameHeader header;
  header.message_type = message_type;
  header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
  for (const auto& vector : outbound.message_vector_set) {
    if (vector.error) {
      header.flags |= sbps::kFlagError;
      break;
    }
  }
  header.payload_schema_id = EventSchemaFor(message_type);
  header.stream_id = request.header.stream_id;
  header.sequence_number = request.header.sequence_number;
  header.request_uuid = request.header.request_uuid;
  header.connection_uuid = request.header.connection_uuid;
  header.session_uuid = request.header.session_uuid;
  return sbps::EncodeFrame(header, payload);
}

bool WriteEventDispatchResult(IpcSocketHandle client_fd,
                              const sbps::Frame& request,
                              const ParserServerEventDispatchResult& dispatch) {
  if (dispatch.outbound_frames.empty()) {
    const auto frame = ErrorFrame(
        {sbps::IpcDiagnostic("PARSER_SERVER_IPC.EVENT_RUNTIME_EMPTY_RESULT",
                             "parser_server_ipc.event_runtime_empty_result",
                             "The event runtime produced no response frame.")},
        request.header.request_uuid,
        request.header.sequence_number);
    return WriteAll(client_fd, frame);
  }
  bool ok = true;
  for (const auto& outbound : dispatch.outbound_frames) {
    ok = WriteAll(client_fd, EventOutboundFrameBytes(request, outbound)) && ok;
  }
  return ok;
}

bool PumpEventNotifications(IpcSocketHandle client_fd,
                            const sbps::Frame& request,
                            const HostedEngineState& engine_state,
                            ServerSessionRegistry* session_registry,
                            ParserEventNotificationRouter* event_router) {
  const auto event_session = EventSessionFromFrame(session_registry, engine_state, request);
  if (!event_session) return true;
  ParserServerEventIpcRuntime runtime(event_router);
  ParserServerEventFrameDispatcher dispatcher(&runtime);
  PsEventDeliveryPumpRequest pump;
  pump.request_uuid = UuidBytesToText(request.header.request_uuid);
  pump.session = *event_session;
  pump.max_events = 64;
  const auto dispatch = dispatcher.PumpCommittedEvents(pump);
  if (dispatch.outbound_frames.empty()) return true;
  return WriteEventDispatchResult(client_fd, request, dispatch);
}

bool HandleEventFrame(IpcSocketHandle client_fd,
                      const sbps::Frame& request,
                      const HostedEngineState& engine_state,
                      ServerSessionRegistry* session_registry,
                      ParserEventNotificationRouter* event_router) {
  const auto event_session = EventSessionFromFrame(session_registry, engine_state, request);
  if (!event_session) {
    WriteAll(client_fd, ErrorFrame(
                           {sbps::IpcDiagnostic("PARSER_SERVER_IPC.SESSION_NOT_BOUND",
                                                "parser_server_ipc.session_not_bound",
                                                "Event subscription IPC requires a bound server session.")},
                           request.header.request_uuid, request.header.sequence_number));
    return false;
  }
  ParserServerEventFrame event_frame;
  event_frame.message_type = static_cast<ParserServerEventMessageType>(request.header.message_type);
  event_frame.request_uuid = UuidBytesToText(request.header.request_uuid);
  event_frame.session = *event_session;
  event_frame.fields = DecodeEventFieldPayload(request.payload);
  ParserServerEventIpcRuntime runtime(event_router);
  ParserServerEventFrameDispatcher dispatcher(&runtime);
  const auto dispatch = dispatcher.DispatchParserFrame(event_frame);
  WriteEventDispatchResult(client_fd, request, dispatch);
  return dispatch.ok;
}

std::vector<std::uint8_t> PongFrame(const sbps::Frame& request, const HostedEngineState& engine_state) {
  auto payload = request.payload;
  const std::string request_text(request.payload.begin(), request.payload.end());
  if (request_text == "database_status") {
    const auto status = HostedEngineStatusJson(engine_state);
    payload.assign(status.begin(), status.end());
  }
  sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPong);
  header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
  header.payload_schema_id = request.header.payload_schema_id;
  header.stream_id = request.header.stream_id;
  header.sequence_number = request.header.sequence_number;
  header.request_uuid = request.header.request_uuid;
  return sbps::EncodeFrame(header, payload);
}

std::vector<std::uint8_t> SessionOperationFrame(const sbps::Frame& request,
                                                const SessionOperationResult& operation) {
  std::vector<std::uint8_t> payload = operation.payload;
  auto schema = operation.response_schema_id;
  auto flags = operation.frame_flags;
  if (!operation.diagnostics.empty() && (flags & sbps::kFlagError) != 0) {
    payload = sbps::EncodeMessageVectorSet(operation.diagnostics, request.header.request_uuid);
    schema = sbps::kSchemaMessageVectorSetV1;
  }
  sbps::FrameHeader header;
  header.message_type = operation.response_message_type;
  header.flags = flags;
  header.payload_schema_id = schema;
  header.stream_id = request.header.stream_id;
  header.sequence_number = request.header.sequence_number;
  header.request_uuid = request.header.request_uuid;
  header.connection_uuid = request.header.connection_uuid;
  header.session_uuid = operation.session_uuid;
  return sbps::EncodeFrame(header, payload);
}

std::vector<std::uint8_t> ManagementOperationFrame(const sbps::Frame& request,
                                                   const ServerManagementResponse& operation) {
  sbps::FrameHeader header;
  header.message_type = operation.response_message_type;
  header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
  if (operation.error) header.flags |= sbps::kFlagError;
  header.payload_schema_id = operation.response_schema_id;
  header.stream_id = request.header.stream_id;
  header.sequence_number = request.header.sequence_number;
  header.request_uuid = request.header.request_uuid;
  header.connection_uuid = request.header.connection_uuid;
  header.session_uuid = operation.session_uuid;
  return sbps::EncodeFrame(header, operation.payload);
}

void PsNamePutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void PsNamePutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PsNamePutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PsNamePutUuid(std::vector<std::uint8_t>* out,
                   const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PsNamePutString(std::vector<std::uint8_t>* out, std::string_view value) {
  const auto len = static_cast<std::uint16_t>(value.size() > 65535 ? 65535 : value.size());
  PsNamePutU16(out, len);
  out->insert(out->end(), value.begin(), value.begin() + len);
}

std::uint16_t PsNameGetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

bool PsNameReadString(const std::vector<std::uint8_t>& data,
                      std::size_t* offset,
                      std::string* out) {
  if (*offset + 2 > data.size()) return false;
  const auto length = PsNameGetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::array<std::uint8_t, 16> PsNameGetUuid(const std::vector<std::uint8_t>& data,
                                           std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  if (offset + uuid.size() <= data.size()) {
    std::copy_n(data.data() + offset, uuid.size(), uuid.data());
  }
  return uuid;
}

int PsNameHexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

std::optional<std::array<std::uint8_t, 16>> PsNameUuidFromText(std::string_view text) {
  std::array<std::uint8_t, 16> uuid{};
  std::size_t nibble = 0;
  for (char ch : text) {
    if (ch == '-') continue;
    const int value = PsNameHexValue(ch);
    if (value < 0 || nibble >= 32) return std::nullopt;
    if ((nibble % 2) == 0) {
      uuid[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      uuid[nibble / 2] = static_cast<std::uint8_t>(uuid[nibble / 2] | value);
    }
    ++nibble;
  }
  if (nibble != 32) return std::nullopt;
  return uuid;
}

std::string PsNameLower(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return value;
}

std::array<std::uint8_t, 16> PsNameSyntheticUuid(std::string_view normalized_name) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char ch : normalized_name) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  std::array<std::uint8_t, 16> uuid{};
  for (int i = 0; i < 8; ++i) {
    uuid[static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((hash >> (i * 8)) & 0xffu);
    uuid[static_cast<std::size_t>(8 + i)] =
        static_cast<std::uint8_t>(((hash ^ 0xa5a5a5a5a5a5a5a5ull) >> (i * 8)) & 0xffu);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

std::string PsNameVirtualSystemName(std::string normalized_name) {
  if (normalized_name == "sys.version" ||
      normalized_name == "sys.metrics" ||
      normalized_name == "sys.catalog") {
    return normalized_name;
  }
  const std::string canonical_name =
      engine_api::SysInformationCanonicalViewPath(normalized_name);
  if (engine_api::FindSysInformationProjectionDefinition(canonical_name) != nullptr) {
    return canonical_name;
  }
  return {};
}

std::optional<std::string> PsNameRenderedVirtualSystemName(
    const std::array<std::uint8_t, 16>& uuid) {
  if (uuid == PsNameSyntheticUuid("sys.version")) return "sys.version";
  if (uuid == PsNameSyntheticUuid("sys.metrics")) return "sys.metrics";
  if (uuid == PsNameSyntheticUuid("sys.catalog")) return "sys.catalog";
  for (const auto& definition : engine_api::BuiltinSysInformationProjectionDefinitions()) {
    if (uuid == PsNameSyntheticUuid(definition.view_path)) return definition.view_path;
    static constexpr std::string_view kCanonicalPrefix = "sys.information.";
    if (definition.view_path.rfind(kCanonicalPrefix, 0) == 0) {
      const std::string legacy_alias =
          "sys.information_schema." + definition.view_path.substr(kCanonicalPrefix.size());
      if (uuid == PsNameSyntheticUuid(legacy_alias)) return definition.view_path;
    }
  }
  return std::nullopt;
}

bool PsNameHasOpenDatabase(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return true;
  }
  return false;
}

struct PsNamePart {
  std::string text;
  bool quoted = false;
};

std::optional<std::vector<PsNamePart>> PsNameSplitPresentedName(std::string_view presented_name,
                                                                bool request_quoted) {
  std::vector<PsNamePart> parts;
  PsNamePart current;
  bool in_quote = false;
  bool saw_quote = false;
  for (std::size_t i = 0; i < presented_name.size(); ++i) {
    const char ch = presented_name[i];
    if (ch == '"') {
      if (in_quote && i + 1 < presented_name.size() && presented_name[i + 1] == '"') {
        current.text.push_back('"');
        ++i;
        continue;
      }
      in_quote = !in_quote;
      saw_quote = true;
      current.quoted = true;
      continue;
    }
    if (ch == '.' && !in_quote) {
      if (current.text.empty()) return std::nullopt;
      current.quoted = current.quoted || request_quoted;
      parts.push_back(std::move(current));
      current = {};
      saw_quote = false;
      continue;
    }
    current.text.push_back(ch);
  }
  if (in_quote || current.text.empty()) return std::nullopt;
  current.quoted = current.quoted || request_quoted || saw_quote;
  parts.push_back(std::move(current));
  return parts;
}

engine_api::EngineIdentifierAtom PsNameIdentifierAtom(const PsNamePart& part,
                                                      std::string_view identifier_profile) {
  engine_api::EngineIdentifierAtom atom;
  atom.raw_text = part.text;
  atom.was_quoted = part.quoted;
  atom.quote_style = part.quoted ? "double_quote" : "none";
  atom.requires_exact_match = part.quoted;
  atom.identifier_profile_uuid = identifier_profile.empty() ? "sbsql_v3" : std::string(identifier_profile);
  return atom;
}

engine_api::EngineRequestContext PsNameEngineContextFromSession(
    const ServerSessionRecord& session,
    const HostedEngineState& engine_state,
    const sbps::Frame& frame,
    std::string_view language) {
  engine_api::EngineRequestContext context;
  context.trust_mode = session.embedded_in_process
                           ? engine_api::EngineTrustMode::embedded_in_process
                           : engine_api::EngineTrustMode::server_isolated;
  context.request_id = UuidBytesToText(frame.header.request_uuid);
  context.database_path = session.database_path;
  context.database_uuid.canonical = session.database_uuid;
  if (context.database_path.empty()) {
    for (const auto& database : engine_state.databases) {
      if (!database.database_open) continue;
      context.database_path = database.database_path;
      context.database_uuid.canonical = database.database_uuid;
      context.database_page_size_bytes = database.page_size_bytes;
      break;
    }
  } else {
    for (const auto& database : engine_state.databases) {
      if (!database.database_open) continue;
      const bool path_matches = database.database_path == context.database_path;
      const bool uuid_matches = !context.database_uuid.canonical.empty() &&
                                database.database_uuid == context.database_uuid.canonical;
      if (path_matches || uuid_matches) {
        context.database_page_size_bytes = database.page_size_bytes;
        break;
      }
    }
  }
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  if (!sbps::IsZeroUuid(session.active_role_uuid)) {
    context.current_role_uuid.canonical = UuidBytesToText(session.active_role_uuid);
  }
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.statement_uuid.canonical = UuidBytesToText(frame.header.request_uuid);
  context.local_transaction_id = session.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  context.statement_timestamp = CurrentUtcTimestampText();
  context.transaction_timestamp = session.transaction_timestamp;
  context.application_name = session.application_name;
  context.current_timestamp = context.statement_timestamp;
  context.current_monotonic_ns = CurrentMonotonicNsText();
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  auto language_session = session;
  if (!language.empty()) {
    ApplyRequestedLanguageProfile(&language_session, language);
  }
  PopulateEngineLanguageContextFromSession(language_session,
                                           &context.language_context);
  context.trace_tags = session.engine_authorization_trace_tags;
  return context;
}

bool PsNameObjectClassMatches(std::string_view requested, std::string_view actual) {
  if (requested.empty()) return true;
  if (requested == actual) return true;
  if (requested == "relation") {
    return actual == "table" || actual == "view" || actual == "materialized_view" ||
           actual == "external_table" || actual == "foreign_table";
  }
  if (requested == "role") {
    return actual == "security_role" || actual == "principal";
  }
  if (requested == "group") {
    return actual == "security_group" || actual == "principal";
  }
  if (requested == "principal") {
    return actual == "user" || actual == "security_role" ||
           actual == "security_group" || actual == "role" || actual == "group";
  }
  if (requested == "policy") {
    return actual == "security_policy";
  }
  if (requested == "mask" || requested == "rls") {
    return actual == "security_policy";
  }
  return false;
}

std::string PsNameCanonicalIdentifierProfile(std::string value) {
  value = PsNameLower(std::move(value));
  if (value.empty() || value == "default" || value == "native" || value == "sbsql" ||
      value == "embedded" || value == "local_ipc" || value == "local-ipc" ||
      value == "inet" || value == "inet_listener" || value == "managed" ||
      value == "sif.test") {
    return "sbsql_v3";
  }
  return value;
}

std::optional<engine_api::NameRegistryEntry> PsNameResolveUniqueRegistryLeaf(
    const engine_api::EngineRequestContext& context,
    const PsNamePart& leaf,
    std::string_view object_class,
    std::string_view identifier_profile) {
  auto load_context = context;
  const std::uint64_t observer_tx =
      context.snapshot_visible_through_local_transaction_id != 0
          ? context.snapshot_visible_through_local_transaction_id
          : context.local_transaction_id;
  auto loaded = engine_api::LoadNameRegistryState(load_context, observer_tx);
  if (!loaded.ok && load_context.local_transaction_id != 0) {
    load_context.local_transaction_id = 0;
    loaded = engine_api::LoadNameRegistryState(load_context, observer_tx);
  }
  if (!loaded.ok) return std::nullopt;
  const std::string profile =
      PsNameCanonicalIdentifierProfile(std::string(identifier_profile));
  const std::string lookup_key =
      engine_api::NameRegistryLookupKey(leaf.text, profile, leaf.quoted);
  std::optional<engine_api::NameRegistryEntry> match;
  for (const auto& entry : loaded.state.entries) {
    if (entry.deleted || entry.lifecycle_state != "active") continue;
    if (!PsNameObjectClassMatches(object_class, entry.object_class)) continue;
    const std::string entry_profile = entry.identifier_profile_uuid.empty()
                                          ? "sbsql_v3"
                                          : entry.identifier_profile_uuid;
    if (PsNameCanonicalIdentifierProfile(entry_profile) != profile) continue;
    if (!leaf.quoted && entry.requires_exact_match) continue;
    const std::string entry_key = leaf.quoted ? entry.exact_lookup_key : entry.normalized_lookup_key;
    if (entry_key != lookup_key) continue;
    if (match && match->object_uuid != entry.object_uuid) return std::nullopt;
    match = entry;
  }
  return match;
}

bool PsNameRegistryMatchVisibleForSession(
    const engine_api::EngineRequestContext& context,
    const engine_api::NameRegistryEntry& match) {
  if (match.object_class != "table" && match.object_class != "relation") {
    return true;
  }
  const auto visibility =
      engine_api::CheckMgaTemporaryTableVisibility(context, match.object_uuid);
  if (!visibility.ok) return false;
  if (visibility.hidden_by_temporary_visibility) return false;
  if (visibility.known_temporary && !visibility.visible_to_session) return false;
  return true;
}

bool PsNameResolutionCacheable(const engine_api::EngineRequestContext& context,
                               std::string_view object_class,
                               std::string_view object_uuid) {
  if (object_uuid.empty()) return false;
  if (object_class != "table" && object_class != "relation") {
    return true;
  }
  const auto visibility =
      engine_api::CheckMgaTemporaryTableVisibility(context, std::string(object_uuid));
  if (!visibility.ok) return false;
  if (visibility.hidden_by_temporary_visibility) return false;
  if (visibility.known_temporary) return false;
  return true;
}

bool PsNameRelationLikeObjectClass(std::string_view object_class) {
  return object_class == "relation" ||
         object_class == "table" ||
         object_class == "view" ||
         object_class == "materialized_view" ||
         object_class == "external_table" ||
         object_class == "foreign_table";
}

bool PsNameStableResolutionCacheable(
    const engine_api::EngineRequestContext& context,
    std::string_view object_class,
    std::string_view object_uuid) {
  return PsNameRelationLikeObjectClass(object_class) &&
         PsNameResolutionCacheable(context, object_class, object_uuid);
}

bool PsNameSessionBound(const ServerSessionRegistry* registry,
                        const std::array<std::uint8_t, 16>& session_uuid) {
  if (registry == nullptr || sbps::IsZeroUuid(session_uuid)) return false;
  return registry->sessions_by_uuid.find(UuidBytesToText(session_uuid)) !=
         registry->sessions_by_uuid.end();
}

std::vector<std::uint8_t> PsNameResponseFrame(const sbps::Frame& request,
                                              std::uint16_t response_type,
                                              std::uint32_t schema,
                                              const std::vector<std::uint8_t>& payload,
                                              bool error = false) {
  sbps::FrameHeader header;
  header.message_type = response_type;
  header.flags = sbps::kFlagResponse | sbps::kFlagFinal | (error ? sbps::kFlagError : 0);
  header.payload_schema_id = schema;
  header.stream_id = request.header.stream_id;
  header.sequence_number = request.header.sequence_number;
  header.request_uuid = request.header.request_uuid;
  header.connection_uuid = request.header.connection_uuid;
  header.session_uuid = request.header.session_uuid;
  return sbps::EncodeFrame(header, payload);
}

struct PsNameResolveRequest {
  std::string presented_name;
  bool quoted = false;
  std::string dialect_profile;
  std::string language;
  std::string search_path;
  std::string object_class;
  bool bypass_cache = false;
};

std::optional<PsNameResolveRequest> DecodePsNameResolveRequest(
    const std::vector<std::uint8_t>& payload) {
  PsNameResolveRequest request;
  std::size_t offset = 0;
  if (!PsNameReadString(payload, &offset, &request.presented_name)) return std::nullopt;
  if (offset >= payload.size()) return std::nullopt;
  request.quoted = payload[offset++] != 0;
  if (!PsNameReadString(payload, &offset, &request.dialect_profile)) return std::nullopt;
  if (!PsNameReadString(payload, &offset, &request.language)) return std::nullopt;
  if (!PsNameReadString(payload, &offset, &request.search_path)) return std::nullopt;
  if (!PsNameReadString(payload, &offset, &request.object_class)) return std::nullopt;
  if (offset < payload.size()) {
    request.bypass_cache = payload[offset++] != 0;
  }
  return request;
}

std::string PsNameTraceField(std::string_view value) {
  std::string out(value);
  for (char& ch : out) {
    if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
  }
  return out;
}

std::uint64_t PsNameTraceElapsedMicros(std::chrono::steady_clock::time_point begin) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin)
          .count());
}

void WritePsNameResolutionTrace(const PsNameResolveRequest& request,
                                const ServerSessionRecord* session,
                                std::string_view outcome,
                                std::string_view detail,
                                std::string_view object_uuid,
                                std::string_view object_class,
                                std::string_view cache_key,
                                std::string_view stable_cache_key,
                                bool normal_cache_checked,
                                bool normal_cache_hit,
                                bool stable_cache_checked,
                                bool stable_cache_hit,
                                std::uint64_t elapsed_us,
                                const ServerSessionRegistry* registry) {
  const char* trace_path = std::getenv("SCRATCHBIRD_PUBLIC_NAME_RESOLUTION_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') return;
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) return;
  out << "layer=server_public_name_resolution"
      << "\toutcome=" << outcome
      << "\tdetail=" << PsNameTraceField(detail)
      << "\tpresented=" << PsNameTraceField(request.presented_name)
      << "\tclass=" << PsNameTraceField(request.object_class)
      << "\tresolved_class=" << PsNameTraceField(object_class)
      << "\tobject_uuid=" << PsNameTraceField(object_uuid)
      << "\tcache_key=" << PsNameTraceField(cache_key)
      << "\tstable_cache_key=" << PsNameTraceField(stable_cache_key)
      << "\tquoted=" << (request.quoted ? "true" : "false")
      << "\tbypass_cache=" << (request.bypass_cache ? "true" : "false")
      << "\tdialect=" << PsNameTraceField(request.dialect_profile)
      << "\tlanguage=" << PsNameTraceField(request.language)
      << "\tsearch_path=" << PsNameTraceField(request.search_path)
      << "\tnormal_cache_checked=" << (normal_cache_checked ? "true" : "false")
      << "\tnormal_cache_hit=" << (normal_cache_hit ? "true" : "false")
      << "\tstable_cache_checked=" << (stable_cache_checked ? "true" : "false")
      << "\tstable_cache_hit=" << (stable_cache_hit ? "true" : "false")
      << "\tstable_cache_entries="
      << (registry == nullptr ? 0 : registry->stable_public_name_resolution_cache_by_key.size())
      << "\tnormal_cache_entries="
      << (registry == nullptr ? 0 : registry->public_name_resolution_cache_by_key.size())
      << "\telapsed_us=" << elapsed_us;
  if (session != nullptr) {
    out << "\tdatabase_uuid=" << PsNameTraceField(session->database_uuid)
        << "\tuser_uuid=" << UuidBytesToText(session->effective_user_uuid)
        << "\tcatalog_generation=" << session->catalog_generation
        << "\tdescriptor_epoch=" << session->descriptor_epoch
        << "\tname_resolution_epoch=" << session->name_resolution_epoch
        << "\tsecurity_epoch=" << session->security_epoch
        << "\tgrant_epoch=" << session->grant_epoch
        << "\tpolicy_generation=" << session->policy_generation
        << "\trole_hash=" << PsNameTraceField(session->role_set_hash)
        << "\tgroup_hash=" << PsNameTraceField(session->group_set_hash)
        << "\tsearch_path_hash=" << PsNameTraceField(session->search_path_hash)
        << "\tlanguage_tag=" << PsNameTraceField(session->language_tag)
        << "\tdefault_language_tag=" << PsNameTraceField(session->default_language_tag);
  }
  out << '\n';
}

std::string PsNameResolutionCacheKey(const ServerSessionRecord& session,
                                     const PsNameResolveRequest& request,
                                     std::string_view identifier_profile) {
  std::ostringstream key;
  key << "db=" << session.database_uuid
      << "|user=" << UuidBytesToText(session.effective_user_uuid)
      << "|presented=" << request.presented_name
      << "|quoted=" << (request.quoted ? "1" : "0")
      << "|class=" << request.object_class
      << "|dialect=" << request.dialect_profile
      << "|identifier_profile=" << identifier_profile
      << "|request_language=" << request.language
      << "|request_search_path=" << request.search_path
      << "|catalog=" << session.catalog_generation
      << "|security=" << session.security_epoch
      << "|descriptor=" << session.descriptor_epoch
      << "|grant=" << session.grant_epoch
      << "|policy=" << session.policy_generation
      << "|name_resolution=" << session.name_resolution_epoch
      << "|role_hash=" << session.role_set_hash
      << "|group_hash=" << session.group_set_hash
      << "|search_path_hash=" << session.search_path_hash
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|language_resource=" << session.language_resource_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::string PsNameStableResolutionCacheKey(const ServerSessionRecord& session,
                                           const PsNameResolveRequest& request,
                                           std::string_view identifier_profile) {
  const bool qualified = request.presented_name.find('.') != std::string_view::npos;
  const std::string_view stable_search_path_hash =
      qualified ? std::string_view("<qualified>") : std::string_view(session.search_path_hash);
  std::ostringstream key;
  key << "stable_relation_v1"
      << "|db=" << session.database_uuid
      << "|user=" << UuidBytesToText(session.effective_user_uuid)
      << "|presented=" << request.presented_name
      << "|quoted=" << (request.quoted ? "1" : "0")
      << "|class=" << request.object_class
      << "|dialect=" << request.dialect_profile
      << "|identifier_profile=" << identifier_profile
      << "|request_language=" << request.language
      << "|request_search_path=" << (qualified ? std::string_view("<qualified>")
                                               : std::string_view(request.search_path))
      << "|security=" << session.security_epoch
      << "|grant=" << session.grant_epoch
      << "|policy=" << session.policy_generation
      << "|role_hash=" << session.role_set_hash
      << "|group_hash=" << session.group_set_hash
      << "|search_path_hash=" << stable_search_path_hash
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|language_resource=" << session.language_resource_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

bool PsNameCachedRecordValid(const ServerPublicNameResolutionCacheRecord& record,
                             const ServerSessionRecord& session) {
  return !record.object_uuid.empty() &&
         record.database_uuid == session.database_uuid &&
         record.effective_user_uuid == session.effective_user_uuid &&
         record.catalog_generation == session.catalog_generation &&
         record.security_epoch == session.security_epoch &&
         record.descriptor_epoch == session.descriptor_epoch &&
         record.grant_epoch == session.grant_epoch &&
         record.policy_generation == session.policy_generation &&
         record.name_resolution_epoch == session.name_resolution_epoch &&
         record.language_resource_epoch == session.language_resource_epoch &&
         record.localized_name_epoch == session.localized_name_epoch &&
         record.message_resource_epoch == session.message_resource_epoch &&
         record.role_set_hash == session.role_set_hash &&
         record.group_set_hash == session.group_set_hash &&
         record.search_path_hash == session.search_path_hash &&
         record.language_profile == session.language_profile &&
         record.language_tag == session.language_tag &&
         record.input_syntax_profile == session.input_syntax_profile &&
         record.input_language_fallback_tag == session.input_language_fallback_tag &&
         record.common_resource_hash == session.common_resource_hash &&
         record.resource_compatibility_identity == session.resource_compatibility_identity &&
         record.resource_version_identity == session.resource_version_identity;
}

bool PsNameStableCachedRecordValid(
    const ServerPublicNameResolutionCacheRecord& record,
    const ServerSessionRecord& session) {
  return !record.object_uuid.empty() &&
         record.database_uuid == session.database_uuid &&
         record.effective_user_uuid == session.effective_user_uuid &&
         record.security_epoch == session.security_epoch &&
         record.grant_epoch == session.grant_epoch &&
         record.policy_generation == session.policy_generation &&
         record.language_resource_epoch == session.language_resource_epoch &&
         record.localized_name_epoch == session.localized_name_epoch &&
         record.message_resource_epoch == session.message_resource_epoch &&
         record.role_set_hash == session.role_set_hash &&
         record.group_set_hash == session.group_set_hash &&
         (record.search_path_hash == "<qualified>" ||
          record.search_path_hash == session.search_path_hash) &&
         record.language_profile == session.language_profile &&
         record.language_tag == session.language_tag &&
         record.input_syntax_profile == session.input_syntax_profile &&
         record.input_language_fallback_tag == session.input_language_fallback_tag &&
         record.common_resource_hash == session.common_resource_hash &&
         record.resource_compatibility_identity == session.resource_compatibility_identity &&
         record.resource_version_identity == session.resource_version_identity;
}

std::optional<ServerPublicNameResolutionCacheRecord> LookupPsNameCache(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    const std::string& cache_key) {
  if (registry == nullptr || cache_key.empty()) return std::nullopt;
  auto found = registry->public_name_resolution_cache_by_key.find(cache_key);
  if (found == registry->public_name_resolution_cache_by_key.end()) {
    return std::nullopt;
  }
  if (!PsNameCachedRecordValid(found->second, session)) {
    registry->public_name_resolution_cache_by_key.erase(found);
    registry->public_name_resolution_cache_lru.erase(
        std::remove(registry->public_name_resolution_cache_lru.begin(),
                    registry->public_name_resolution_cache_lru.end(),
                    cache_key),
        registry->public_name_resolution_cache_lru.end());
    return std::nullopt;
  }
  ++found->second.hit_count;
  registry->public_name_resolution_cache_lru.erase(
      std::remove(registry->public_name_resolution_cache_lru.begin(),
                  registry->public_name_resolution_cache_lru.end(),
                  cache_key),
      registry->public_name_resolution_cache_lru.end());
  registry->public_name_resolution_cache_lru.push_back(cache_key);
  return found->second;
}

std::optional<ServerPublicNameResolutionCacheRecord> LookupPsNameStableCache(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    const std::string& cache_key) {
  if (registry == nullptr || cache_key.empty()) return std::nullopt;
  auto found = registry->stable_public_name_resolution_cache_by_key.find(cache_key);
  if (found == registry->stable_public_name_resolution_cache_by_key.end()) {
    return std::nullopt;
  }
  if (!PsNameStableCachedRecordValid(found->second, session)) {
    registry->stable_public_name_resolution_cache_by_key.erase(found);
    registry->stable_public_name_resolution_cache_lru.erase(
        std::remove(registry->stable_public_name_resolution_cache_lru.begin(),
                    registry->stable_public_name_resolution_cache_lru.end(),
                    cache_key),
        registry->stable_public_name_resolution_cache_lru.end());
    return std::nullopt;
  }
  ++found->second.hit_count;
  registry->stable_public_name_resolution_cache_lru.erase(
      std::remove(registry->stable_public_name_resolution_cache_lru.begin(),
                  registry->stable_public_name_resolution_cache_lru.end(),
                  cache_key),
      registry->stable_public_name_resolution_cache_lru.end());
  registry->stable_public_name_resolution_cache_lru.push_back(cache_key);
  return found->second;
}

void StorePsNameCache(ServerSessionRegistry* registry,
                      const ServerSessionRecord& session,
                      const std::string& cache_key,
                      std::string_view object_uuid,
                      std::string_view canonical_name,
                      std::string_view object_class,
                      std::uint64_t catalog_epoch,
                      std::uint64_t security_epoch) {
  if (registry == nullptr || cache_key.empty() || object_uuid.empty()) return;
  (void)catalog_epoch;
  (void)security_epoch;
  constexpr std::size_t kMaxServerPublicNameResolutionCacheEntries = 8192;
  ServerPublicNameResolutionCacheRecord record;
  record.cache_key = cache_key;
  record.effective_user_uuid = session.effective_user_uuid;
  record.database_uuid = session.database_uuid;
  record.object_uuid = std::string(object_uuid);
  record.canonical_name = canonical_name.empty() ? std::string(object_uuid)
                                                 : std::string(canonical_name);
  record.object_class = std::string(object_class);
  record.catalog_generation = session.catalog_generation;
  record.security_epoch = session.security_epoch;
  record.descriptor_epoch = session.descriptor_epoch;
  record.grant_epoch = session.grant_epoch;
  record.policy_generation = session.policy_generation;
  record.name_resolution_epoch = session.name_resolution_epoch;
  record.language_resource_epoch = session.language_resource_epoch;
  record.localized_name_epoch = session.localized_name_epoch;
  record.message_resource_epoch = session.message_resource_epoch;
  record.role_set_hash = session.role_set_hash;
  record.group_set_hash = session.group_set_hash;
  record.search_path_hash = session.search_path_hash;
  record.language_profile = session.language_profile;
  record.language_tag = session.language_tag;
  record.input_syntax_profile = session.input_syntax_profile;
  record.input_language_fallback_tag = session.input_language_fallback_tag;
  record.common_resource_hash = session.common_resource_hash;
  record.resource_compatibility_identity = session.resource_compatibility_identity;
  record.resource_version_identity = session.resource_version_identity;
  record.generation = registry->next_public_name_resolution_cache_generation++;
  registry->public_name_resolution_cache_by_key[cache_key] = std::move(record);
  registry->public_name_resolution_cache_lru.erase(
      std::remove(registry->public_name_resolution_cache_lru.begin(),
                  registry->public_name_resolution_cache_lru.end(),
                  cache_key),
      registry->public_name_resolution_cache_lru.end());
  registry->public_name_resolution_cache_lru.push_back(cache_key);
  while (registry->public_name_resolution_cache_by_key.size() >
             kMaxServerPublicNameResolutionCacheEntries &&
         !registry->public_name_resolution_cache_lru.empty()) {
    registry->public_name_resolution_cache_by_key.erase(
        registry->public_name_resolution_cache_lru.front());
    registry->public_name_resolution_cache_lru.pop_front();
  }
}

void StorePsNameStableCache(ServerSessionRegistry* registry,
                            const ServerSessionRecord& session,
                            const std::string& cache_key,
                            std::string_view object_uuid,
                            std::string_view canonical_name,
                            std::string_view object_class,
                            std::string_view search_path_hash) {
  if (registry == nullptr || cache_key.empty() || object_uuid.empty()) return;
  constexpr std::size_t kMaxServerStablePublicNameResolutionCacheEntries = 8192;
  ServerPublicNameResolutionCacheRecord record;
  record.cache_key = cache_key;
  record.effective_user_uuid = session.effective_user_uuid;
  record.database_uuid = session.database_uuid;
  record.object_uuid = std::string(object_uuid);
  record.canonical_name = canonical_name.empty() ? std::string(object_uuid)
                                                 : std::string(canonical_name);
  record.object_class = std::string(object_class);
  record.catalog_generation = session.catalog_generation;
  record.security_epoch = session.security_epoch;
  record.descriptor_epoch = session.descriptor_epoch;
  record.grant_epoch = session.grant_epoch;
  record.policy_generation = session.policy_generation;
  record.name_resolution_epoch = session.name_resolution_epoch;
  record.language_resource_epoch = session.language_resource_epoch;
  record.localized_name_epoch = session.localized_name_epoch;
  record.message_resource_epoch = session.message_resource_epoch;
  record.role_set_hash = session.role_set_hash;
  record.group_set_hash = session.group_set_hash;
  record.search_path_hash = search_path_hash.empty() ? session.search_path_hash
                                                     : std::string(search_path_hash);
  record.language_profile = session.language_profile;
  record.language_tag = session.language_tag;
  record.input_syntax_profile = session.input_syntax_profile;
  record.input_language_fallback_tag = session.input_language_fallback_tag;
  record.common_resource_hash = session.common_resource_hash;
  record.resource_compatibility_identity = session.resource_compatibility_identity;
  record.resource_version_identity = session.resource_version_identity;
  record.generation = registry->next_public_name_resolution_cache_generation++;
  registry->stable_public_name_resolution_cache_by_key[cache_key] = std::move(record);
  registry->stable_public_name_resolution_cache_lru.erase(
      std::remove(registry->stable_public_name_resolution_cache_lru.begin(),
                  registry->stable_public_name_resolution_cache_lru.end(),
                  cache_key),
      registry->stable_public_name_resolution_cache_lru.end());
  registry->stable_public_name_resolution_cache_lru.push_back(cache_key);
  while (registry->stable_public_name_resolution_cache_by_key.size() >
             kMaxServerStablePublicNameResolutionCacheEntries &&
         !registry->stable_public_name_resolution_cache_lru.empty()) {
    registry->stable_public_name_resolution_cache_by_key.erase(
        registry->stable_public_name_resolution_cache_lru.front());
    registry->stable_public_name_resolution_cache_lru.pop_front();
  }
}

void StorePsNameCacheVariants(ServerSessionRegistry* registry,
                              const ServerSessionRecord& session,
                              const PsNameResolveRequest& request,
                              std::string_view identifier_profile,
                              const engine_api::EngineRequestContext& context,
                              std::string_view object_uuid,
                              std::string_view canonical_name,
                              std::string_view object_class,
                              std::uint64_t catalog_epoch,
                              std::uint64_t security_epoch) {
  if (registry == nullptr || object_uuid.empty()) return;
  std::vector<PsNameResolveRequest> requests;
  requests.push_back(request);
  if (!object_class.empty() && object_class != request.object_class) {
    auto actual = request;
    actual.object_class = std::string(object_class);
    requests.push_back(std::move(actual));
  }
  if (PsNameRelationLikeObjectClass(object_class) && request.object_class != "relation") {
    auto relation = request;
    relation.object_class = "relation";
    requests.push_back(std::move(relation));
  }
  for (const auto& cache_request : requests) {
    const std::string cache_key =
        PsNameResolutionCacheKey(session, cache_request, identifier_profile);
    StorePsNameCache(registry,
                     session,
                     cache_key,
                     object_uuid,
                     canonical_name,
                     object_class,
                     catalog_epoch,
                     security_epoch);
    if (PsNameStableResolutionCacheable(context, object_class, object_uuid)) {
      const bool qualified =
          cache_request.presented_name.find('.') != std::string_view::npos;
      StorePsNameStableCache(
          registry,
          session,
          PsNameStableResolutionCacheKey(session, cache_request, identifier_profile),
          object_uuid,
          canonical_name,
          object_class,
          qualified ? std::string_view("<qualified>")
                    : std::string_view(session.search_path_hash));
    }
  }
}

std::vector<std::uint8_t> EncodePsNameResolvePayload(std::string_view outcome,
                                                     const std::array<std::uint8_t, 16>& object_uuid,
                                                     std::string_view canonical_name,
                                                     std::string_view object_class,
                                                     std::uint64_t catalog_epoch,
                                                     std::uint64_t security_epoch,
                                                     std::string_view detail) {
  std::vector<std::uint8_t> payload;
  PsNamePutString(&payload, outcome);
  PsNamePutUuid(&payload, object_uuid);
  PsNamePutString(&payload, canonical_name);
  PsNamePutString(&payload, object_class);
  PsNamePutU64(&payload, catalog_epoch);
  PsNamePutU64(&payload, security_epoch);
  PsNamePutString(&payload, detail);
  return payload;
}

std::vector<std::uint8_t> ResolveNamePublicFrame(const sbps::Frame& frame,
                                                 const HostedEngineState& engine_state,
                                                 ServerSessionRegistry* session_registry) {
  const auto trace_begin = std::chrono::steady_clock::now();
  if (!PsNameSessionBound(session_registry, frame.header.session_uuid)) {
    return ErrorFrame({sbps::IpcDiagnostic("PARSER_SERVER_IPC.SESSION_REQUIRED",
                                           "parser_server_ipc.session_required",
                                           "Public name resolution requires a bound server session.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
  }
  if (!PsNameHasOpenDatabase(engine_state)) {
    return ErrorFrame({sbps::IpcDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_UNAVAILABLE",
                                           "parser_server_ipc.attach_database_unavailable",
                                           "No hosted database is available for public name resolution.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
  }
  const auto decoded = DecodePsNameResolveRequest(frame.payload);
  if (!decoded || decoded->presented_name.empty()) {
    return ErrorFrame({sbps::IpcDiagnostic("PARSER_SERVER_IPC.RESOLVE_NAME_INVALID",
                                           "parser_server_ipc.resolve_name_invalid",
                                           "The public name-resolution request is malformed.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
  }
  const auto normalized = PsNameLower(decoded->presented_name);
  const std::string virtual_system_name = PsNameVirtualSystemName(normalized);
  if (!virtual_system_name.empty()) {
    WritePsNameResolutionTrace(*decoded,
                               nullptr,
                               "resolved",
                               "virtual_system_object",
                               virtual_system_name,
                               decoded->object_class.empty() ? "relation" : decoded->object_class,
                               "",
                               "",
                               false,
                               false,
                               false,
                               false,
                               PsNameTraceElapsedMicros(trace_begin),
                               session_registry);
    return PsNameResponseFrame(
        frame,
        static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
        sbps::kSchemaResolveNameResultV1,
        EncodePsNameResolvePayload("resolved",
                                   PsNameSyntheticUuid(virtual_system_name),
                                   virtual_system_name,
                                   decoded->object_class.empty() ? "relation" : decoded->object_class,
                                   1,
                                   1,
                                   "public virtual system object"),
        false);
  }
  std::optional<ServerSessionRecord> session;
  if (session_registry != nullptr) {
    const auto found = session_registry->sessions_by_uuid.find(UuidBytesToText(frame.header.session_uuid));
    if (found != session_registry->sessions_by_uuid.end()) session = found->second;
  }
  const auto parts = PsNameSplitPresentedName(decoded->presented_name, decoded->quoted);
  if (session && parts && !parts->empty()) {
    const std::string identifier_profile =
        PsNameCanonicalIdentifierProfile(decoded->dialect_profile);
    const std::string cache_key =
        PsNameResolutionCacheKey(*session, *decoded, identifier_profile);
    const std::string stable_cache_key =
        PsNameStableResolutionCacheKey(*session, *decoded, identifier_profile);
    bool normal_cache_checked = !decoded->bypass_cache;
    bool normal_cache_hit = false;
    bool stable_cache_checked = false;
    bool stable_cache_hit = false;
    if (!decoded->bypass_cache) {
      if (const auto cached =
              LookupPsNameCache(session_registry, *session, cache_key)) {
        if (const auto object_uuid = PsNameUuidFromText(cached->object_uuid)) {
          normal_cache_hit = true;
          WritePsNameResolutionTrace(*decoded,
                                     &*session,
                                     "resolved",
                                     "normal_cache",
                                     cached->object_uuid,
                                     cached->object_class,
                                     cache_key,
                                     stable_cache_key,
                                     normal_cache_checked,
                                     normal_cache_hit,
                                     stable_cache_checked,
                                     stable_cache_hit,
                                     PsNameTraceElapsedMicros(trace_begin),
                                     session_registry);
          return PsNameResponseFrame(
              frame,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
              sbps::kSchemaResolveNameResultV1,
              EncodePsNameResolvePayload("resolved",
                                         *object_uuid,
                                         cached->canonical_name,
                                         cached->object_class,
                                         cached->catalog_generation,
                                         cached->security_epoch,
                                         "server public name resolution cache"),
              false);
        }
      }
      stable_cache_checked = true;
      if (const auto stable_cached = LookupPsNameStableCache(
              session_registry,
              *session,
              stable_cache_key)) {
        if (const auto object_uuid = PsNameUuidFromText(stable_cached->object_uuid)) {
          stable_cache_hit = true;
          WritePsNameResolutionTrace(*decoded,
                                     &*session,
                                     "resolved",
                                     "stable_relation_cache",
                                     stable_cached->object_uuid,
                                     stable_cached->object_class,
                                     cache_key,
                                     stable_cache_key,
                                     normal_cache_checked,
                                     normal_cache_hit,
                                     stable_cache_checked,
                                     stable_cache_hit,
                                     PsNameTraceElapsedMicros(trace_begin),
                                     session_registry);
          return PsNameResponseFrame(
              frame,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
              sbps::kSchemaResolveNameResultV1,
              EncodePsNameResolvePayload("resolved",
                                         *object_uuid,
                                         stable_cached->canonical_name,
                                         stable_cached->object_class,
                                         session->catalog_generation,
                                         session->security_epoch,
                                         "server stable relation name resolution cache"),
              false);
        }
      }
    }
    engine_api::EngineResolveNameRequest request;
    request.context = PsNameEngineContextFromSession(*session, engine_state, frame, decoded->language);
    request.sql_object_reference.expected_object_type =
        decoded->object_class == "relation" ? std::string{} : decoded->object_class;
    request.sql_object_reference.path_type = parts->size() > 1 ? "qualified" : "unqualified";
    request.sql_object_reference.no_search_path = parts->size() > 1;
    for (std::size_t i = 0; i + 1 < parts->size(); ++i) {
      request.sql_object_reference.path_components.push_back(
          PsNameIdentifierAtom((*parts)[i], identifier_profile));
    }
    request.sql_object_reference.object_name =
        PsNameIdentifierAtom(parts->back(), identifier_profile);
    const auto resolved = engine_api::EngineResolveName(request);
    if (resolved.ok && !resolved.primary_object.uuid.canonical.empty()) {
      const auto object_uuid = PsNameUuidFromText(resolved.primary_object.uuid.canonical);
      if (object_uuid) {
        const auto catalog_epoch =
            resolved.bound_object_identity.catalog_generation_id != 0
                ? resolved.bound_object_identity.catalog_generation_id
                : session->catalog_generation;
        const auto security_epoch =
            resolved.bound_object_identity.security_epoch != 0
                ? resolved.bound_object_identity.security_epoch
                : session->security_epoch;
        const std::string resolved_object_class =
            resolved.primary_object.object_kind.empty()
                ? decoded->object_class
                : resolved.primary_object.object_kind;
        if (!decoded->bypass_cache &&
            PsNameResolutionCacheable(request.context,
                                      resolved_object_class,
                                      resolved.primary_object.uuid.canonical)) {
          StorePsNameCacheVariants(session_registry,
                                   *session,
                                   *decoded,
                                   identifier_profile,
                                   request.context,
                                   resolved.primary_object.uuid.canonical,
                                   decoded->presented_name,
                                   resolved_object_class,
                                   catalog_epoch,
                                   security_epoch);
        }
        WritePsNameResolutionTrace(*decoded,
                                   &*session,
                                   "resolved",
                                   "engine_catalog_resolver",
                                   resolved.primary_object.uuid.canonical,
                                   resolved_object_class,
                                   cache_key,
                                   stable_cache_key,
                                   normal_cache_checked,
                                   normal_cache_hit,
                                   stable_cache_checked,
                                   stable_cache_hit,
                                   PsNameTraceElapsedMicros(trace_begin),
                                   session_registry);
        return PsNameResponseFrame(
            frame,
            static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
            sbps::kSchemaResolveNameResultV1,
            EncodePsNameResolvePayload("resolved",
                                       *object_uuid,
                                       decoded->presented_name,
                                       resolved_object_class,
                                       catalog_epoch,
                                       security_epoch,
                                       "engine catalog resolver"),
            false);
      }
    }
    if (parts->size() == 1) {
      const auto registry_match = PsNameResolveUniqueRegistryLeaf(
          request.context, parts->back(), decoded->object_class, identifier_profile);
      if (registry_match &&
          PsNameRegistryMatchVisibleForSession(request.context, *registry_match)) {
        const auto object_uuid = PsNameUuidFromText(registry_match->object_uuid);
        if (object_uuid) {
          if (!decoded->bypass_cache &&
              PsNameResolutionCacheable(request.context,
                                        registry_match->object_class,
                                        registry_match->object_uuid)) {
            StorePsNameCacheVariants(session_registry,
                                     *session,
                                     *decoded,
                                     identifier_profile,
                                     request.context,
                                     registry_match->object_uuid,
                                     decoded->presented_name,
                                     registry_match->object_class,
                                     registry_match->catalog_generation_id == 0
                                         ? session->catalog_generation
                                         : registry_match->catalog_generation_id,
                                     session->security_epoch);
          }
          WritePsNameResolutionTrace(*decoded,
                                     &*session,
                                     "resolved",
                                     "engine_name_registry_resolver",
                                     registry_match->object_uuid,
                                     registry_match->object_class,
                                     cache_key,
                                     stable_cache_key,
                                     normal_cache_checked,
                                     normal_cache_hit,
                                     stable_cache_checked,
                                     stable_cache_hit,
                                     PsNameTraceElapsedMicros(trace_begin),
                                     session_registry);
          return PsNameResponseFrame(
              frame,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
              sbps::kSchemaResolveNameResultV1,
              EncodePsNameResolvePayload("resolved",
                                         *object_uuid,
                                         decoded->presented_name,
                                         registry_match->object_class,
                                         registry_match->catalog_generation_id == 0
                                             ? session->catalog_generation
                                             : registry_match->catalog_generation_id,
                                         session->security_epoch,
                                         "engine name registry resolver"),
              false);
        }
      }
    }
    WritePsNameResolutionTrace(*decoded,
                               &*session,
                               "not_found_or_not_visible",
                               "public_resolver_returned_no_uuid",
                               "",
                               decoded->object_class,
                               cache_key,
                               stable_cache_key,
                               normal_cache_checked,
                               normal_cache_hit,
                               stable_cache_checked,
                               stable_cache_hit,
                               PsNameTraceElapsedMicros(trace_begin),
                               session_registry);
  }
  if (!session || !parts || parts->empty()) {
    WritePsNameResolutionTrace(*decoded,
                               nullptr,
                               "not_found_or_not_visible",
                               "missing_session_or_invalid_parts",
                               "",
                               decoded->object_class,
                               "",
                               "",
                               false,
                               false,
                               false,
                               false,
                               PsNameTraceElapsedMicros(trace_begin),
                               session_registry);
  }
  return PsNameResponseFrame(
      frame,
      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
      sbps::kSchemaResolveNameResultV1,
      EncodePsNameResolvePayload("not_found_or_not_visible",
                                 {},
                                 "",
                                 decoded->object_class,
                                 1,
                                 1,
                                 "public resolver returned no UUID"),
      false);
}

std::vector<std::uint8_t> RenderUuidPublicFrame(const sbps::Frame& frame,
                                                const ServerSessionRegistry* session_registry) {
  if (!PsNameSessionBound(session_registry, frame.header.session_uuid)) {
    return ErrorFrame({sbps::IpcDiagnostic("PARSER_SERVER_IPC.SESSION_REQUIRED",
                                           "parser_server_ipc.session_required",
                                           "Public UUID rendering requires a bound server session.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidResult));
  }
  if (frame.payload.size() < 16) {
    return ErrorFrame({sbps::IpcDiagnostic("PARSER_SERVER_IPC.RENDER_UUID_INVALID",
                                           "parser_server_ipc.render_uuid_invalid",
                                           "The public UUID-rendering request is malformed.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidResult));
  }
  const auto uuid = PsNameGetUuid(frame.payload, 0);
  const auto name = PsNameRenderedVirtualSystemName(uuid);
  if (!name) {
    return PsNameResponseFrame(
        frame,
        static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidResult),
        sbps::kSchemaRenderUuidResultV1,
        EncodePsNameResolvePayload("not_found_or_not_visible",
                                   {},
                                   "",
                                   "",
                                   1,
                                   1,
                                   "public renderer returned no name"),
        false);
  }
  return PsNameResponseFrame(
      frame,
      static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidResult),
      sbps::kSchemaRenderUuidResultV1,
      EncodePsNameResolvePayload("rendered",
                                 uuid,
                                 *name,
                                 "relation",
                                 1,
                                 1,
                                 "public virtual system object"),
      false);
}

struct IparProjectionSourceContext {
  ServerAgentRuntime* agent_runtime = nullptr;
  ServerObservabilityState* observability = nullptr;
};

ServerIparProjectionSources BuildIparProjectionSourcesForServer(void* opaque_context) {
  ServerIparProjectionSources sources;
  auto* context = static_cast<IparProjectionSourceContext*>(opaque_context);
  if (context == nullptr) {
    return sources;
  }
  if (context->agent_runtime != nullptr) {
    sources.agent_lifecycle.push_back(
        BuildIparAgentLifecycleProjectionSource(context->agent_runtime->Snapshot()));
  }
  if (context->observability != nullptr) {
    sources.metric_counters =
        BuildIparMetricCounterProjectionSources(*context->observability);
    sources.telemetry_controls =
        BuildIparTelemetryControlProjectionSources(*context->observability);
    sources.slow_path_reasons =
        BuildIparSlowPathReasonProjectionSources(*context->observability);
  }
  return sources;
}

bool HandleClientFrame(IpcSocketHandle client_fd,
                       const ServerBootstrapConfig& config,
                       const ServerLifecycleArtifacts& artifacts,
                       const HostedEngineState& engine_state,
                       ServerSessionRegistry* session_registry,
                       const ParserPackageRegistry& parser_registry,
                       ParserEventNotificationRouter* event_router,
                       ServerListenerOrchestrator* listener_orchestrator,
                       ServerMaintenanceCoordinator* maintenance_coordinator,
                       ServerAgentRuntime* agent_runtime,
                       ServerObservabilityState* observability,
                       bool* release_heap_after_close) {
  sbps::Frame frame;
  std::vector<ServerDiagnostic> frame_diagnostics;
  if (!ReadPhysicalFrame(client_fd, config, &frame, &frame_diagnostics) ||
      !AssembleChunkedFrame(client_fd, config, &frame, &frame_diagnostics)) {
    IncrementServerMetric(observability,
                          "sys.metrics.ipc.parser_server.frame.invalid_total",
                          1,
                          {{"reason", frame_diagnostics.empty() ? "decode" : frame_diagnostics.front().code}});
    RecordServerAuditEvent(observability,
                           "server.ipc.frame_decode",
                           "refused",
                           "invalid parser-server IPC frame",
                           frame_diagnostics.empty() ? "" : frame_diagnostics.front().code);
    WriteAll(client_fd, ErrorFrame(frame_diagnostics, frame.header.request_uuid, frame.header.sequence_number));
    return false;
  }
  const bool session_bound_message =
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kResolveNameRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kFetch) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventSubscribeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventUnsubscribeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventAck) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventNotification) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventBackpressure) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventSubscriptionInvalidate) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventChannelClosed);
  if (!sbps::IsZeroUuid(frame.header.session_uuid) && !session_bound_message) {
    WriteAll(client_fd, ErrorFrame(
                           {sbps::IpcDiagnostic("PARSER_SERVER_IPC.SESSION_BOUND_TOO_EARLY",
                                                "parser_server_ipc.session_bound_too_early",
                                                "A pre-authentication SBPS frame carried a session UUID.")},
                           frame.header.request_uuid, frame.header.sequence_number));
    return false;
  }
  if (frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kHello)) {
    auto hello = sbps::DecodeHelloRequest(frame.payload);
    if (hello && sbps::HasUnknownCapabilityBits(hello->capability_bitmap)) {
      WriteAll(client_fd, ErrorFrame(
                             {sbps::IpcDiagnostic("PARSER_SERVER_IPC.FEATURE_UNKNOWN_REQUIRED",
                                                  "parser_server_ipc.feature_unknown_required",
                                                  "The parser hello advertised an unknown required capability bit.")},
                             frame.header.request_uuid, frame.header.sequence_number,
                             static_cast<std::uint16_t>(sbps::MessageType::kHelloReject)));
      IncrementServerMetric(observability,
                            "sys.metrics.ipc.parser_server.channel.open_total",
                            1,
                            {{"parser_family_uuid", "unknown"}, {"outcome", "rejected"}});
      RecordServerAuditEvent(observability,
                             "server.parser.hello",
                             "rejected",
                             "parser hello rejected",
                             "PARSER_SERVER_IPC.FEATURE_UNKNOWN_REQUIRED");
      return false;
    }
    if (!hello || !sbps::IsBuiltInTestHello(*hello)) {
      WriteAll(client_fd, ErrorFrame(
                             {sbps::IpcDiagnostic("PARSER_SERVER_IPC.PARSER_PROFILE_MISMATCH",
                                                  "parser_server_ipc.parser_profile_mismatch",
                                                  "The parser profile is not accepted by this endpoint.")},
                             frame.header.request_uuid, frame.header.sequence_number,
                             static_cast<std::uint16_t>(sbps::MessageType::kHelloReject)));
      IncrementServerMetric(observability,
                            "sys.metrics.ipc.parser_server.channel.open_total",
                            1,
                            {{"parser_family_uuid", "unknown"}, {"outcome", "rejected"}});
      RecordServerAuditEvent(observability,
                             "server.parser.hello",
                             "rejected",
                             "parser hello rejected",
                             "PARSER_SERVER_IPC.PARSER_PROFILE_MISMATCH");
      return false;
    }
    const auto admission = AdmitParserPackage(
        parser_registry, *hello, frame.header.protocol_major, frame.header.protocol_minor);
    if (!admission.admitted) {
      WriteAll(client_fd, ErrorFrame(
                             admission.diagnostics.empty()
                                 ? std::vector<ServerDiagnostic>{sbps::IpcDiagnostic(
                                       "SERVER.PARSER.PACKAGE_REJECTED",
                                       "server.parser.package_rejected",
                                       "The parser package was rejected by registry policy.")}
                                 : admission.diagnostics,
                             frame.header.request_uuid, frame.header.sequence_number,
                             static_cast<std::uint16_t>(sbps::MessageType::kHelloReject)));
      IncrementServerMetric(observability,
                            "sys.metrics.ipc.parser_server.channel.open_total",
                            1,
                            {{"parser_family_uuid", "registry"}, {"outcome", "rejected"}});
      RecordServerAuditEvent(observability,
                             "server.parser.hello",
                             "rejected",
                             "parser package admission rejected",
                             admission.diagnostics.empty() ? "SERVER.PARSER.PACKAGE_REJECTED" : admission.diagnostics.front().code);
      return false;
    }
    IncrementServerMetric(observability,
                          "sys.metrics.ipc.parser_server.channel.open_total",
                          1,
                          {{"parser_family_uuid", "accepted"}, {"outcome", "accepted"}});
    RecordServerAuditEvent(observability,
                           "server.parser.hello",
                           "accepted",
                           "parser package admitted");
    WriteAll(client_fd, AcceptFrame(frame, config));
    return true;
  }
  if (frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kPing)) {
    const std::string request_text(frame.payload.begin(), frame.payload.end());
    if (request_text == "session_registry_status") {
      const auto status = SessionRegistryStatusJson(*session_registry);
      sbps::FrameHeader header;
      header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPong);
      header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
      header.payload_schema_id = frame.header.payload_schema_id;
      header.stream_id = frame.header.stream_id;
      header.sequence_number = frame.header.sequence_number;
      header.request_uuid = frame.header.request_uuid;
      std::vector<std::uint8_t> status_payload(status.begin(), status.end());
      WriteAll(client_fd, sbps::EncodeFrame(header, status_payload));
    } else if (request_text == "parser_registry_status") {
      const auto status = ParserPackageRegistryStatusJson(parser_registry);
      sbps::FrameHeader header;
      header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPong);
      header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
      header.payload_schema_id = frame.header.payload_schema_id;
      header.stream_id = frame.header.stream_id;
      header.sequence_number = frame.header.sequence_number;
      header.request_uuid = frame.header.request_uuid;
      std::vector<std::uint8_t> status_payload(status.begin(), status.end());
      WriteAll(client_fd, sbps::EncodeFrame(header, status_payload));
    } else if (request_text == "notification_router_status") {
      std::ostringstream status;
      status << "{\"notification_router\":{\"active_subscriptions\":"
             << event_router->ActiveSubscriptionCount() << "}}\n";
      sbps::FrameHeader header;
      header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPong);
      header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
      header.payload_schema_id = frame.header.payload_schema_id;
      header.stream_id = frame.header.stream_id;
      header.sequence_number = frame.header.sequence_number;
      header.request_uuid = frame.header.request_uuid;
      const auto text = status.str();
      std::vector<std::uint8_t> status_payload(text.begin(), text.end());
      WriteAll(client_fd, sbps::EncodeFrame(header, status_payload));
    } else if (request_text == "listener_orchestrator_status") {
      const auto status = ListenerOrchestratorStatusJson(*listener_orchestrator);
      sbps::FrameHeader header;
      header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPong);
      header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
      header.payload_schema_id = frame.header.payload_schema_id;
      header.stream_id = frame.header.stream_id;
      header.sequence_number = frame.header.sequence_number;
      header.request_uuid = frame.header.request_uuid;
      std::vector<std::uint8_t> status_payload(status.begin(), status.end());
      WriteAll(client_fd, sbps::EncodeFrame(header, status_payload));
    } else if (request_text == "server_management_rights") {
      const auto status = ServerManagementRightsMatrixJson();
      sbps::FrameHeader header;
      header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPong);
      header.flags = sbps::kFlagResponse | sbps::kFlagFinal;
      header.payload_schema_id = frame.header.payload_schema_id;
      header.stream_id = frame.header.stream_id;
      header.sequence_number = frame.header.sequence_number;
      header.request_uuid = frame.header.request_uuid;
      std::vector<std::uint8_t> status_payload(status.begin(), status.end());
      WriteAll(client_fd, sbps::EncodeFrame(header, status_payload));
    } else {
      WriteAll(client_fd, PongFrame(frame, engine_state));
    }
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kAuthHandoff)) {
    const auto result = HandleAuthHandoff(session_registry, engine_state, frame);
    IncrementServerMetric(observability,
                          "sys.metrics.ipc.parser_server.auth.latency_microseconds",
                          1,
                          {{"provider_class", "local"}, {"outcome", result.accepted ? "accepted" : "rejected"}});
    RecordServerAuditEvent(observability,
                           "server.auth_handoff",
                           result.accepted ? "accepted" : "rejected",
                           "authentication handoff processed",
                           result.diagnostics.empty() ? "" : result.diagnostics.front().code);
    WriteAll(client_fd, SessionOperationFrame(frame, result));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kAttachDatabase)) {
    if (maintenance_coordinator != nullptr && !MaintenanceAllowsAttach(*maintenance_coordinator)) {
      WriteAll(client_fd, ErrorFrame(
                             {MaintenanceAdmissionDiagnostic(*maintenance_coordinator,
                                                             "attach_database",
                                                             "attach_admission_fenced")},
                             frame.header.request_uuid, frame.header.sequence_number,
                             static_cast<std::uint16_t>(sbps::MessageType::kAttachResult)));
      RecordServerAuditEvent(observability,
                             "server.attach_database",
                             "refused",
                             "database attach refused by maintenance coordinator",
                             "SERVER.MAINTENANCE.ADMISSION_DENIED");
      return true;
    }
    const auto result = HandleAttachDatabase(session_registry, engine_state, frame);
    SetServerMetric(observability,
                    "sys.metrics.server.session.active",
                    static_cast<std::uint64_t>(session_registry->sessions_by_uuid.size()));
    RecordServerAuditEvent(observability,
                           "server.attach_database",
                           result.accepted ? "accepted" : "rejected",
                           "database attach processed",
                           result.diagnostics.empty() ? "" : result.diagnostics.front().code);
    WriteAll(client_fd, SessionOperationFrame(frame, result));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice)) {
    if (release_heap_after_close != nullptr) {
      *release_heap_after_close = true;
    }
    if (const auto event_session = EventSessionFromFrame(session_registry, engine_state, frame)) {
      ParserServerEventIpcRuntime runtime(event_router);
      PsEventDisconnectRequest disconnect;
      disconnect.session = *event_session;
      disconnect.disconnect_reason = "parser_disconnect_notice";
      runtime.HandleDisconnect(disconnect);
    }
    const auto result = HandleDisconnectNotice(session_registry, frame);
    SetServerMetric(observability,
                    "sys.metrics.server.session.active",
                    static_cast<std::uint64_t>(session_registry->sessions_by_uuid.size()));
    RecordServerAuditEvent(observability,
                           "server.disconnect_notice",
                           result.accepted ? "completed" : "not_found",
                           "parser disconnect notice processed");
    WriteAll(client_fd, SessionOperationFrame(frame, result));
    return false;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameRequest)) {
    WriteAll(client_fd, ResolveNamePublicFrame(frame, engine_state, session_registry));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidRequest)) {
    WriteAll(client_fd, RenderUuidPublicFrame(frame, session_registry));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest)) {
    ServerManagementContext context;
    context.config = &config;
    context.artifacts = &artifacts;
    context.engine_state = &engine_state;
    context.session_registry = session_registry;
    context.parser_registry = &parser_registry;
    context.listener_orchestrator = listener_orchestrator;
    context.maintenance_coordinator = maintenance_coordinator;
    context.observability = observability;
    WriteAll(client_fd, ManagementOperationFrame(
                           frame, HandleServerManagementRequest(context, frame)));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr)) {
    if (maintenance_coordinator != nullptr && !MaintenanceAllowsSblr(*maintenance_coordinator)) {
      WriteAll(client_fd, ErrorFrame(
                             {MaintenanceAdmissionDiagnostic(*maintenance_coordinator,
                                                             "prepare_sblr",
                                                             "sblr_admission_fenced")},
                             frame.header.request_uuid, frame.header.sequence_number,
                             static_cast<std::uint16_t>(sbps::MessageType::kPrepareResult)));
      return true;
    }
    WriteAll(client_fd, SessionOperationFrame(
                           frame, HandlePrepareSblr(session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr)) {
    if (maintenance_coordinator != nullptr && !MaintenanceAllowsSblr(*maintenance_coordinator)) {
      WriteAll(client_fd, ErrorFrame(
                             {MaintenanceAdmissionDiagnostic(*maintenance_coordinator,
                                                             "execute_sblr",
                                                             "sblr_admission_fenced")},
                             frame.header.request_uuid, frame.header.sequence_number,
                             static_cast<std::uint16_t>(sbps::MessageType::kExecuteResult)));
      return true;
    }
    IparProjectionSourceContext ipar_context{agent_runtime, observability};
    ServerIparProjectionSourceFactory ipar_factory;
    ipar_factory.context = &ipar_context;
    ipar_factory.build = &BuildIparProjectionSourcesForServer;
    const auto operation = HandleExecuteSblr(session_registry,
                                             engine_state,
                                             frame,
                                             &ipar_factory);
    IncrementServerMetric(observability,
                          "sys.metrics.ipc.parser_server.sblr.execute_microseconds",
                          1,
                          {{"operation_family", "sblr"}, {"outcome", operation.accepted ? "accepted" : "rejected"}});
    RecordServerAuditEvent(observability,
                           "server.sblr.execute",
                           operation.accepted ? "completed" : "rejected",
                           "SBLR execute processed",
                           operation.diagnostics.empty() ? "" : operation.diagnostics.front().code);
    WriteAll(client_fd, SessionOperationFrame(frame, operation));
    if (operation.accepted) {
      PumpEventNotifications(client_fd, frame, engine_state, session_registry, event_router);
    }
    return true;
  }
  if (frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventSubscribeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventUnsubscribeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventAck) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventNotification) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventBackpressure) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventSubscriptionInvalidate) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kEventChannelClosed)) {
    if (maintenance_coordinator != nullptr && !MaintenanceAllowsEvents(*maintenance_coordinator)) {
      WriteAll(client_fd, ErrorFrame(
                             {MaintenanceAdmissionDiagnostic(*maintenance_coordinator,
                                                             "event_ipc",
                                                             "event_admission_fenced")},
                             frame.header.request_uuid, frame.header.sequence_number));
      return true;
    }
    HandleEventFrame(client_fd, frame, engine_state, session_registry, event_router);
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kFetch)) {
    WriteAll(client_fd, SessionOperationFrame(frame, HandleFetch(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor)) {
    WriteAll(client_fd, SessionOperationFrame(frame, HandleCloseCursor(session_registry, frame)));
    return true;
  }
  WriteAll(client_fd, ErrorFrame(
                         {sbps::IpcDiagnostic("PARSER_SERVER_IPC.MESSAGE_TYPE_UNSUPPORTED",
                                              "parser_server_ipc.message_type_unsupported",
                                              "The SBPS message type is not supported in this server stage.")},
                         frame.header.request_uuid, frame.header.sequence_number));
  return false;
}

}  // namespace

std::vector<std::uint8_t> ResolveNamePublicFrameForEmbedded(
    const sbps::Frame& frame,
    const HostedEngineState& engine_state,
    ServerSessionRegistry* session_registry) {
  return ResolveNamePublicFrame(frame, engine_state, session_registry);
}

std::vector<std::uint8_t> RenderUuidPublicFrameForEmbedded(
    const sbps::Frame& frame,
    const ServerSessionRegistry* session_registry) {
  return RenderUuidPublicFrame(frame, session_registry);
}

ServerIpcEndpointResult RunParserServerIpcEndpoint(const ServerBootstrapConfig& config,
                                                   const ServerLifecycleArtifacts& artifacts,
                                                   const HostedEngineState& engine_state) {
  ServerIpcEndpointResult result;
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    result.exit_code = 2;
    result.diagnostics.push_back(EndpointDiagnostic(
        "PARSER_SERVER_IPC.TRANSPORT_UNAVAILABLE",
        "Winsock initialization failed for the SBPS endpoint.",
        {{"error", LastIpcSocketErrorString()}}));
    return result;
  }
#endif
  std::signal(SIGTERM, HandleStopSignal);
  std::signal(SIGINT, HandleStopSignal);

  std::error_code ec;
  std::filesystem::create_directories(config.sbps_endpoint.parent_path(), ec);
  if (ec) {
    result.exit_code = 2;
    result.diagnostics.push_back(EndpointDiagnostic(
        "PARSER_SERVER_IPC.ENDPOINT_CREATE_FAILED",
        "The SBPS endpoint directory could not be created.",
        {{"endpoint", config.sbps_endpoint.string()}}));
    return result;
  }

#ifdef _WIN32
  const auto endpoint = PlatformEndpointPath(config.sbps_endpoint);
  RemoveEndpointPath(endpoint);
#else
  struct stat existing {};
  if (::lstat(config.sbps_endpoint.c_str(), &existing) == 0) {
    if (S_ISSOCK(existing.st_mode)) {
      ::unlink(config.sbps_endpoint.c_str());
    } else {
      result.exit_code = 2;
      result.diagnostics.push_back(EndpointDiagnostic(
          "PARSER_SERVER_IPC.ENDPOINT_BUSY",
          "The SBPS endpoint path exists and is not a socket.",
          {{"endpoint", config.sbps_endpoint.string()}}));
      return result;
    }
  }
  const auto endpoint = PlatformEndpointPath(config.sbps_endpoint);
#endif

  const IpcSocketHandle server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd == kInvalidIpcSocket) {
    result.exit_code = 2;
    result.diagnostics.push_back(EndpointDiagnostic(
        "PARSER_SERVER_IPC.ENDPOINT_CREATE_FAILED",
        "The SBPS socket could not be created.",
        {{"error", LastIpcSocketErrorString()}}));
    return result;
  }

  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(addr.sun_path)) {
    CloseIpcSocket(server_fd);
    result.exit_code = 2;
    result.diagnostics.push_back(EndpointDiagnostic(
        "PARSER_SERVER_IPC.ENDPOINT_NAME_INVALID",
        "The SBPS endpoint path is too long for AF_UNIX.",
        {{"endpoint", endpoint}}));
    return result;
  }
  std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseIpcSocket(server_fd);
    result.exit_code = 2;
    result.diagnostics.push_back(EndpointDiagnostic(
        "PARSER_SERVER_IPC.ENDPOINT_BIND_FAILED",
        "The SBPS endpoint could not be bound.",
        {{"endpoint", endpoint}, {"error", LastIpcSocketErrorString()}}));
    return result;
  }
#ifndef _WIN32
  ::chmod(endpoint.c_str(), 0600);
#endif
  if (::listen(server_fd, 16) != 0) {
    CloseIpcSocket(server_fd);
    RemoveEndpointPath(endpoint);
    result.exit_code = 2;
    result.diagnostics.push_back(EndpointDiagnostic(
        "PARSER_SERVER_IPC.ENDPOINT_LISTEN_FAILED",
        "The SBPS endpoint could not listen.",
        {{"endpoint", endpoint}, {"error", LastIpcSocketErrorString()}}));
    return result;
  }
  if (!WriteEndpointDescriptor(config, engine_state, artifacts, &result.diagnostics)) {
    CloseIpcSocket(server_fd);
    RemoveEndpointPath(endpoint);
    result.exit_code = 2;
    return result;
  }
  ServerSessionRegistry session_registry;
  const ParserPackageRegistry parser_registry = LoadParserPackageRegistry(config);
  ServerAgentRuntime agent_runtime;
  if (!agent_runtime.Start(config, engine_state, &result.diagnostics)) {
    result.exit_code = 2;
    CloseIpcSocket(server_fd);
    RemoveEndpointPath(endpoint);
    return result;
  }
  ParserEventNotificationRouter event_router;
  ServerListenerOrchestrator listener_orchestrator = BuildListenerOrchestrator(config, artifacts);
  const auto listener_start = StartEnabledServerListeners(&listener_orchestrator, config, artifacts);
  if (!listener_start.ok) {
    result.exit_code = 2;
    result.diagnostics = listener_start.diagnostics;
    agent_runtime.Stop();
    CloseIpcSocket(server_fd);
    RemoveEndpointPath(endpoint);
    return result;
  }
  const auto daemon_lifecycle =
      EvaluateServerDaemonLifecycle(config, artifacts, engine_state);
  if (!daemon_lifecycle.diagnostics.empty()) {
    result.exit_code = 2;
    result.diagnostics = daemon_lifecycle.diagnostics;
    agent_runtime.Stop();
    CloseIpcSocket(server_fd);
    RemoveEndpointPath(endpoint);
    StopManagedServerListeners(&listener_orchestrator, "force");
    return result;
  }
  WriteServingState(config, artifacts, daemon_lifecycle);
  ServerMaintenanceCoordinator maintenance_coordinator = BuildMaintenanceCoordinator(config, artifacts);
  ServerObservabilityState observability =
      InitializeServerObservability(config, artifacts, engine_state, parser_registry, listener_orchestrator);
  std::mutex client_dispatch_mutex;
  std::vector<std::thread> client_threads;

  while (!g_stop_requested.load()) {
#ifdef _WIN32
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(server_fd, &read_set);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    const int poll_rc = ::select(0, &read_set, nullptr, nullptr, &timeout);
    if (poll_rc < 0) {
      if (IpcSocketInterrupted()) continue;
      result.exit_code = 2;
      result.diagnostics.push_back(EndpointDiagnostic(
          "PARSER_SERVER_IPC.ACCEPT_FAILED",
          "The SBPS endpoint polling operation failed.",
          {{"error", LastIpcSocketErrorString()}}));
      break;
    }
    if (poll_rc == 0 || !FD_ISSET(server_fd, &read_set)) {
      continue;
    }
#else
    pollfd listener {};
    listener.fd = server_fd;
    listener.events = POLLIN;
    const int poll_rc = ::poll(&listener, 1, 100);
    if (poll_rc < 0) {
      if (errno == EINTR) continue;
      result.exit_code = 2;
      result.diagnostics.push_back(EndpointDiagnostic(
          "PARSER_SERVER_IPC.ACCEPT_FAILED",
          "The SBPS endpoint polling operation failed.",
          {{"error", LastIpcSocketErrorString()}}));
      break;
    }
    if (poll_rc == 0) {
      continue;
    }
    if ((listener.revents & (POLLERR | POLLNVAL)) != 0) {
      result.exit_code = 2;
      result.diagnostics.push_back(EndpointDiagnostic(
          "PARSER_SERVER_IPC.ACCEPT_FAILED",
          "The SBPS endpoint listener socket entered a failed polling state.",
          {{"poll_revents", std::to_string(listener.revents)}}));
      break;
    }
    if ((listener.revents & POLLIN) == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
#endif
    const IpcSocketHandle client_fd = ::accept(server_fd, nullptr, nullptr);
    if (client_fd == kInvalidIpcSocket) {
      if (IpcSocketInterrupted()) continue;
      result.exit_code = 2;
      result.diagnostics.push_back(EndpointDiagnostic(
          "PARSER_SERVER_IPC.ACCEPT_FAILED",
          "The SBPS endpoint accept operation failed.",
          {{"error", LastIpcSocketErrorString()}}));
      break;
    }
    client_threads.emplace_back([client_fd,
                                 &config,
                                 &artifacts,
                                 &engine_state,
                                 &session_registry,
                                 &parser_registry,
                                 &event_router,
                                 &listener_orchestrator,
                                 &maintenance_coordinator,
                                 &agent_runtime,
                                 &observability,
                                 &client_dispatch_mutex]() {
      bool release_heap_after_close = false;
      while (!g_stop_requested.load()) {
        if (!ClientSocketReady(client_fd)) {
          continue;
        }
        bool keep_open = false;
        {
          std::lock_guard<std::mutex> dispatch_guard(client_dispatch_mutex);
          keep_open = HandleClientFrame(client_fd,
                                        config,
                                        artifacts,
                                        engine_state,
                                        &session_registry,
                                        parser_registry,
                                        &event_router,
                                        &listener_orchestrator,
                                        &maintenance_coordinator,
                                        &agent_runtime,
                                        &observability,
                                        &release_heap_after_close);
          if (maintenance_coordinator.shutdown_requested) {
            g_stop_requested.store(true);
          }
        }
        if (!keep_open) {
          break;
        }
      }
      CloseIpcSocket(client_fd);
      if (release_heap_after_close) {
        std::lock_guard<std::mutex> dispatch_guard(client_dispatch_mutex);
        ReleaseIdleConnectionHeap(config, &observability);
      }
    });
  }

  g_stop_requested.store(true);
  for (auto& client_thread : client_threads) {
    if (client_thread.joinable()) {
      client_thread.join();
    }
  }
  CloseIpcSocket(server_fd);
  RemoveEndpointPath(endpoint);
  agent_runtime.Stop();
  const auto listener_stop = StopManagedServerListeners(&listener_orchestrator, "graceful");
  if (!listener_stop.diagnostics.empty()) {
    result.diagnostics.insert(result.diagnostics.end(),
                              listener_stop.diagnostics.begin(),
                              listener_stop.diagnostics.end());
  }
  RecordServerAuditEvent(&observability, "server.shutdown", "completed", "parser-server IPC endpoint stopped");
  RecordServerLog(&observability, {"server.shutdown", "info", "sb_server", {}, "parser-server IPC endpoint stopped", "clean"});
  const auto flush = FlushServerObservability(&observability, "server_shutdown");
  if (!flush.flushed) {
    result.diagnostics.push_back(EndpointDiagnostic(
        flush.diagnostic_code.empty() ? "OPS.EVIDENCE.FLUSH_FAILED" : flush.diagnostic_code,
        "Server observability evidence did not flush cleanly during shutdown."));
  }
  const auto stopped = WriteStoppedLifecycleArtifacts(config, artifacts.generation);
  if (!stopped.diagnostics.empty()) {
    result.diagnostics.insert(result.diagnostics.end(),
                              stopped.diagnostics.begin(),
                              stopped.diagnostics.end());
  }
  result.exit_code = result.diagnostics.empty() ? 0 : 2;
  return result;
}

}  // namespace scratchbird::server
