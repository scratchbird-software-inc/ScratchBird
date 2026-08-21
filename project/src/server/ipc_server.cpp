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
#include "catalog/global_aggregate_view.hpp"
#include "catalog/relation_projection_view.hpp"
#include "catalog/relation_descriptor_projection.hpp"
#include "catalog/sys_information_projection.hpp"
#include "dml/global_aggregate_projection.hpp"
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
#include <set>
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
  g_stop_requested.store(true, std::memory_order_release);
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

struct ClientNegotiationState {
  bool hello_admitted = false;
  bool connection_authenticated = false;
  std::array<std::uint8_t, 16> server_channel_uuid{};
  std::array<std::uint8_t, 32> accepted_capability_bitmap{};
  std::uint16_t admitted_protocol_major = 0;
  std::uint16_t admitted_protocol_minor = 0;
  std::vector<std::uint8_t> admitted_hello_payload;
};

std::vector<std::uint8_t> AcceptFrame(
    const sbps::Frame& request,
    const ServerBootstrapConfig& config,
    ClientNegotiationState* negotiation_state) {
  sbps::HelloAccept accept;
  accept.server_uuid = sbps::MakeUuidV7Bytes();
  accept.channel_uuid =
      negotiation_state != nullptr && negotiation_state->hello_admitted
          ? negotiation_state->server_channel_uuid
          : sbps::MakeUuidV7Bytes();
  accept.max_frame_bytes = static_cast<std::uint32_t>(config.sbps_max_frame_bytes);
  accept.max_streams = static_cast<std::uint32_t>(config.sbps_max_streams);
  if (const auto hello = sbps::DecodeHelloRequest(request.payload)) {
    if (negotiation_state != nullptr && negotiation_state->hello_admitted) {
      accept.accepted_capability_bitmap =
          negotiation_state->accepted_capability_bitmap;
    } else {
      accept.accepted_capability_bitmap[0] =
          hello->capability_bitmap[0] & sbps::kKnownCapabilityByte0;
    }
  }
  if (negotiation_state != nullptr) {
    const bool first_hello = !negotiation_state->hello_admitted;
    negotiation_state->hello_admitted = true;
    negotiation_state->server_channel_uuid = accept.channel_uuid;
    negotiation_state->accepted_capability_bitmap =
        accept.accepted_capability_bitmap;
    if (first_hello) {
      negotiation_state->admitted_protocol_major =
          request.header.protocol_major;
      negotiation_state->admitted_protocol_minor =
          request.header.protocol_minor;
      negotiation_state->admitted_hello_payload = request.payload;
    }
  }
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
  event_session.draining =
      session->channel_state == ServerChannelState::kDraining;
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

void PsNamePutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
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

bool PsNameReadU64(const std::vector<std::uint8_t>& data,
                   std::size_t* offset,
                   std::uint64_t* out) {
  if (offset == nullptr || out == nullptr || *offset + 8 > data.size()) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[*offset + index]) << (index * 8u);
  }
  *offset += 8;
  *out = value;
  return true;
}

bool PsNameReadString(const std::vector<std::uint8_t>& data,
                      std::size_t* offset,
                      std::string* out) {
  if (offset == nullptr || out == nullptr || *offset > data.size() ||
      data.size() - *offset < 2) {
    return false;
  }
  const auto length = PsNameGetU16(data, *offset);
  *offset += 2;
  if (*offset > data.size() || length > data.size() - *offset) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

bool PsNameReadBoundedString(const std::vector<std::uint8_t>& data,
                             std::size_t* offset,
                             std::string* out,
                             std::size_t max_bytes) {
  if (offset == nullptr || out == nullptr || *offset > data.size() ||
      data.size() - *offset < 2) {
    return false;
  }
  const auto length = PsNameGetU16(data, *offset);
  *offset += 2;
  if (length > max_bytes || *offset > data.size() ||
      length > data.size() - *offset) {
    return false;
  }
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
    std::string_view identifier_profile,
    bool require_transaction_context) {
  auto load_context = context;
  const std::uint64_t observer_tx =
      context.snapshot_visible_through_local_transaction_id != 0
          ? context.snapshot_visible_through_local_transaction_id
          : context.local_transaction_id;
  auto loaded = engine_api::LoadNameRegistryState(load_context, observer_tx);
  if (!loaded.ok && load_context.local_transaction_id != 0 &&
      !require_transaction_context) {
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
  if (object_class == "charset" || object_class == "collation") return false;
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
  std::array<std::uint8_t, 16> session_uuid{};
  std::string presented_name;
  bool quoted = false;
  std::string dialect_profile;
  std::string language;
  std::string search_path;
  std::string object_class;
  bool bypass_cache = false;
  bool transaction_routed = false;
  std::uint64_t local_transaction_id = 0;
  std::string transaction_uuid;
  std::uint8_t projection_flags = 0;
  bool include_persisted_relation_descriptor = false;
};

constexpr std::uint8_t kPsNameProjectionRelationDescriptorV1 = 0x01u;
constexpr std::uint8_t kPsRelationDescriptorExtensionKind = 0x02u;
constexpr std::uint8_t kPsRelationDescriptorExtensionVersion = 0x02u;
constexpr std::size_t kMaxPsRelationProjectionBytes = 512u * 1024u;
constexpr std::uint32_t kMaxPsRelationProjectionColumns = 4096;
constexpr std::size_t kMaxPsRelationMetadataTextBytes = 4096;
constexpr std::size_t kMaxPsEncodedTypeDescriptorBytes = 65534;

std::optional<PsNameResolveRequest> DecodePsNameResolveRequest(
    const std::vector<std::uint8_t>& payload,
    std::uint32_t schema) {
  if (schema != sbps::kSchemaResolveNameRequestV1 &&
      schema != sbps::kSchemaResolveNameRequestV2 &&
      schema != sbps::kSchemaResolveNameRequestV3) {
    return std::nullopt;
  }
  PsNameResolveRequest request;
  std::size_t offset = 0;
  const bool v3 = schema == sbps::kSchemaResolveNameRequestV3;
  auto read_request_string = [&](std::string* value,
                                 std::size_t max_bytes) {
    return v3 ? PsNameReadBoundedString(payload,
                                        &offset,
                                        value,
                                        max_bytes)
              : PsNameReadString(payload, &offset, value);
  };
  if (!read_request_string(&request.presented_name,
                           kMaxPsRelationMetadataTextBytes)) {
    return std::nullopt;
  }
  if (offset >= payload.size()) return std::nullopt;
  const std::uint8_t quoted = payload[offset++];
  if (v3 && quoted > 1) return std::nullopt;
  request.quoted = quoted != 0;
  if (!read_request_string(&request.dialect_profile,
                           kMaxPsRelationMetadataTextBytes) ||
      !read_request_string(&request.language,
                           kMaxPsRelationMetadataTextBytes) ||
      !read_request_string(&request.search_path,
                           kMaxPsRelationMetadataTextBytes) ||
      !read_request_string(&request.object_class, 128)) {
    return std::nullopt;
  }
  if (schema == sbps::kSchemaResolveNameRequestV1) {
    if (offset < payload.size()) {
      request.bypass_cache = payload[offset++] != 0;
    }
    // Preserve the V1 decoder's historical tolerance of trailing extension
    // bytes.  V2 is exact and fail-closed below.
    return request;
  }
  if (offset >= payload.size()) return std::nullopt;
  const std::uint8_t bypass_cache = payload[offset++];
  if (v3 && bypass_cache > 1) return std::nullopt;
  request.bypass_cache = bypass_cache != 0;
  if (offset + 16 > payload.size()) return std::nullopt;
  request.session_uuid = PsNameGetUuid(payload, offset);
  offset += 16;
  if (!PsNameReadU64(payload, &offset, &request.local_transaction_id)) {
    return std::nullopt;
  }
  if (!read_request_string(&request.transaction_uuid, 64)) {
    return std::nullopt;
  }
  if (schema == sbps::kSchemaResolveNameRequestV2) {
    if (offset != payload.size()) return std::nullopt;
  } else {
    if (offset >= payload.size()) return std::nullopt;
    request.projection_flags = payload[offset++];
    if ((request.projection_flags &
         ~kPsNameProjectionRelationDescriptorV1) != 0 ||
        offset != payload.size()) {
      return std::nullopt;
    }
    request.include_persisted_relation_descriptor =
        (request.projection_flags &
         kPsNameProjectionRelationDescriptorV1) != 0;
    const std::string object_class = PsNameLower(request.object_class);
    if (request.include_persisted_relation_descriptor &&
        object_class != "relation" && object_class != "table") {
      return std::nullopt;
    }
  }
  request.transaction_routed = true;
  // Transaction-routed resolution must never consult or populate a
  // session/global cache because visibility belongs to this exact snapshot.
  request.bypass_cache = true;
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
                                                     std::string_view detail,
                                                     const engine_api::EngineResolvedResourceDescriptor*
                                                         resource_descriptor = nullptr) {
  std::vector<std::uint8_t> payload;
  PsNamePutString(&payload, outcome);
  PsNamePutUuid(&payload, object_uuid);
  PsNamePutString(&payload, canonical_name);
  PsNamePutString(&payload, object_class);
  PsNamePutU64(&payload, catalog_epoch);
  PsNamePutU64(&payload, security_epoch);
  PsNamePutString(&payload, detail);
  if (resource_descriptor != nullptr && resource_descriptor->present) {
    constexpr std::uint8_t kResourceDescriptorExtensionV1 = 1;
    PsNamePutU8(&payload, kResourceDescriptorExtensionV1);
    PsNamePutString(&payload, resource_descriptor->resource_family);
    PsNamePutString(&payload, resource_descriptor->canonical_name);
    PsNamePutString(&payload,
                    resource_descriptor->parent_resource_uuid.canonical);
    PsNamePutString(&payload, resource_descriptor->parent_canonical_name);
    PsNamePutString(&payload,
                    resource_descriptor->default_collation_uuid.canonical);
    PsNamePutString(&payload, resource_descriptor->default_collation_name);
    PsNamePutU64(&payload, resource_descriptor->resource_epoch);
    PsNamePutU64(&payload, resource_descriptor->family_epoch);
    PsNamePutString(&payload, resource_descriptor->family_version);
    PsNamePutU32(&payload, resource_descriptor->min_bytes);
    PsNamePutU32(&payload, resource_descriptor->max_bytes);
    std::uint8_t attributes = 0;
    if (resource_descriptor->variable_width) attributes |= 0x01u;
    if (resource_descriptor->default_for_parent) attributes |= 0x02u;
    if (resource_descriptor->case_insensitive) attributes |= 0x04u;
    if (resource_descriptor->accent_insensitive) attributes |= 0x08u;
    PsNamePutU8(&payload, attributes);
  }
  return payload;
}

struct PsPublicRelationColumnProjection {
  std::array<std::uint8_t, 16> column_uuid{};
  std::uint32_t ordinal = 0;
  std::string canonical_name_key;
  std::array<std::uint8_t, 16> type_descriptor_uuid{};
  std::string type_descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_type_descriptor;
  bool nullable = true;
  bool generated = false;
  bool identity_column = false;
  std::array<std::uint8_t, 16> charset_uuid{};
  std::string charset_canonical_name;
  std::array<std::uint8_t, 16> collation_uuid{};
  std::string collation_canonical_name;
  std::uint32_t character_length = 0;
  std::uint32_t charset_min_bytes = 0;
  std::uint32_t charset_max_bytes = 0;
  bool charset_variable_width = false;
};

struct PsPublicRelationProjection {
  std::array<std::uint8_t, 16> descriptor_uuid{};
  std::array<std::uint8_t, 16> relation_uuid{};
  std::array<std::uint8_t, 16> schema_uuid{};
  std::uint64_t descriptor_generation = 0;
  std::uint64_t validated_resource_epoch = 0;
  std::vector<PsPublicRelationColumnProjection> columns;
};

struct PsPublicRelationProjectionResult {
  bool ok = false;
  PsPublicRelationProjection projection;
  ServerDiagnostic diagnostic;
};

ServerDiagnostic PsRelationProjectionDiagnostic(
    std::string code,
    std::string key,
    std::string safe_message,
    std::string reason = {}) {
  std::vector<ServerDiagnosticField> fields;
  if (!reason.empty()) fields.push_back({"reason", std::move(reason)});
  return sbps::IpcDiagnostic(std::move(code),
                             std::move(key),
                             std::move(safe_message),
                             std::move(fields));
}

ServerDiagnostic PsRelationProjectionEngineDiagnostic(
    const engine_api::EngineApiDiagnostic& diagnostic,
    std::string_view fallback_code,
    std::string_view fallback_key,
    std::string_view safe_message) {
  return PsRelationProjectionDiagnostic(
      diagnostic.code.empty() ? std::string(fallback_code) : diagnostic.code,
      diagnostic.message_key.empty() ? std::string(fallback_key)
                                     : diagnostic.message_key,
      std::string(safe_message),
      diagnostic.detail);
}

bool PsEncodedDescriptorHasExactField(std::string_view descriptor,
                                      std::string_view key,
                                      std::string_view expected_value) {
  const std::string expected =
      std::string(key) + "=" + std::string(expected_value);
  const std::string prefix = std::string(key) + "=";
  bool matched = false;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto delimiter = descriptor.find(';', offset);
    const auto field = descriptor.substr(
        offset,
        delimiter == std::string_view::npos
            ? descriptor.size() - offset
            : delimiter - offset);
    if (field == expected) {
      if (matched) return false;
      matched = true;
    } else if (field.starts_with(prefix)) {
      return false;
    }
    if (delimiter == std::string_view::npos) break;
    offset = delimiter + 1;
  }
  return matched;
}

PsPublicRelationProjectionResult BuildPsPublicRelationProjection(
    const engine_api::EngineRequestContext& context,
    std::string_view resolved_relation_uuid) {
  PsPublicRelationProjectionResult result;
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      context.resource_epoch == 0 || resolved_relation_uuid.empty()) {
    result.diagnostic = PsRelationProjectionDiagnostic(
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID",
        "parser_server_ipc.relation_descriptor_request_invalid",
        "Persisted relation projection requires an exact active transaction and current resource epoch.",
        "exact_transaction_and_resource_epoch_required");
    return result;
  }
  const auto loaded = engine_api::LoadMgaRelationStorageDescriptor(
      context, std::string(resolved_relation_uuid));
  if (!loaded.ok) {
    result.diagnostic = PsRelationProjectionEngineDiagnostic(
        loaded.diagnostic,
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUIRED",
        "parser_server_ipc.relation_descriptor_required",
        "The persisted MGA relation descriptor is unavailable at the selected transaction snapshot.");
    return result;
  }
  const auto descriptor_uuid = PsNameUuidFromText(
      loaded.descriptor.descriptor_uuid.canonical);
  const auto relation_uuid = PsNameUuidFromText(
      loaded.descriptor.relation_uuid.canonical);
  const auto schema_uuid = PsNameUuidFromText(
      loaded.descriptor.schema_uuid.canonical);
  if (!descriptor_uuid || !relation_uuid || !schema_uuid ||
      loaded.descriptor.relation_uuid.canonical != resolved_relation_uuid ||
      loaded.descriptor.descriptor_generation == 0 ||
      loaded.descriptor.columns.empty()) {
    result.diagnostic = PsRelationProjectionDiagnostic(
        loaded.descriptor.relation_uuid.canonical != resolved_relation_uuid
            ? "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RELATION_MISMATCH"
            : "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
        loaded.descriptor.relation_uuid.canonical != resolved_relation_uuid
            ? "parser_server_ipc.relation_descriptor_relation_mismatch"
            : "parser_server_ipc.relation_descriptor_invalid",
        "The persisted MGA relation descriptor failed neutral projection validation.",
        "descriptor_identity_invalid");
    return result;
  }
  if (loaded.descriptor.columns.size() >
      kMaxPsRelationProjectionColumns) {
    result.diagnostic = PsRelationProjectionDiagnostic(
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE",
        "parser_server_ipc.relation_descriptor_too_large",
        "The persisted MGA relation descriptor exceeds the public projection limit.",
        "column_count_limit_exceeded");
    return result;
  }
  result.projection.descriptor_uuid = *descriptor_uuid;
  result.projection.relation_uuid = *relation_uuid;
  result.projection.schema_uuid = *schema_uuid;
  result.projection.descriptor_generation =
      loaded.descriptor.descriptor_generation;
  result.projection.validated_resource_epoch = context.resource_epoch;
  result.projection.columns.reserve(loaded.descriptor.columns.size());
  std::set<std::string> column_uuids;
  std::set<std::uint32_t> ordinals;
  for (const auto& source : loaded.descriptor.columns) {
    const auto column_uuid =
        PsNameUuidFromText(source.column_uuid.canonical);
    const auto type_descriptor_uuid = PsNameUuidFromText(
        source.value_descriptor.descriptor_uuid.canonical);
    if (!column_uuid || !type_descriptor_uuid ||
        source.canonical_name_key.empty() ||
        source.canonical_name_key.size() >
            kMaxPsRelationMetadataTextBytes ||
        source.value_descriptor.descriptor_kind.empty() ||
        source.value_descriptor.descriptor_kind.size() >
            kMaxPsRelationMetadataTextBytes ||
        source.value_descriptor.canonical_type_name.empty() ||
        source.value_descriptor.canonical_type_name.size() >
            kMaxPsRelationMetadataTextBytes ||
        source.value_descriptor.encoded_descriptor.empty() ||
        source.value_descriptor.encoded_descriptor.size() >
            kMaxPsEncodedTypeDescriptorBytes ||
        !column_uuids.insert(source.column_uuid.canonical).second ||
        !ordinals.insert(source.ordinal).second) {
      result.diagnostic = PsRelationProjectionDiagnostic(
          "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
          "parser_server_ipc.relation_descriptor_invalid",
          "A persisted MGA relation column failed neutral projection validation.",
          "column_descriptor_invalid");
      return result;
    }
    PsPublicRelationColumnProjection column;
    column.column_uuid = *column_uuid;
    column.ordinal = source.ordinal;
    column.canonical_name_key = source.canonical_name_key;
    column.type_descriptor_uuid = *type_descriptor_uuid;
    column.type_descriptor_kind =
        source.value_descriptor.descriptor_kind;
    column.canonical_type_name =
        source.value_descriptor.canonical_type_name;
    column.encoded_type_descriptor =
        source.value_descriptor.encoded_descriptor;
    column.nullable = source.nullable;
    column.generated = source.generated;
    column.identity_column = source.identity_column;
    column.character_length = source.character_length;

    if (!source.charset_uuid.empty()) {
      const auto charset_uuid = PsNameUuidFromText(source.charset_uuid);
      if (!charset_uuid) {
        result.diagnostic = PsRelationProjectionDiagnostic(
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
            "parser_server_ipc.relation_descriptor_resource_mismatch",
            "A persisted relation column has an invalid charset identity.",
            "charset_uuid_invalid");
        return result;
      }
      engine_api::EngineUuid engine_charset_uuid;
      engine_charset_uuid.canonical = source.charset_uuid;
      const auto charset = engine_api::LookupEngineResourceDescriptorByUuid(
          context, engine_charset_uuid, "charset");
      if (!charset.ok) {
        result.diagnostic = PsRelationProjectionEngineDiagnostic(
            charset.diagnostic,
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
            "parser_server_ipc.relation_descriptor_resource_mismatch",
            "The persisted relation charset identity is not valid at the selected transaction snapshot.");
        return result;
      }
      const auto& resource = charset.resource_descriptor;
      const bool text_large_object = PsEncodedDescriptorHasExactField(
          source.value_descriptor.encoded_descriptor,
          "text_resource_storage",
          "large_object");
      if (!resource.present || resource.resource_family != "charset" ||
          resource.resource_uuid.canonical != source.charset_uuid ||
          resource.canonical_name.empty() ||
          resource.canonical_name.size() >
              kMaxPsRelationMetadataTextBytes ||
          resource.resource_epoch != context.resource_epoch ||
          resource.min_bytes == 0 ||
          resource.max_bytes < resource.min_bytes ||
          (text_large_object ? source.character_length != 0
                             : source.character_length == 0)) {
        result.diagnostic = PsRelationProjectionDiagnostic(
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
            "parser_server_ipc.relation_descriptor_resource_mismatch",
            "The persisted relation charset descriptor is inconsistent with resource authority.",
            "charset_descriptor_mismatch");
        return result;
      }
      column.charset_uuid = *charset_uuid;
      column.charset_canonical_name = resource.canonical_name;
      column.charset_min_bytes = resource.min_bytes;
      column.charset_max_bytes = resource.max_bytes;
      column.charset_variable_width = resource.variable_width;
    } else if (!source.collation_uuid.empty() ||
               source.character_length != 0) {
      result.diagnostic = PsRelationProjectionDiagnostic(
          "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
          "parser_server_ipc.relation_descriptor_resource_mismatch",
          "A persisted relation column has incomplete text resource identity.",
          "charset_identity_required");
      return result;
    }

    if (!source.collation_uuid.empty()) {
      const auto collation_uuid =
          PsNameUuidFromText(source.collation_uuid);
      if (!collation_uuid || source.charset_uuid.empty()) {
        result.diagnostic = PsRelationProjectionDiagnostic(
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
            "parser_server_ipc.relation_descriptor_resource_mismatch",
            "A persisted relation column has an invalid collation identity.",
            "collation_uuid_invalid");
        return result;
      }
      engine_api::EngineUuid engine_collation_uuid;
      engine_collation_uuid.canonical = source.collation_uuid;
      const auto collation = engine_api::LookupEngineResourceDescriptorByUuid(
          context, engine_collation_uuid, "collation");
      if (!collation.ok) {
        result.diagnostic = PsRelationProjectionEngineDiagnostic(
            collation.diagnostic,
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
            "parser_server_ipc.relation_descriptor_resource_mismatch",
            "The persisted relation collation identity is not valid at the selected transaction snapshot.");
        return result;
      }
      const auto& resource = collation.resource_descriptor;
      if (!resource.present || resource.resource_family != "collation" ||
          resource.resource_uuid.canonical != source.collation_uuid ||
          resource.parent_resource_uuid.canonical != source.charset_uuid ||
          resource.canonical_name.empty() ||
          resource.canonical_name.size() >
              kMaxPsRelationMetadataTextBytes ||
          resource.resource_epoch != context.resource_epoch) {
        result.diagnostic = PsRelationProjectionDiagnostic(
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
            "parser_server_ipc.relation_descriptor_resource_mismatch",
            "The persisted relation collation descriptor is inconsistent with resource authority.",
            "collation_charset_relationship_mismatch");
        return result;
      }
      column.collation_uuid = *collation_uuid;
      column.collation_canonical_name = resource.canonical_name;
    }
    result.projection.columns.push_back(std::move(column));
  }
  result.ok = true;
  return result;
}

std::optional<std::vector<std::uint8_t>>
EncodePsNameResolvePayloadV3(
    std::string_view outcome,
    const std::array<std::uint8_t, 16>& object_uuid,
    std::string_view canonical_name,
    std::string_view object_class,
    std::uint64_t catalog_epoch,
    std::uint64_t security_epoch,
    std::string_view detail,
    const PsPublicRelationProjection* relation_projection) {
  if (canonical_name.size() > kMaxPsRelationMetadataTextBytes ||
      object_class.size() > kMaxPsRelationMetadataTextBytes ||
      detail.size() > kMaxPsRelationMetadataTextBytes) {
    return std::nullopt;
  }
  auto payload = EncodePsNameResolvePayload(outcome,
                                            object_uuid,
                                            canonical_name,
                                            object_class,
                                            catalog_epoch,
                                            security_epoch,
                                            detail);
  PsNamePutU8(&payload, relation_projection == nullptr ? 0 : 1);
  if (relation_projection == nullptr) return payload;

  std::vector<std::uint8_t> extension;
  PsNamePutUuid(&extension, relation_projection->descriptor_uuid);
  PsNamePutUuid(&extension, relation_projection->relation_uuid);
  PsNamePutUuid(&extension, relation_projection->schema_uuid);
  PsNamePutU64(&extension, relation_projection->descriptor_generation);
  PsNamePutU64(&extension, relation_projection->validated_resource_epoch);
  PsNamePutU32(
      &extension,
      static_cast<std::uint32_t>(relation_projection->columns.size()));
  for (const auto& column : relation_projection->columns) {
    PsNamePutUuid(&extension, column.column_uuid);
    PsNamePutU32(&extension, column.ordinal);
    PsNamePutString(&extension, column.canonical_name_key);
    PsNamePutUuid(&extension, column.type_descriptor_uuid);
    PsNamePutString(&extension, column.type_descriptor_kind);
    PsNamePutString(&extension, column.canonical_type_name);
    PsNamePutString(&extension, column.encoded_type_descriptor);
    std::uint8_t attributes = 0;
    if (column.nullable) attributes |= 0x01u;
    if (column.generated) attributes |= 0x02u;
    if (column.identity_column) attributes |= 0x04u;
    if (column.charset_variable_width) attributes |= 0x08u;
    PsNamePutU8(&extension, attributes);
    PsNamePutUuid(&extension, column.charset_uuid);
    PsNamePutString(&extension, column.charset_canonical_name);
    PsNamePutUuid(&extension, column.collation_uuid);
    PsNamePutString(&extension, column.collation_canonical_name);
    PsNamePutU32(&extension, column.character_length);
    PsNamePutU32(&extension, column.charset_min_bytes);
    PsNamePutU32(&extension, column.charset_max_bytes);
    if (extension.size() > kMaxPsRelationProjectionBytes) {
      return std::nullopt;
    }
  }
  PsNamePutU8(&payload, kPsRelationDescriptorExtensionKind);
  PsNamePutU8(&payload, kPsRelationDescriptorExtensionVersion);
  PsNamePutU32(&payload, static_cast<std::uint32_t>(extension.size()));
  payload.insert(payload.end(), extension.begin(), extension.end());
  return payload;
}

struct PsNameV3PayloadResult {
  bool ok = false;
  std::vector<std::uint8_t> payload;
  ServerDiagnostic diagnostic;
};

struct PsNameSemanticDetailResult {
  bool ok = true;
  std::string detail;
  ServerDiagnostic diagnostic;
};

// SB_SERVER_GLOBAL_AGGREGATE_VIEW_SEMANTIC_DETAIL_V1_BEGIN
std::string PsNameHexEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2u);
  for (const unsigned char byte : value) {
    encoded.push_back(kHex[(byte >> 4u) & 0x0fu]);
    encoded.push_back(kHex[byte & 0x0fu]);
  }
  return encoded;
}

std::optional<std::string> EncodeGlobalAggregateViewSemanticDetail(
    const engine_api::EngineResolvedSemanticProjection& projection) {
  const auto expected_result =
      engine_api::EngineGlobalAggregateAvgIntegerResultDescriptor();
  if (!projection.present ||
      projection.marker != engine_api::kEngineGlobalAggregateViewMarkerV1 ||
      projection.projection_descriptor.descriptor_uuid.canonical.empty() ||
      projection.projection_descriptor.descriptor_kind !=
          "global_aggregate_view" ||
      projection.projection_descriptor.canonical_type_name !=
          engine_api::kEngineGlobalAggregateViewMarkerV1 ||
      projection.projection_descriptor.encoded_descriptor.empty() ||
      projection.descriptor_generation == 0 ||
      projection.result_alias.empty() ||
      projection.result_descriptor.descriptor_kind !=
          expected_result.descriptor_kind ||
      projection.result_descriptor.canonical_type_name !=
          expected_result.canonical_type_name ||
      projection.result_descriptor.encoded_descriptor !=
          expected_result.encoded_descriptor) {
    return std::nullopt;
  }

  std::ostringstream packed;
  packed << "gavs1|" << PsNameHexEncode(projection.marker) << '|'
         << PsNameHexEncode(
                projection.projection_descriptor.descriptor_uuid.canonical)
         << '|' << projection.descriptor_generation << '|'
         << PsNameHexEncode(
                projection.projection_descriptor.descriptor_kind)
         << '|'
         << PsNameHexEncode(
                projection.projection_descriptor.canonical_type_name)
         << '|'
         << PsNameHexEncode(
                projection.projection_descriptor.encoded_descriptor)
         << '|' << PsNameHexEncode(projection.result_alias) << '|'
         << PsNameHexEncode(projection.result_descriptor.descriptor_kind)
         << '|'
         << PsNameHexEncode(
                projection.result_descriptor.canonical_type_name)
         << '|'
         << PsNameHexEncode(
                projection.result_descriptor.encoded_descriptor);
  return packed.str();
}

std::optional<std::string> EncodeRelationProjectionViewSemanticDetail(
    const engine_api::EngineResolvedSemanticProjection& projection) {
  const bool v1 = projection.marker ==
                  engine_api::kEngineRelationProjectionViewMarkerV1;
  const bool v2 = projection.marker ==
                  engine_api::kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_output_count = v1 ? 2u : 1u;
  if (!projection.present ||
      (!v1 && !v2) ||
      projection.projection_descriptor.descriptor_uuid.canonical.empty() ||
      projection.projection_descriptor.descriptor_kind !=
          "relation_projection_view" ||
      projection.projection_descriptor.canonical_type_name !=
          projection.marker ||
      projection.projection_descriptor.encoded_descriptor.empty() ||
      projection.descriptor_generation == 0 ||
      projection.ordered_outputs.size() != expected_output_count) {
    return std::nullopt;
  }

  std::set<std::string> identities = {
      projection.projection_descriptor.descriptor_uuid.canonical};
  std::set<std::string> names;
  for (std::size_t index = 0; index < projection.ordered_outputs.size();
       ++index) {
    const auto& output = projection.ordered_outputs[index];
    if (output.ordinal != index || output.output_name.empty() ||
        output.output_column_uuid.canonical.empty() ||
        output.output_type.type_descriptor_uuid.canonical.empty() ||
        output.output_type.descriptor_kind.empty() ||
        output.output_type.canonical_type_name.empty() ||
        output.output_type.encoded_descriptor.empty() ||
        !identities.insert(output.output_column_uuid.canonical).second ||
        !identities.insert(
             output.output_type.type_descriptor_uuid.canonical).second ||
        !names.insert(output.output_name).second) {
      return std::nullopt;
    }
  }

  std::ostringstream packed;
  packed << (v1 ? "rpvs1|" : "rpvd2|")
         << PsNameHexEncode(projection.marker) << '|'
         << PsNameHexEncode(
                projection.projection_descriptor.descriptor_uuid.canonical)
         << '|' << projection.descriptor_generation << '|'
         << expected_output_count;
  for (const auto& output : projection.ordered_outputs) {
    packed << '|' << output.ordinal << '|'
           << PsNameHexEncode(output.output_name) << '|'
           << PsNameHexEncode(output.output_column_uuid.canonical) << '|'
           << PsNameHexEncode(
                  output.output_type.type_descriptor_uuid.canonical)
           << '|' << PsNameHexEncode(output.output_type.descriptor_kind)
           << '|' << PsNameHexEncode(
                  output.output_type.canonical_type_name)
           << '|' << PsNameHexEncode(
                  output.output_type.encoded_descriptor)
           << '|' << PsNameHexEncode(output.nullable ? "1" : "0");
  }
  return packed.str();
}
// SB_SERVER_GLOBAL_AGGREGATE_VIEW_SEMANTIC_DETAIL_V1_END

PsNameSemanticDetailResult BuildPsNameSemanticDetail(
    const engine_api::EngineRequestContext& context,
    std::string_view object_uuid,
    std::string_view object_class,
    std::string_view ordinary_detail,
    const engine_api::EngineResolvedSemanticProjection*
        resolved_semantic_projection) {
  PsNameSemanticDetailResult result;
  result.detail = std::string(ordinary_detail);
  if (PsNameLower(std::string(object_class)) != "view") return result;

  if (resolved_semantic_projection != nullptr &&
      resolved_semantic_projection->present) {
    const auto encoded =
        (resolved_semantic_projection->marker ==
                 engine_api::kEngineRelationProjectionViewMarkerV1 ||
         resolved_semantic_projection->marker ==
                 engine_api::kEngineRelationProjectionViewMarkerV2)
            ? EncodeRelationProjectionViewSemanticDetail(
                  *resolved_semantic_projection)
            : resolved_semantic_projection->marker ==
                      engine_api::kEngineGlobalAggregateViewMarkerV1
                  ? EncodeGlobalAggregateViewSemanticDetail(
                        *resolved_semantic_projection)
                  : std::optional<std::string>{};
    if (!encoded) {
      result.ok = false;
      result.detail.clear();
      result.diagnostic = PsRelationProjectionDiagnostic(
          "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
          "parser_server_ipc.view_descriptor_invalid",
          "The exact engine-owned persisted view semantic descriptor is invalid.",
          "engine_view_semantic_descriptor_invalid");
      return result;
    }
    result.detail = *encoded;
    return result;
  }

  const auto aggregate_view =
      engine_api::DescribeEngineGlobalAggregateView(
          context, std::string(object_uuid));
  if (aggregate_view.diagnostic.error) {
    result.ok = false;
    result.detail.clear();
    result.diagnostic = PsRelationProjectionEngineDiagnostic(
        aggregate_view.diagnostic,
        "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
        "parser_server_ipc.view_descriptor_invalid",
        "The exact engine-owned persisted view descriptor is invalid or not visible.");
    return result;
  }
  if (aggregate_view.present) {
    engine_api::EngineResolvedSemanticProjection projection;
    projection.present = true;
    projection.marker = engine_api::kEngineGlobalAggregateViewMarkerV1;
    projection.projection_descriptor =
        engine_api::EngineGlobalAggregateViewSemanticDescriptor(
            aggregate_view);
    projection.descriptor_generation =
        aggregate_view.view_descriptor_generation;
    projection.result_alias = aggregate_view.result_alias;
    projection.result_descriptor = aggregate_view.result_descriptor;
    const auto encoded =
        EncodeGlobalAggregateViewSemanticDetail(projection);
    if (!encoded) {
      result.ok = false;
      result.detail.clear();
      result.diagnostic = PsRelationProjectionDiagnostic(
          "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
          "parser_server_ipc.view_descriptor_invalid",
          "The exact engine-owned persisted view semantic descriptor is invalid.",
          "global_aggregate_view_semantic_descriptor_invalid");
      return result;
    }
    result.detail = *encoded;
    return result;
  }

  const auto relation_view =
      engine_api::DescribeEngineRelationProjectionView(
          context, std::string(object_uuid));
  if (relation_view.diagnostic.error) {
    result.ok = false;
    result.detail.clear();
    result.diagnostic = PsRelationProjectionEngineDiagnostic(
        relation_view.diagnostic,
        "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
        "parser_server_ipc.view_descriptor_invalid",
        "The exact engine-owned persisted relation-view descriptor is invalid or not visible.");
    return result;
  }
  if (relation_view.present) {
    engine_api::EngineResolvedSemanticProjection projection;
    projection.present = true;
    projection.marker = relation_view.marker;
    projection.projection_descriptor =
        engine_api::EngineRelationProjectionViewSemanticDescriptor(
            relation_view);
    projection.descriptor_generation =
        relation_view.view_descriptor_generation;
    projection.ordered_outputs =
        engine_api::EngineRelationProjectionViewSemanticOutputs(
            relation_view);
    const auto encoded =
        EncodeRelationProjectionViewSemanticDetail(projection);
    if (!encoded) {
      result.ok = false;
      result.detail.clear();
      result.diagnostic = PsRelationProjectionDiagnostic(
          "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
          "parser_server_ipc.view_descriptor_invalid",
          "The exact engine-owned persisted relation-view semantic descriptor is invalid.",
          "relation_projection_view_semantic_descriptor_invalid");
      return result;
    }
    result.detail = *encoded;
    return result;
  }

  const auto descriptor =
      engine_api::DescribeEngineCatalogRelationProjectionView(
          context, std::string(object_uuid));
  if (descriptor.diagnostic.error) {
    result.ok = false;
    result.detail.clear();
    result.diagnostic = PsRelationProjectionEngineDiagnostic(
        descriptor.diagnostic,
        "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
        "parser_server_ipc.view_descriptor_invalid",
        "The exact engine-owned persisted view descriptor is invalid or not visible.");
    return result;
  }
  if (!descriptor.present) return result;

  const bool supported_variant =
      descriptor.semantic_variant ==
          engine_api::kRelationDescriptorProjectionTypeInventoryVariantV1 ||
      descriptor.semantic_variant ==
          engine_api::kRelationDescriptorProjectionCharsetInventoryVariantV1;
  if (!supported_variant) {
    result.ok = false;
    result.detail.clear();
    result.diagnostic = PsRelationProjectionDiagnostic(
        "PARSER_SERVER_IPC.VIEW_DESCRIPTOR_INVALID",
        "parser_server_ipc.view_descriptor_invalid",
        "The exact engine-owned persisted view descriptor has an unsupported semantic variant.",
        "semantic_variant_unsupported");
    return result;
  }
  result.detail =
      std::string(engine_api::kRelationDescriptorProjectionMarkerV1) + ":" +
      descriptor.semantic_variant;
  return result;
}

PsNameV3PayloadResult BuildPsNameResolvedPayloadV3(
    const PsNameResolveRequest& decoded,
    const engine_api::EngineRequestContext& context,
    const std::array<std::uint8_t, 16>& object_uuid,
    std::string_view canonical_name,
    std::string_view object_class,
    std::uint64_t catalog_epoch,
    std::uint64_t security_epoch,
    std::string_view detail) {
  PsNameV3PayloadResult result;
  std::optional<PsPublicRelationProjection> projection;
  if (decoded.include_persisted_relation_descriptor) {
    const auto built = BuildPsPublicRelationProjection(
        context, UuidBytesToText(object_uuid));
    if (!built.ok) {
      result.diagnostic = built.diagnostic;
      return result;
    }
    projection = built.projection;
  }
  auto encoded = EncodePsNameResolvePayloadV3(
      "resolved",
      object_uuid,
      canonical_name,
      object_class,
      catalog_epoch,
      security_epoch,
      detail,
      projection ? &*projection : nullptr);
  if (!encoded) {
    result.diagnostic = PsRelationProjectionDiagnostic(
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE",
        "parser_server_ipc.relation_descriptor_too_large",
        "The persisted MGA relation descriptor exceeds the public projection limit.",
        "encoded_projection_limit_exceeded");
    return result;
  }
  result.ok = true;
  result.payload = std::move(*encoded);
  return result;
}

std::vector<std::uint8_t> ResolveNamePublicFrame(const sbps::Frame& frame,
                                                 const HostedEngineState& engine_state,
                                                 ServerSessionRegistry* session_registry) {
  const auto trace_begin = std::chrono::steady_clock::now();
  const bool transaction_routed =
      frame.header.payload_schema_id == sbps::kSchemaResolveNameRequestV2 ||
      frame.header.payload_schema_id == sbps::kSchemaResolveNameRequestV3;
  const bool relation_projection_schema =
      frame.header.payload_schema_id == sbps::kSchemaResolveNameRequestV3;
  const std::uint32_t response_schema =
      relation_projection_schema
          ? sbps::kSchemaResolveNameResultV3
          : transaction_routed ? sbps::kSchemaResolveNameResultV2
                               : sbps::kSchemaResolveNameResultV1;
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
  const auto decoded = DecodePsNameResolveRequest(frame.payload,
                                                  frame.header.payload_schema_id);
  if (!decoded || decoded->presented_name.empty()) {
    return ErrorFrame({sbps::IpcDiagnostic(
                          relation_projection_schema
                              ? "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID"
                              : "PARSER_SERVER_IPC.RESOLVE_NAME_INVALID",
                          relation_projection_schema
                              ? "parser_server_ipc.relation_descriptor_request_invalid"
                              : "parser_server_ipc.resolve_name_invalid",
                          "The public name-resolution request is malformed.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
  }
  std::optional<ServerSessionRecord> session;
  std::unique_lock<std::mutex> transaction_lock;
  if (session_registry != nullptr) {
    const auto found = session_registry->sessions_by_uuid.find(
        UuidBytesToText(frame.header.session_uuid));
    if (found != session_registry->sessions_by_uuid.end()) {
      if (found->second.detached_recovery_quarantined) {
        return ErrorFrame(
            {sbps::IpcDiagnostic(
                "PARSER_SERVER_IPC.SESSION_RECOVERY_QUARANTINED",
                "parser_server_ipc.session_recovery_quarantined",
                "Name resolution is blocked while detached transaction finality awaits engine recovery.")},
            frame.header.request_uuid,
            frame.header.sequence_number,
            static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
      }
      if (found->second.transaction_mutex != nullptr) {
        transaction_lock =
            std::unique_lock<std::mutex>(*found->second.transaction_mutex);
      }
      const bool exact_frame_binding =
          !sbps::IsZeroUuid(frame.header.connection_uuid) &&
          !sbps::IsZeroUuid(frame.header.session_uuid) &&
          !sbps::IsZeroUuid(found->second.connection_uuid) &&
          frame.header.session_uuid == found->second.session_uuid &&
          frame.header.connection_uuid == found->second.connection_uuid;
      if (!exact_frame_binding) {
        return ErrorFrame(
            {sbps::IpcDiagnostic(
                "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                "parser_server_ipc.route_association_mismatch",
                "Name resolution requires exact connection, header, and session binding.")},
            frame.header.request_uuid,
            frame.header.sequence_number,
            static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
      }
      if (transaction_routed) {
        const bool exact_binding =
            !sbps::IsZeroUuid(decoded->session_uuid) &&
            frame.header.session_uuid == decoded->session_uuid &&
            found->second.session_uuid == decoded->session_uuid;
        if (!exact_binding) {
          return ErrorFrame(
              {sbps::IpcDiagnostic(
                  "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                  "parser_server_ipc.route_association_mismatch",
                  "Transaction-routed name resolution requires exact connection, header, payload, and session binding.")},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
        }
        if (!found->second.transaction_routing_v2_negotiated) {
          return ErrorFrame(
              {sbps::IpcDiagnostic(
                  "PARSER_SERVER_IPC.TRANSACTION_ROUTING_V2_NOT_NEGOTIATED",
                  "parser_server_ipc.transaction_routing_v2_not_negotiated",
                  "Transaction-routed name resolution was not negotiated for this server session.")},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
        }
        if (relation_projection_schema &&
            !found->second.relation_descriptor_projection_v3_negotiated) {
          return ErrorFrame(
              {sbps::IpcDiagnostic(
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_NOT_NEGOTIATED",
                  "parser_server_ipc.relation_descriptor_v3_not_negotiated",
                  "Persisted relation projection was not negotiated for this server session.")},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
        }
        const auto selected = found->second.transactions_by_local_id.find(
            decoded->local_transaction_id);
        if (decoded->local_transaction_id == 0 ||
            decoded->transaction_uuid.empty() ||
            selected == found->second.transactions_by_local_id.end() ||
            selected->second.transaction_uuid != decoded->transaction_uuid ||
            selected->second.lifecycle_state !=
                ServerTransactionLifecycleState::kActive) {
          return ErrorFrame(
              {sbps::IpcDiagnostic(
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_INVALID",
                  "parser_server_ipc.transaction_selector_invalid",
                  "The transaction selector is not active and owned by this session.")},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
        }
        session = found->second;
        session->local_transaction_id =
            selected->second.local_transaction_id;
        session->snapshot_visible_through_local_transaction_id =
            selected->second.snapshot_visible_through_local_transaction_id;
        session->transaction_uuid = selected->second.transaction_uuid;
        session->transaction_timestamp = selected->second.transaction_timestamp;
      } else {
        const std::uint64_t default_id =
            found->second.default_local_transaction_id != 0
                ? found->second.default_local_transaction_id
                : found->second.local_transaction_id;
        const auto default_transaction =
            found->second.transactions_by_local_id.find(default_id);
        if (default_transaction !=
                found->second.transactions_by_local_id.end() &&
            default_transaction->second.lifecycle_state ==
                ServerTransactionLifecycleState::kFinalityUnknown) {
          return ErrorFrame(
              {sbps::IpcDiagnostic(
                  "PARSER_SERVER_IPC.DEFAULT_TRANSACTION_FINALITY_UNKNOWN",
                  "parser_server_ipc.default_transaction_finality_unknown",
                  "Legacy name resolution is blocked while default transaction finality is unknown.")},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
        }
        const auto* active_default =
            AdoptAndFindExactActiveDefaultTransaction(&found->second);
        if (active_default == nullptr) {
          return ErrorFrame(
              {sbps::IpcDiagnostic(
                  "PARSER_SERVER_IPC.DEFAULT_TRANSACTION_NOT_ACTIVE",
                  "parser_server_ipc.default_transaction_not_active",
                  "Legacy name resolution requires one exact active default transaction.")},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
        }
        session = found->second;
      }
    }
  }
  if (!session) {
    return ErrorFrame({sbps::IpcDiagnostic("PARSER_SERVER_IPC.SESSION_REQUIRED",
                                           "parser_server_ipc.session_required",
                                           "Public name resolution requires a bound server session.")},
                      frame.header.request_uuid,
                      frame.header.sequence_number,
                      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult));
  }
  const auto normalized = PsNameLower(decoded->presented_name);
  const std::string virtual_system_name = PsNameVirtualSystemName(normalized);
  if (!virtual_system_name.empty()) {
    if (decoded->include_persisted_relation_descriptor) {
      return ErrorFrame(
          {PsRelationProjectionDiagnostic(
              "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUIRED",
              "parser_server_ipc.relation_descriptor_required",
              "A virtual system relation has no persisted MGA relation descriptor.",
              "persisted_descriptor_required")},
          frame.header.request_uuid,
          frame.header.sequence_number,
          static_cast<std::uint16_t>(
              sbps::MessageType::kResolveNameResult));
    }
    WritePsNameResolutionTrace(*decoded,
                               &*session,
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
    const auto virtual_uuid = PsNameSyntheticUuid(virtual_system_name);
    const auto virtual_payload = relation_projection_schema
        ? EncodePsNameResolvePayloadV3(
              "resolved",
              virtual_uuid,
              virtual_system_name,
              decoded->object_class.empty() ? "relation"
                                            : decoded->object_class,
              1,
              1,
              "public virtual system object",
              nullptr)
        : std::optional<std::vector<std::uint8_t>>(
              EncodePsNameResolvePayload(
                  "resolved",
                  virtual_uuid,
                  virtual_system_name,
                  decoded->object_class.empty() ? "relation"
                                                : decoded->object_class,
                  1,
                  1,
                  "public virtual system object"));
    if (!virtual_payload) {
      return ErrorFrame(
          {PsRelationProjectionDiagnostic(
              "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE",
              "parser_server_ipc.relation_descriptor_too_large",
              "The public relation result exceeds the V3 projection limit.")},
          frame.header.request_uuid,
          frame.header.sequence_number,
          static_cast<std::uint16_t>(
              sbps::MessageType::kResolveNameResult));
    }
    return PsNameResponseFrame(
        frame,
        static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
        response_schema,
        *virtual_payload,
        false);
  }
  const auto parts = PsNameSplitPresentedName(decoded->presented_name, decoded->quoted);
  if (session && parts && !parts->empty()) {
    const std::string identifier_profile =
        PsNameCanonicalIdentifierProfile(decoded->dialect_profile);
    const std::string cache_key =
        PsNameResolutionCacheKey(*session, *decoded, identifier_profile);
    const std::string stable_cache_key =
        PsNameStableResolutionCacheKey(*session, *decoded, identifier_profile);
    const bool resource_resolution_request =
        PsNameLower(decoded->object_class) == "charset" ||
        PsNameLower(decoded->object_class) == "collation";
    // A marked view's semantic variant is engine-owned, transaction-visible
    // metadata. Generic name caches do not retain that descriptor, so view
    // resolution remains uncached and is classified on the exact selector.
    const bool semantic_view_resolution_request =
        relation_projection_schema &&
        PsNameLower(decoded->object_class) == "view";
    // A V3 persisted-relation-descriptor request is also transaction-visible
    // engine metadata.  Ordinary name-cache entries carry only the V1 name
    // payload; returning one under a V3 response schema would omit the
    // required descriptor and can expose stale relation identity.  Resolve
    // these requests against the exact engine/MGA context instead.
    const bool descriptor_resolution_request =
        relation_projection_schema &&
        decoded->include_persisted_relation_descriptor;
    bool normal_cache_checked =
        !decoded->bypass_cache && !resource_resolution_request &&
        !semantic_view_resolution_request &&
        !descriptor_resolution_request;
    bool normal_cache_hit = false;
    bool stable_cache_checked = false;
    bool stable_cache_hit = false;
    if (!decoded->bypass_cache && !resource_resolution_request &&
        !semantic_view_resolution_request &&
        !descriptor_resolution_request) {
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
              response_schema,
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
              response_schema,
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
    // Registry bootstrap entries are materialized under the request's
    // identifier profile.  Keep the engine context and identifier atoms on
    // the same profile so one parser-family request cannot be compared
    // against another profile's cached registry image.
    request.context.identifier_profile_uuid = identifier_profile;
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
        if (!decoded->bypass_cache && !resource_resolution_request &&
            !semantic_view_resolution_request &&
            !descriptor_resolution_request &&
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
        const std::string response_name =
            resolved.resource_descriptor.present
                ? resolved.resource_descriptor.canonical_name
                : decoded->presented_name;
        PsNameSemanticDetailResult semantic_detail;
        semantic_detail.detail = "engine catalog resolver";
        if (relation_projection_schema) {
          semantic_detail = BuildPsNameSemanticDetail(
              request.context,
              resolved.primary_object.uuid.canonical,
              resolved_object_class,
              semantic_detail.detail,
              &resolved.semantic_projection);
        }
        if (!semantic_detail.ok) {
          return ErrorFrame(
              {semantic_detail.diagnostic},
              frame.header.request_uuid,
              frame.header.sequence_number,
              static_cast<std::uint16_t>(
                  sbps::MessageType::kResolveNameResult));
        }
        std::vector<std::uint8_t> response_payload;
        if (relation_projection_schema) {
          const auto projected = BuildPsNameResolvedPayloadV3(
              *decoded,
              request.context,
              *object_uuid,
              response_name,
              resolved_object_class,
              catalog_epoch,
              security_epoch,
              semantic_detail.detail);
          if (!projected.ok) {
            return ErrorFrame(
                {projected.diagnostic},
                frame.header.request_uuid,
                frame.header.sequence_number,
                static_cast<std::uint16_t>(
                    sbps::MessageType::kResolveNameResult));
          }
          response_payload = projected.payload;
        } else {
          response_payload = EncodePsNameResolvePayload(
              "resolved",
              *object_uuid,
              response_name,
              resolved_object_class,
              catalog_epoch,
              security_epoch,
              semantic_detail.detail,
              resolved.resource_descriptor.present
                  ? &resolved.resource_descriptor
                  : nullptr);
        }
        return PsNameResponseFrame(
            frame,
            static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
            response_schema,
            response_payload,
            false);
      }
    }
    if (parts->size() == 1 && !resource_resolution_request) {
      const auto registry_match = PsNameResolveUniqueRegistryLeaf(
          request.context,
          parts->back(),
          decoded->object_class,
          identifier_profile,
          transaction_routed);
      if (registry_match &&
          PsNameRegistryMatchVisibleForSession(request.context, *registry_match)) {
        const auto object_uuid = PsNameUuidFromText(registry_match->object_uuid);
        if (object_uuid) {
          if (!decoded->bypass_cache &&
              !semantic_view_resolution_request &&
              !descriptor_resolution_request &&
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
          const auto registry_catalog_epoch =
              registry_match->catalog_generation_id == 0
                  ? session->catalog_generation
                  : registry_match->catalog_generation_id;
          PsNameSemanticDetailResult semantic_detail;
          semantic_detail.detail = "engine name registry resolver";
          if (relation_projection_schema) {
            semantic_detail = BuildPsNameSemanticDetail(
                request.context,
                registry_match->object_uuid,
                registry_match->object_class,
                semantic_detail.detail,
                nullptr);
          }
          if (!semantic_detail.ok) {
            return ErrorFrame(
                {semantic_detail.diagnostic},
                frame.header.request_uuid,
                frame.header.sequence_number,
                static_cast<std::uint16_t>(
                    sbps::MessageType::kResolveNameResult));
          }
          std::vector<std::uint8_t> response_payload;
          if (relation_projection_schema) {
            const auto projected = BuildPsNameResolvedPayloadV3(
                *decoded,
                request.context,
                *object_uuid,
                decoded->presented_name,
                registry_match->object_class,
                registry_catalog_epoch,
                session->security_epoch,
                semantic_detail.detail);
            if (!projected.ok) {
              return ErrorFrame(
                  {projected.diagnostic},
                  frame.header.request_uuid,
                  frame.header.sequence_number,
                  static_cast<std::uint16_t>(
                      sbps::MessageType::kResolveNameResult));
            }
            response_payload = projected.payload;
          } else {
            response_payload = EncodePsNameResolvePayload(
                "resolved",
                *object_uuid,
                decoded->presented_name,
                registry_match->object_class,
                registry_catalog_epoch,
                session->security_epoch,
                semantic_detail.detail);
          }
          return PsNameResponseFrame(
              frame,
              static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
              response_schema,
              response_payload,
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
  const auto not_found_payload = relation_projection_schema
      ? EncodePsNameResolvePayloadV3(
            "not_found_or_not_visible",
            {},
            "",
            decoded->object_class,
            1,
            1,
            "public resolver returned no UUID",
            nullptr)
      : std::optional<std::vector<std::uint8_t>>(
            EncodePsNameResolvePayload(
                "not_found_or_not_visible",
                {},
                "",
                decoded->object_class,
                1,
                1,
                "public resolver returned no UUID"));
  if (!not_found_payload) {
    return ErrorFrame(
        {PsRelationProjectionDiagnostic(
            "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE",
            "parser_server_ipc.relation_descriptor_too_large",
            "The public relation result exceeds the V3 projection limit.")},
        frame.header.request_uuid,
        frame.header.sequence_number,
        static_cast<std::uint16_t>(
            sbps::MessageType::kResolveNameResult));
  }
  return PsNameResponseFrame(
      frame,
      static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult),
      response_schema,
      *not_found_payload,
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
  const auto session = session_registry->sessions_by_uuid.find(
      UuidBytesToText(frame.header.session_uuid));
  const bool exact_binding =
      session != session_registry->sessions_by_uuid.end() &&
      !sbps::IsZeroUuid(frame.header.connection_uuid) &&
      !sbps::IsZeroUuid(session->second.connection_uuid) &&
      frame.header.connection_uuid == session->second.connection_uuid;
  if (!exact_binding) {
    return ErrorFrame(
        {sbps::IpcDiagnostic(
            "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
            "parser_server_ipc.route_association_mismatch",
            "UUID rendering requires exact connection and session binding.")},
        frame.header.request_uuid,
        frame.header.sequence_number,
        static_cast<std::uint16_t>(sbps::MessageType::kRenderUuidResult));
  }
  if (session->second.transaction_routing_v2_negotiated) {
    return ErrorFrame(
        {sbps::IpcDiagnostic(
            "PARSER_SERVER_IPC.TRANSACTIONAL_UUID_RENDER_UNSUPPORTED",
            "parser_server_ipc.transactional_uuid_render_unsupported",
            "Shared V1 UUID rendering is prohibited for transaction-routed sessions.")},
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
                       ClientNegotiationState* negotiation_state,
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
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kAcquireStatementContextRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kNegotiateLiteralDescriptorsRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kFetch) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kClosePreparedSblr) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kBeginParameterExecutionCoordinationRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kBeginVariableFrameRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kNegotiateVariableDescriptorsRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kFinalizeVariableBindingRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kCloseVariableFrameRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kAssignVariableValuesRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kIssueSourceMapRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kIssueErrorVectorRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kReserveSavepointRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kReserveAutonomousFrameRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kCoordinateReservationReleaseRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateTemporaryInstanceCleanupRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateCursorOpenRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateReadByKeyRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAccessCursorOpenRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAccessCursorFetchRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAccessCursorCloseRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateInsertRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateUpdateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDeleteRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateMergeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateTableTruncateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateTableAnalyzeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateBulkImportStreamRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateBulkExportStreamRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateStatementBatchRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAtomicCasRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAtomicRmwRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAdvisoryLockRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAdvisoryLockReleaseRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateFunctionCallRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateOperatorCallRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateCastRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateCompareRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDomainOperationRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateUdrInvokeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateProcedureInvokeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateFunctionInvokeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAggregateInvokeRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSequenceNextvalRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSequenceCurrvalRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSequenceSetvalRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateQueryNumericRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAdvancedDatatypeFamilyRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateProjectRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAggregateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateGroupRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSortRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateLimitRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateWindowRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateReturnResultSetRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateKvStructuredReadRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateKvStructuredMutateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateKvStructuredScanRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateKvStructuredStreamReadRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateKvStructuredStreamAppendRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateKvStructuredTimeseriesRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSystemConfigSetRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateDomainRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateSchemaRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateTableRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateIndexRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropIndexRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterDomainRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateViewRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterViewRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropViewRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateTriggerRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterTriggerRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropTriggerRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateProcedureRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterProcedureRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropProcedureRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateFunctionRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterFunctionRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropFunctionRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreatePackageRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateTemporaryTableRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropTemporaryTableRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlRenameObjectVectorRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateOrReplaceSrsRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropSrsRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateRewriteRuleRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterRewriteRuleRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropRewriteRuleRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlValidateConstraintRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityCreatePrivilegeTemplateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityAlterPrivilegeTemplateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateSecurityDropPrivilegeTemplateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDatabaseCreateTemplateCloneRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateAggregateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlAlterAggregateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropAggregateRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlPurgeSystemHistoryRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlSetIndexOptimizerEligibilityRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlSetTableTypeEnforcementRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDatabaseSerializeLogicalSnapshotRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDatabaseDeserializeLogicalSnapshotRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlCreateMacroRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateDdlDropMacroRequest) ||
      frame.header.message_type == static_cast<std::uint16_t>(sbps::MessageType::kCoordinateAdminRegisterExternalRelationResolverRequest) ||
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
  if (session_bound_message && sbps::IsZeroUuid(frame.header.session_uuid)) {
    WriteAll(client_fd,
             ErrorFrame(
                 {sbps::IpcDiagnostic(
                     "PARSER_SERVER_IPC.SESSION_REQUIRED",
                     "parser_server_ipc.session_required",
                     "A session-bound frame requires an exact nonzero session UUID.")},
                 frame.header.request_uuid,
                 frame.header.sequence_number));
    return false;
  }
  if (session_bound_message) {
    bool exact_physical_binding = false;
    if (session_registry != nullptr && negotiation_state != nullptr &&
        negotiation_state->hello_admitted &&
        !sbps::IsZeroUuid(negotiation_state->server_channel_uuid) &&
        !sbps::IsZeroUuid(frame.header.connection_uuid)) {
      const auto session_it = session_registry->sessions_by_uuid.find(
          UuidBytesToText(frame.header.session_uuid));
      const auto owner_it =
          session_registry->physical_channel_by_connection_uuid.find(
              UuidBytesToText(frame.header.connection_uuid));
      exact_physical_binding =
          session_it != session_registry->sessions_by_uuid.end() &&
          owner_it !=
              session_registry->physical_channel_by_connection_uuid.end() &&
          owner_it->second == negotiation_state->server_channel_uuid &&
          session_it->second.server_channel_uuid ==
              negotiation_state->server_channel_uuid &&
          session_it->second.connection_uuid == frame.header.connection_uuid;
    }
    if (!exact_physical_binding) {
      WriteAll(client_fd,
               ErrorFrame(
                   {sbps::IpcDiagnostic(
                       "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                       "parser_server_ipc.route_association_mismatch",
                       "The session-bound frame does not belong to this physical parser channel.")},
                   frame.header.request_uuid,
                   frame.header.sequence_number));
      return false;
    }
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
    const bool repeated_hello =
        negotiation_state != nullptr && negotiation_state->hello_admitted;
    std::array<std::uint8_t, 32> requested_capabilities{};
    requested_capabilities[0] =
        hello->capability_bitmap[0] & sbps::kKnownCapabilityByte0;
    const bool capabilities_unchanged =
        !repeated_hello ||
        requested_capabilities ==
            negotiation_state->accepted_capability_bitmap;
    const bool hello_identity_unchanged =
        !repeated_hello ||
        (frame.header.protocol_major ==
             negotiation_state->admitted_protocol_major &&
         frame.header.protocol_minor ==
             negotiation_state->admitted_protocol_minor &&
         frame.payload == negotiation_state->admitted_hello_payload);
    if (!ParserChannelHelloMayBeAdmittedForTest(
            repeated_hello,
            capabilities_unchanged,
            hello_identity_unchanged,
            negotiation_state != nullptr &&
                negotiation_state->connection_authenticated)) {
      WriteAll(client_fd,
               ErrorFrame(
                   {sbps::IpcDiagnostic(
                       "PARSER_SERVER_IPC.HELLO_RENEGOTIATION_REFUSED",
                       "parser_server_ipc.hello_renegotiation_refused",
                       "An admitted physical parser channel cannot change its negotiated capabilities.")},
                   frame.header.request_uuid,
                   frame.header.sequence_number,
                   static_cast<std::uint16_t>(
                       sbps::MessageType::kHelloReject)));
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
    WriteAll(client_fd, AcceptFrame(frame, config, negotiation_state));
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
    if (session_registry == nullptr || negotiation_state == nullptr ||
        !negotiation_state->hello_admitted ||
        sbps::IsZeroUuid(negotiation_state->server_channel_uuid) ||
        sbps::IsZeroUuid(frame.header.connection_uuid)) {
      WriteAll(client_fd,
               ErrorFrame(
                   {sbps::IpcDiagnostic(
                       "PARSER_SERVER_IPC.HELLO_REQUIRED",
                       "parser_server_ipc.hello_required",
                       "Authentication requires an admitted hello on this physical parser channel.")},
                   frame.header.request_uuid,
                   frame.header.sequence_number,
                   static_cast<std::uint16_t>(sbps::MessageType::kAuthResult)));
      return false;
    }
    const std::string connection_key =
        UuidBytesToText(frame.header.connection_uuid);
    const auto existing_owner =
        session_registry->physical_channel_by_connection_uuid.find(
            connection_key);
    if (existing_owner !=
            session_registry->physical_channel_by_connection_uuid.end() &&
        existing_owner->second != negotiation_state->server_channel_uuid) {
      WriteAll(client_fd,
               ErrorFrame(
                   {sbps::IpcDiagnostic(
                       "PARSER_SERVER_IPC.CONNECTION_REPLAY_REFUSED",
                       "parser_server_ipc.connection_replay_refused",
                       "The connection UUID is already owned by another physical parser channel.")},
                   frame.header.request_uuid,
                   frame.header.sequence_number,
                   static_cast<std::uint16_t>(sbps::MessageType::kAuthResult)));
      return false;
    }
    session_registry->physical_channel_by_connection_uuid.insert_or_assign(
        connection_key, negotiation_state->server_channel_uuid);
    session_registry->negotiated_capabilities_by_connection_uuid
        .insert_or_assign(connection_key,
                          negotiation_state->accepted_capability_bitmap);
    const auto admitted_hello = sbps::DecodeHelloRequest(
        negotiation_state->admitted_hello_payload);
    if (admitted_hello.has_value()) {
      ServerAdmittedParserChannelIdentity identity;
      identity.parser_package_uuid = admitted_hello->parser_package_uuid;
      identity.dialect_profile_uuid = admitted_hello->dialect_profile_uuid;
      identity.parser_package_version_major =
          admitted_hello->parser_api_major;
      identity.parser_package_version_minor =
          admitted_hello->parser_api_minor;
      identity.parser_package_version_patch = 0;
      session_registry->admitted_parser_identity_by_connection_uuid
          .insert_or_assign(connection_key, identity);
    }
    const auto result = HandleAuthHandoff(session_registry, engine_state, frame);
    if (result.accepted && negotiation_state != nullptr) {
      negotiation_state->connection_authenticated = true;
    }
    if (!result.accepted) {
      const auto owner =
          session_registry->physical_channel_by_connection_uuid.find(
              connection_key);
      if (owner !=
              session_registry->physical_channel_by_connection_uuid.end() &&
          owner->second == negotiation_state->server_channel_uuid) {
        session_registry->physical_channel_by_connection_uuid.erase(owner);
        session_registry->negotiated_capabilities_by_connection_uuid.erase(
            connection_key);
        session_registry->admitted_parser_identity_by_connection_uuid.erase(
            connection_key);
      }
    }
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
    bool exact_attach_channel = false;
    if (session_registry != nullptr && negotiation_state != nullptr &&
        negotiation_state->hello_admitted &&
        !sbps::IsZeroUuid(negotiation_state->server_channel_uuid) &&
        !sbps::IsZeroUuid(frame.header.connection_uuid)) {
      const auto owner =
          session_registry->physical_channel_by_connection_uuid.find(
              UuidBytesToText(frame.header.connection_uuid));
      exact_attach_channel =
          owner !=
              session_registry->physical_channel_by_connection_uuid.end() &&
          owner->second == negotiation_state->server_channel_uuid;
    }
    if (!exact_attach_channel) {
      WriteAll(client_fd,
               ErrorFrame(
                   {sbps::IpcDiagnostic(
                       "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                       "parser_server_ipc.route_association_mismatch",
                       "Attach does not belong to the physical parser channel that authenticated the connection.")},
                   frame.header.request_uuid,
                   frame.header.sequence_number,
                   static_cast<std::uint16_t>(sbps::MessageType::kAttachResult)));
      return false;
    }
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
  if (frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kAcquireStatementContextRequest)) {
    WriteAll(client_fd,
             SessionOperationFrame(
                 frame,
                 HandleAcquireStatementContext(
                     session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type == static_cast<std::uint16_t>(
          sbps::MessageType::kNegotiateLiteralDescriptorsRequest) &&
      frame.header.payload_schema_id ==
          sbps::kSchemaNegotiateLiteralDescriptorsRequestV1) {
    WriteAll(client_fd,SessionOperationFrame(
        frame,HandleNegotiateLiteralDescriptors(session_registry,frame)));
    return true;
  }
  if (frame.header.message_type == 40 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaFinalizeLiteralBindingRequestV1) {
    WriteAll(client_fd,SessionOperationFrame(
        frame,HandleFinalizeLiteralBinding(session_registry,frame)));
    return true;
  }
  if (frame.header.message_type == 42 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaNegotiateParameterDescriptorsRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleNegotiateParameterDescriptors(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type == 50 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaBeginParameterExecutionCoordinationRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleBeginParameterExecutionCoordination(
                   session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type == 56 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaBeginVariableFrameRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleBeginVariableFrame(session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type == 52 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaNegotiateVariableDescriptorsRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleNegotiateVariableDescriptors(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type == 54 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaFinalizeVariableBindingRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleFinalizeVariableBinding(
                   session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type == 58 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaCloseVariableFrameRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleCloseVariableFrame(session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type == 60 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaAssignVariableValuesRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleAssignVariableValues(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type == 62 &&
      frame.header.payload_schema_id == sbps::kSchemaIssueSourceMapRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleIssueSourceMapDescriptor(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type == 64 &&
      frame.header.payload_schema_id == sbps::kSchemaIssueErrorVectorRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleIssueErrorVectorDescriptor(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type == 66 &&
      frame.header.payload_schema_id == sbps::kSchemaReserveSavepointRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleReserveSavepoint(session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type == 68 &&
      frame.header.payload_schema_id == sbps::kSchemaReserveAutonomousFrameRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleReserveAutonomousFrame(session_registry, engine_state, frame)));
    return true;
  }
  if(frame.header.message_type==72&&frame.header.payload_schema_id==sbps::kSchemaCoordinateReservationReleaseRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateReservationRelease(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==76&&frame.header.payload_schema_id==sbps::kSchemaCoordinateTemporaryInstanceCleanupRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateTemporaryInstanceCleanup(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==78&&frame.header.payload_schema_id==sbps::kSchemaCoordinateCursorOpenRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateCursorOpen(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==80&&frame.header.payload_schema_id==sbps::kSchemaCoordinateReadByKeyRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateReadByKey(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==82&&frame.header.payload_schema_id==sbps::kSchemaCoordinateReadRangeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateReadRange(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==84&&frame.header.payload_schema_id==sbps::kSchemaCoordinateReadStreamRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateReadStream(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==86&&frame.header.payload_schema_id==sbps::kSchemaCoordinateResultSetPassRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateResultSetPass(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==88&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAccessCursorOpenRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAccessCursorOpen(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==90&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAccessCursorFetchRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAccessCursorFetch(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==92&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAccessCursorCloseRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAccessCursorClose(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==94&&frame.header.payload_schema_id==sbps::kSchemaCoordinateInsertRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateInsert(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==96&&frame.header.payload_schema_id==sbps::kSchemaCoordinateUpdateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateUpdate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==98&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDeleteRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDelete(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==100&&frame.header.payload_schema_id==sbps::kSchemaCoordinateMergeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateMerge(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==102&&frame.header.payload_schema_id==sbps::kSchemaCoordinateTableTruncateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateTableTruncate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==104&&frame.header.payload_schema_id==sbps::kSchemaCoordinateTableAnalyzeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateTableAnalyze(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==106&&frame.header.payload_schema_id==sbps::kSchemaCoordinateBulkImportStreamRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateBulkImportStream(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==108&&frame.header.payload_schema_id==sbps::kSchemaCoordinateBulkExportStreamRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateBulkExportStream(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==110&&frame.header.payload_schema_id==sbps::kSchemaCoordinateStatementBatchRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateStatementBatch(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==112&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAtomicCasRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAtomicCas(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==114&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAtomicRmwRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAtomicRmw(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==116&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAdvisoryLockRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAdvisoryLock(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==118&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAdvisoryLockReleaseRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAdvisoryLockRelease(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==120&&frame.header.payload_schema_id==sbps::kSchemaCoordinateFunctionCallRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateFunctionCall(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==122&&frame.header.payload_schema_id==sbps::kSchemaCoordinateOperatorCallRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateOperatorCall(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==124&&frame.header.payload_schema_id==sbps::kSchemaCoordinateCastRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateCast(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==126&&frame.header.payload_schema_id==sbps::kSchemaCoordinateCompareRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateCompare(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==128&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDomainOperationRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDomainOperation(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==130&&frame.header.payload_schema_id==sbps::kSchemaCoordinateUdrInvokeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateUdrInvoke(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==132&&frame.header.payload_schema_id==sbps::kSchemaCoordinateProcedureInvokeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateProcedureInvoke(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==134&&frame.header.payload_schema_id==sbps::kSchemaCoordinateFunctionInvokeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateFunctionInvoke(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==136&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAggregateInvokeRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAggregateInvoke(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==138&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSequenceNextvalRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSequenceNextval(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==140&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSequenceCurrvalRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSequenceCurrval(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==142&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSequenceSetvalRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSequenceSetval(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==144&&frame.header.payload_schema_id==sbps::kSchemaCoordinateQueryNumericRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateQueryNumeric(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==146&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAdvancedDatatypeFamilyRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAdvancedDatatypeFamily(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==148&&frame.header.payload_schema_id==sbps::kSchemaCoordinateProjectRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateProject(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==150&&frame.header.payload_schema_id==sbps::kSchemaCoordinateAggregateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateAggregate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==152&&frame.header.payload_schema_id==sbps::kSchemaCoordinateGroupRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateGroup(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==154&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSortRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSort(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==156&&frame.header.payload_schema_id==sbps::kSchemaCoordinateLimitRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateLimit(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==158&&frame.header.payload_schema_id==sbps::kSchemaCoordinateWindowRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateWindow(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==160&&frame.header.payload_schema_id==sbps::kSchemaCoordinateReturnResultSetRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateReturnResultSet(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==162&&frame.header.payload_schema_id==sbps::kSchemaCoordinateKvStructuredReadRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateKvStructuredRead(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==164&&frame.header.payload_schema_id==sbps::kSchemaCoordinateKvStructuredMutateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateKvStructuredMutate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==166&&frame.header.payload_schema_id==sbps::kSchemaCoordinateKvStructuredScanRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateKvStructuredScan(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==168&&frame.header.payload_schema_id==sbps::kSchemaCoordinateKvStructuredStreamReadRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateKvStructuredStreamRead(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==170&&frame.header.payload_schema_id==sbps::kSchemaCoordinateKvStructuredStreamAppendRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateKvStructuredStreamAppend(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==172&&frame.header.payload_schema_id==sbps::kSchemaCoordinateKvStructuredTimeseriesRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateKvStructuredTimeseries(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==174&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSystemConfigSetRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSystemConfigSet(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==176&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateDomainRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateDomain(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==178&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateSchemaRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateSchema(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==180&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateTableRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateTable(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==182&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateIndexRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateIndex(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==184&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropIndexRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropIndex(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==186&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterDomainRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterDomain(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==188&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateViewRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateView(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==190&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterViewRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterView(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==192&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropViewRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropView(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==194&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateTriggerRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateTrigger(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==196&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterTriggerRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterTrigger(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==198&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropTriggerRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropTrigger(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==200&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateProcedureRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateProcedure(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==202&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterProcedureRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterProcedure(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==204&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropProcedureRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropProcedure(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==206&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateFunctionRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateFunction(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==208&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterFunctionRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterFunction(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==210&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropFunctionRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropFunction(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==212&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreatePackageRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreatePackage(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==214&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateTemporaryTableRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateTemporaryTable(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==216&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropTemporaryTableRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropTemporaryTable(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==218&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlRenameObjectVectorRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlRenameObjectVector(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==220&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateOrReplaceSrsRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateOrReplaceSrs(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==222&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropSrsRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropSrs(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==224&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateRewriteRuleRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateRewriteRule(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==226&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterRewriteRuleRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterRewriteRule(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==228&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropRewriteRuleRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropRewriteRule(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==230&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlValidateConstraintRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlValidateConstraint(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==232&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSecurityCreatePrivilegeTemplateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSecurityCreatePrivilegeTemplate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==234&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSecurityAlterPrivilegeTemplateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSecurityAlterPrivilegeTemplate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==236&&frame.header.payload_schema_id==sbps::kSchemaCoordinateSecurityDropPrivilegeTemplateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateSecurityDropPrivilegeTemplate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==238&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDatabaseCreateTemplateCloneRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDatabaseCreateTemplateClone(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==240&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateAggregateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateAggregate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==242&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlAlterAggregateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlAlterAggregate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==244&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropAggregateRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropAggregate(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==246&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlPurgeSystemHistoryRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlPurgeSystemHistory(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==248&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlSetIndexOptimizerEligibilityRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlSetIndexOptimizerEligibility(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==250&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlSetTableTypeEnforcementRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlSetTableTypeEnforcement(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==252&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDatabaseSerializeLogicalSnapshotRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDatabaseSerializeLogicalSnapshot(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==254&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDatabaseDeserializeLogicalSnapshotRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDatabaseDeserializeLogicalSnapshot(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==256&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlCreateMacroRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlCreateMacro(session_registry,engine_state,frame)));return true;}
  if(frame.header.message_type==258&&frame.header.payload_schema_id==sbps::kSchemaCoordinateDdlDropMacroRequestV1){WriteAll(client_fd,SessionOperationFrame(frame,HandleCoordinateDdlDropMacro(session_registry,engine_state,frame)));return true;}
  if (frame.header.message_type == 44 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaFinalizeParameterBindingRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleFinalizeParameterBinding(session_registry, frame)));
    return true;
  }
  if (frame.header.message_type == 40 &&
      frame.header.payload_schema_id ==
          sbps::kSchemaFinalizePreparedSblrParameterRequestV1) {
    WriteAll(client_fd, SessionOperationFrame(
        frame, HandleFinalizePreparedSblrParameter(session_registry, frame)));
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
      auto refused = RejectPrepareSblrBeforeEngine(
          frame,
          "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
          "sblr_admission_fenced");
      refused.diagnostics.push_back(MaintenanceAdmissionDiagnostic(
          *maintenance_coordinator,
          "prepare_sblr",
          "sblr_admission_fenced"));
      WriteAll(client_fd, SessionOperationFrame(frame, refused));
      return true;
    }
    WriteAll(client_fd, SessionOperationFrame(
                           frame, HandlePrepareSblr(session_registry, engine_state, frame)));
    return true;
  }
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr)) {
    if (maintenance_coordinator != nullptr && !MaintenanceAllowsSblr(*maintenance_coordinator)) {
      auto refused = RejectExecuteSblrBeforeEngine(
          frame,
          "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
          "sblr_admission_fenced");
      refused.diagnostics.push_back(MaintenanceAdmissionDiagnostic(
          *maintenance_coordinator,
          "execute_sblr",
          "sblr_admission_fenced"));
      WriteAll(client_fd, SessionOperationFrame(frame, refused));
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
    std::string execute_audit_detail = "SBLR execute processed";
    if (!operation.accepted && !operation.diagnostics.empty() &&
        !operation.diagnostics.front().internal_audit_key.empty()) {
      execute_audit_detail += ":" +
          operation.diagnostics.front().internal_audit_key;
    }
    RecordServerAuditEvent(observability,
                           "server.sblr.execute",
                           operation.accepted ? "completed" : "rejected",
                           execute_audit_detail,
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
  if (frame.header.message_type ==
      static_cast<std::uint16_t>(sbps::MessageType::kClosePreparedSblr)) {
    WriteAll(client_fd,
             SessionOperationFrame(
                 frame, HandleClosePreparedSblr(session_registry, frame)));
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

void ResetParserServerStopRequest() {
  g_stop_requested.store(false, std::memory_order_release);
}

void RequestParserServerStop() {
  g_stop_requested.store(true, std::memory_order_release);
}

bool ParserServerStopRequested() {
  return g_stop_requested.load(std::memory_order_acquire);
}

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

std::vector<SessionOperationResult> HandleUnexpectedParserChannelClose(
    ServerSessionRegistry* session_registry,
    const std::array<std::uint8_t, 16>& server_channel_uuid) {
  std::vector<SessionOperationResult> results;
  if (session_registry == nullptr || sbps::IsZeroUuid(server_channel_uuid)) {
    return results;
  }

  struct OwnedSessionBinding {
    std::array<std::uint8_t, 16> session_uuid{};
    std::array<std::uint8_t, 16> connection_uuid{};
  };
  std::vector<OwnedSessionBinding> owned_sessions;
  for (const auto& [_, session] : session_registry->sessions_by_uuid) {
    if (session.server_channel_uuid == server_channel_uuid) {
      owned_sessions.push_back(
          {session.session_uuid, session.connection_uuid});
    }
  }
  std::sort(owned_sessions.begin(),
            owned_sessions.end(),
            [](const OwnedSessionBinding& left,
               const OwnedSessionBinding& right) {
              return left.session_uuid < right.session_uuid;
            });

  for (const auto& owned : owned_sessions) {
    sbps::Frame trusted_disconnect;
    trusted_disconnect.header.message_type =
        static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
    trusted_disconnect.header.request_uuid = sbps::MakeUuidV7Bytes();
    trusted_disconnect.header.connection_uuid = owned.connection_uuid;
    trusted_disconnect.header.session_uuid = owned.session_uuid;
    trusted_disconnect.payload.insert(trusted_disconnect.payload.end(),
                                      owned.session_uuid.begin(),
                                      owned.session_uuid.end());
    PutString(&trusted_disconnect.payload,
              "physical_parser_channel_lost");
    results.push_back(
        HandleDisconnectNotice(session_registry, trusted_disconnect));
  }

  // A dead physical channel can no longer own a reusable connection binding.
  // Unknown transaction outcomes remain in their quarantined session record;
  // removing transport ownership does not resolve or discard MGA finality.
  for (auto it =
           session_registry->physical_channel_by_connection_uuid.begin();
       it != session_registry->physical_channel_by_connection_uuid.end();) {
    if (it->second == server_channel_uuid) {
      session_registry->negotiated_capabilities_by_connection_uuid.erase(
          it->first);
      session_registry->admitted_parser_identity_by_connection_uuid.erase(
          it->first);
      it = session_registry->physical_channel_by_connection_uuid.erase(it);
    } else {
      ++it;
    }
  }
  return results;
}

bool ParserChannelHelloMayBeAdmittedForTest(
    bool hello_already_admitted,
    bool capability_bitmap_unchanged,
    bool hello_identity_unchanged,
    bool connection_authenticated) {
  return !hello_already_admitted ||
         (!connection_authenticated && capability_bitmap_unchanged &&
          hello_identity_unchanged);
}

ServerIpcEndpointResult RunParserServerIpcEndpoint(const ServerBootstrapConfig& config,
                                                   const ServerLifecycleArtifacts& artifacts,
                                                   const HostedEngineState& engine_state,
                                                   const ParserServerIpcLifecycleCallbacks& callbacks) {
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
  if (!ParserServerStopRequested() && callbacks.on_ready) {
    callbacks.on_ready();
  }
  ServerMaintenanceCoordinator maintenance_coordinator = BuildMaintenanceCoordinator(config, artifacts);
  ServerObservabilityState observability =
      InitializeServerObservability(config, artifacts, engine_state, parser_registry, listener_orchestrator);
  std::mutex client_dispatch_mutex;
  std::vector<std::thread> client_threads;

  while (!ParserServerStopRequested()) {
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
      ClientNegotiationState negotiation_state;
      while (!ParserServerStopRequested()) {
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
                                        &negotiation_state,
                                        &release_heap_after_close);
          if (maintenance_coordinator.shutdown_requested) {
            RequestParserServerStop();
          }
        }
        if (!keep_open) {
          break;
        }
      }
      {
        std::lock_guard<std::mutex> dispatch_guard(client_dispatch_mutex);
        const auto channel_cleanup = HandleUnexpectedParserChannelClose(
            &session_registry, negotiation_state.server_channel_uuid);
        if (!channel_cleanup.empty()) {
          release_heap_after_close = true;
          SetServerMetric(
              &observability,
              "sys.metrics.server.session.active",
              static_cast<std::uint64_t>(
                  session_registry.sessions_by_uuid.size()));
          RecordServerAuditEvent(
              &observability,
              "server.parser_channel_lost",
              "cleanup_attempted",
              "server-owned cleanup processed all sessions bound to a closed physical parser channel");
        }
      }
      CloseIpcSocket(client_fd);
      if (release_heap_after_close) {
        std::lock_guard<std::mutex> dispatch_guard(client_dispatch_mutex);
        ReleaseIdleConnectionHeap(config, &observability);
      }
    });
  }

  RequestParserServerStop();
  if (callbacks.on_stopping) {
    callbacks.on_stopping();
  }
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
