// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBSQL_EMBEDDED_ENGINE_CLIENT

#include "embedded/embedded_engine_client.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
#include "engine_host.hpp"
#include "ipc_server.hpp"
#include "sbps.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#endif

namespace scratchbird::parser::sbsql {
namespace {

void AddDiagnostic(MessageVectorSet* messages,
                   std::string code,
                   std::string message,
                   std::string detail = {}) {
  if (messages == nullptr) return;
  std::vector<Field> fields;
  if (!detail.empty()) fields.push_back({"detail", std::move(detail)});
  messages->diagnostics.push_back(MakeDiagnostic(std::move(code),
                                                 "ERROR",
                                                 std::move(message),
                                                 "sbp_sbsql.embedded",
                                                 std::move(fields)));
}

std::string StripDevBootstrapPrefix(std::string value) {
  constexpr std::string_view kPrefix = "dev_bootstrap_path:";
  if (value.rfind(kPrefix, 0) == 0) value.erase(0, kPrefix.size());
  return value;
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (offset == nullptr || out == nullptr || *offset + 2 > data.size()) return false;
  const auto length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data,
                                     std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  if (offset + uuid.size() <= data.size()) {
    std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  }
  return uuid;
}

std::array<std::uint8_t, 16> TextToUuid(std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::size_t nibble = 0;
  for (char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return {};
    if ((nibble % 2) == 0) out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    else out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    ++nibble;
  }
  return nibble == 32 ? out : std::array<std::uint8_t, 16>{};
}

void PopulateTransactionStateFromPayload(std::string_view payload,
                                         ServerExecutionResult* result) {
  if (result == nullptr || payload.empty()) return;
  auto line_value = [&](std::string_view key) -> std::optional<std::string> {
    std::size_t pos = 0;
    while (pos < payload.size()) {
      const auto end = payload.find('\n', pos);
      const auto line = payload.substr(pos, end == std::string_view::npos
                                                ? std::string_view::npos
                                                : end - pos);
      if (line.rfind(key, 0) == 0 && line.size() > key.size() && line[key.size()] == '=') {
        return std::string(line.substr(key.size() + 1));
      }
      if (end == std::string_view::npos) break;
      pos = end + 1;
    }
    return std::nullopt;
  };
  auto parse_u64 = [](const std::string& value) -> std::uint64_t {
    try {
      return static_cast<std::uint64_t>(std::stoull(value));
    } catch (...) {
      return 0;
    }
  };
  const auto replacement_id = line_value("replacement_local_transaction_id");
  const auto active_id = line_value("local_transaction_id");
  if (replacement_id || active_id) {
    result->transaction_state_present = true;
    result->local_transaction_id = parse_u64(replacement_id.value_or(active_id.value_or("0")));
    result->snapshot_visible_through_local_transaction_id =
        parse_u64(line_value(replacement_id ? "replacement_snapshot_visible_through_local_transaction_id"
                                            : "snapshot_visible_through_local_transaction_id")
                      .value_or("0"));
    result->transaction_uuid =
        line_value(replacement_id ? "replacement_transaction_uuid" : "transaction_uuid").value_or("");
    result->transaction_timestamp =
        line_value(replacement_id ? "replacement_transaction_timestamp" : "transaction_timestamp")
            .value_or("");
  }
}

#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)

constexpr std::uint32_t kSchemaAuthHandoffV1 = 3001;
constexpr std::uint32_t kSchemaAttachRequestV1 = 3003;

void AddServerDiagnostics(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
                          MessageVectorSet* messages) {
  if (messages == nullptr) return;
  for (const auto& diagnostic : diagnostics) {
    std::vector<Field> fields;
    for (const auto& field : diagnostic.fields) {
      fields.push_back({field.key, field.value});
    }
    messages->diagnostics.push_back(MakeDiagnostic(
        diagnostic.code.empty() ? "PARSER_SERVER_IPC.EMBEDDED_REJECTED" : diagnostic.code,
        diagnostic.severity == scratchbird::server::ServerDiagnosticSeverity::kWarning ? "WARNING" : "ERROR",
        diagnostic.safe_message.empty() ? diagnostic.message_key : diagnostic.safe_message,
        "sbp_sbsql.embedded",
        std::move(fields)));
  }
}

scratchbird::server::sbps::Frame BaseConnectionFrame(
    scratchbird::server::sbps::MessageType message_type,
    std::uint32_t schema_id,
    const std::array<std::uint8_t, 16>& connection_uuid,
    const std::array<std::uint8_t, 16>& session_uuid = {}) {
  scratchbird::server::sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(message_type);
  frame.header.payload_schema_id = schema_id;
  frame.header.request_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  frame.header.sequence_number = 1;
  frame.header.connection_uuid = connection_uuid;
  frame.header.session_uuid = session_uuid;
  return frame;
}

scratchbird::server::sbps::Frame BaseFrame(std::uint16_t message_type,
                                           const SessionContext& session) {
  scratchbird::server::sbps::Frame frame;
  frame.header.message_type = message_type;
  frame.header.request_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  frame.header.sequence_number = 1;
  frame.header.session_uuid = TextToUuid(session.session_uuid);
  frame.header.connection_uuid = TextToUuid(session.connection_uuid);
  return frame;
}

std::vector<std::uint8_t> EncodeAuthPayload(
    const AuthCredentialEnvelope& credentials,
    const std::array<std::uint8_t, 16>& connection_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutU8(&out, credentials.credential_evidence_present ? 1 : 0);
  PutU8(&out, credentials.credential_invalid ? 1 : 0);
  PutU8(&out, credentials.mfa_required ? 1 : 0);
  PutU8(&out, credentials.mfa_evidence_present ? 1 : 0);
  PutString(&out, credentials.provider_family.empty() ? "local_password" : credentials.provider_family);
  PutString(&out, credentials.principal);
  PutString(&out, credentials.requested_database.empty() ? "default" : credentials.requested_database);
  PutString(&out, credentials.requested_language.empty() ? "en" : credentials.requested_language);
  PutString(&out, credentials.credential_evidence);
  PutString(&out, credentials.application_name);
  return out;
}

std::vector<std::uint8_t> EncodeAttachPayload(
    const std::array<std::uint8_t, 16>& connection_uuid,
    const std::array<std::uint8_t, 16>& auth_context_uuid,
    std::string_view requested_database) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, requested_database.empty() ? "default" : requested_database);
  PutString(&out, "read_write");
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayload(const SessionContext& session,
                                                   std::string_view presented_name,
                                                   bool quoted,
                                                   std::string_view object_class,
                                                   const ParserConfig& config) {
  std::vector<std::uint8_t> out;
  PutString(&out, presented_name);
  PutU8(&out, quoted ? 1 : 0);
  const std::string identifier_profile =
      session.dialect_profile_uuid.empty() ? "sbsql_v3" : session.dialect_profile_uuid;
  PutString(&out, identifier_profile);
  PutString(&out, session.default_language.empty() ? "en" : session.default_language);
  std::string search_path;
  for (const auto& item : session.search_path) {
    if (!search_path.empty()) search_path.push_back(',');
    search_path += item;
  }
  PutString(&out, search_path);
  PutString(&out, object_class);
  (void)config;
  return out;
}

std::vector<std::uint8_t> EncodeRenderUuidPayload(std::string_view object_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, TextToUuid(object_uuid));
  return out;
}

PublicNameResolutionResult DecodePublicNamePayload(const std::vector<std::uint8_t>& payload,
                                                   std::string_view success_outcome) {
  PublicNameResolutionResult result;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(payload, &offset, &outcome) || offset + 16 > payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The embedded public name response payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  const auto object_uuid = GetUuid(payload, offset);
  offset += 16;
  std::string canonical_name;
  std::string object_class;
  if (!ReadString(payload, &offset, &canonical_name) ||
      !ReadString(payload, &offset, &object_class) ||
      offset + 16 > payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The embedded public name response payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  result.catalog_epoch = GetU64(payload, offset);
  offset += 8;
  result.security_epoch = GetU64(payload, offset);
  offset += 8;
  if (outcome != success_outcome) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE",
        "ERROR",
        "object name could not be resolved or is not visible",
        "sbp_sbsql.embedded"));
    return result;
  }
  result.resolved = true;
  result.object_uuid = scratchbird::server::UuidBytesToText(object_uuid);
  result.canonical_name = canonical_name;
  result.object_class = object_class;
  return result;
}

ServerExecutionResult DecodeExecutePayload(
    const scratchbird::server::SessionOperationResult& operation) {
  ServerExecutionResult result;
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(operation.payload, &offset, &outcome) || outcome != "accepted" ||
      offset + 16 + 16 + 8 > operation.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
        "ERROR",
        "The embedded execute result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  offset += 16;
  result.cursor_uuid = scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
  offset += 16;
  result.row_count = GetU64(operation.payload, offset);
  offset += 8;
  if (!ReadString(operation.payload, &offset, &result.operation_id) ||
      !ReadString(operation.payload, &offset, &result.row_packet)) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
        "ERROR",
        "The embedded execute result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  PopulateTransactionStateFromPayload(result.row_packet, &result);
  result.accepted = true;
  return result;
}

#endif

}  // namespace

struct EmbeddedEngineClient::Impl {
  explicit Impl(ParserConfig cfg) : config(std::move(cfg)) {}

  ParserConfig config;

#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  scratchbird::server::HostedEngineState engine_state;
  scratchbird::server::ServerSessionRegistry registry;
  bool started = false;

  bool EnsureStarted(std::string requested_database, MessageVectorSet* messages) {
    if (started) return true;
    std::string database_path = StripDevBootstrapPrefix(config.embedded_database_path);
    if (database_path.empty()) database_path = StripDevBootstrapPrefix(config.database_token);
    if ((database_path.empty() || database_path == "default") &&
        !requested_database.empty() && requested_database != "default") {
      database_path = StripDevBootstrapPrefix(std::move(requested_database));
    }
    if (database_path.empty() || database_path == "default") {
      AddDiagnostic(messages,
                    "SBSQL.EMBEDDED.DATABASE_PATH_REQUIRED",
                    "embedded SBsql requires a database path");
      return false;
    }

    scratchbird::server::ServerBootstrapConfig server_config;
    server_config.database_default_path = database_path;
    server_config.database_auto_create = true;
    server_config.database_ownership_prelocked = config.embedded_database_ownership_prelocked;
    server_config.database_ownership_owner_kind = "embedded";
    server_config.embedded_direct_mode = true;
    server_config.database_open_mode = "normal";
    server_config.sbps_enabled = false;
    auto hosted = scratchbird::server::StartHostedEngine(server_config);
    if (!hosted.ok()) {
      AddServerDiagnostics(hosted.diagnostics, messages);
      if (messages != nullptr && messages->diagnostics.empty()) {
        AddDiagnostic(messages,
                      "SBSQL.EMBEDDED.ENGINE_OPEN_FAILED",
                      "embedded engine could not open the database");
      }
      return false;
    }
    engine_state = std::move(hosted.state);
    started = true;
    return true;
  }
#endif
};

EmbeddedEngineClient::EmbeddedEngineClient(ParserConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

EmbeddedEngineClient::~EmbeddedEngineClient() = default;

bool EmbeddedEngineClient::AuthenticateAndAttach(
    const AuthCredentialEnvelope& credentials,
    SessionContext* session,
    MessageVectorSet* messages) {
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (session == nullptr) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_CONTEXT_MISSING",
                  "The parser session context is unavailable.");
    return false;
  }
  if (!impl_->EnsureStarted(credentials.requested_database, messages)) return false;

  const auto connection_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  auto auth_frame = BaseConnectionFrame(
      scratchbird::server::sbps::MessageType::kAuthHandoff,
      kSchemaAuthHandoffV1,
      connection_uuid);
  auth_frame.payload = EncodeAuthPayload(credentials, connection_uuid);
  const auto auth_response = scratchbird::server::HandleAuthHandoff(
      &impl_->registry,
      impl_->engine_state,
      auth_frame);
  if (!auth_response.accepted) {
    AddServerDiagnostics(auth_response.diagnostics, messages);
    if (messages != nullptr && messages->diagnostics.empty()) {
      AddDiagnostic(messages,
                    "SECURITY.AUTHENTICATION.FAILED",
                    "embedded direct authentication was rejected by the engine");
    }
    return false;
  }

  std::size_t offset = 0;
  std::string auth_outcome;
  if (!ReadString(auth_response.payload, &offset, &auth_outcome) ||
      auth_outcome != "accepted" ||
      offset + 16 * 4 + 8 > auth_response.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.AUTH_RESULT_INVALID",
                  "The embedded authentication result payload is malformed.");
    return false;
  }
  const auto auth_context_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  offset += 16;  // auth session UUID
  offset += 16;  // principal UUID
  offset += 16;  // effective user UUID
  offset += 8;   // security epoch

  auto attach_frame = BaseConnectionFrame(
      scratchbird::server::sbps::MessageType::kAttachDatabase,
      kSchemaAttachRequestV1,
      connection_uuid);
  attach_frame.payload =
      EncodeAttachPayload(connection_uuid, auth_context_uuid, credentials.requested_database);
  const auto attached = scratchbird::server::HandleAttachDatabase(
      &impl_->registry,
      impl_->engine_state,
      attach_frame);
  if (!attached.accepted) {
    AddServerDiagnostics(attached.diagnostics, messages);
    if (messages != nullptr && messages->diagnostics.empty()) {
      AddDiagnostic(messages,
                    "PARSER_SERVER_IPC.ATTACH_DATABASE_FAILED",
                    "embedded direct database attach was rejected by the engine");
    }
    return false;
  }

  offset = 0;
  std::string attach_outcome;
  if (!ReadString(attached.payload, &offset, &attach_outcome) ||
      attach_outcome != "accepted" ||
      offset + 32 > attached.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  const auto session_uuid = GetUuid(attached.payload, offset);
  offset += 16;
  const auto user_uuid = GetUuid(attached.payload, offset);
  offset += 16;
  std::string database_path;
  std::string database_uuid;
  std::string attach_mode;
  if (!ReadString(attached.payload, &offset, &database_path) ||
      !ReadString(attached.payload, &offset, &database_uuid) ||
      !ReadString(attached.payload, &offset, &attach_mode) ||
      offset + 8 * 5 > attached.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  const auto catalog_generation = GetU64(attached.payload, offset);
  offset += 8;
  const auto attach_security_epoch = GetU64(attached.payload, offset);
  offset += 8;
  const auto policy_generation = GetU64(attached.payload, offset);
  offset += 8;
  const auto name_resolution_epoch = GetU64(attached.payload, offset);
  offset += 8;
  const auto descriptor_epoch = GetU64(attached.payload, offset);
  offset += 8;
  std::string attach_detail;
  std::string engine_health;
  if (!ReadString(attached.payload, &offset, &attach_detail) ||
      !ReadString(attached.payload, &offset, &engine_health) ||
      offset + 16 > attached.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  const auto local_transaction_id = GetU64(attached.payload, offset);
  offset += 8;
  const auto snapshot_visible_through_local_transaction_id = GetU64(attached.payload, offset);
  offset += 8;
  std::string transaction_uuid;
  std::string transaction_timestamp;
  if (!ReadString(attached.payload, &offset, &transaction_uuid) ||
      !ReadString(attached.payload, &offset, &transaction_timestamp)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  if (local_transaction_id == 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_TRANSACTION_REQUIRED",
                  "Accepted embedded attach did not publish the required active transaction.");
    return false;
  }

  session->authenticated = true;
  session->session_uuid = scratchbird::server::UuidBytesToText(session_uuid);
  session->connection_uuid = scratchbird::server::UuidBytesToText(connection_uuid);
  session->database_uuid = database_uuid;
  session->authenticated_user_uuid = scratchbird::server::UuidBytesToText(user_uuid);
  session->principal_claim = credentials.principal;
  session->auth_provider_family =
      credentials.provider_family.empty() ? "local_password" : credentials.provider_family;
  session->default_language = "en";
  session->dialect_profile_uuid = "sbsql_v3";
  session->search_path = {"sys", "public"};
  session->transaction_context = "always_active";
  session->local_transaction_id = local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = transaction_uuid;
  session->transaction_timestamp = transaction_timestamp;
  session->catalog_epoch = catalog_generation;
  session->security_policy_epoch = attach_security_epoch == 0 ? policy_generation : attach_security_epoch;
  session->descriptor_epoch = descriptor_epoch == 0 ? name_resolution_epoch : descriptor_epoch;
  return true;
#else
  (void)credentials;
  (void)session;
  AddDiagnostic(messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
  return false;
#endif
}

bool EmbeddedEngineClient::AuthenticateAndAttachSysarch(
    const AuthCredentialEnvelope& credentials,
    SessionContext* session,
    MessageVectorSet* messages) {
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (session == nullptr) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_CONTEXT_MISSING",
                  "The parser session context is unavailable.");
    return false;
  }
  if (!impl_->EnsureStarted(credentials.requested_database, messages)) return false;
  auto attached = scratchbird::server::HandleEmbeddedSysarchAttach(
      &impl_->registry,
      impl_->engine_state,
      credentials.requested_database,
      credentials.application_name.empty() ? "sb_isql" : credentials.application_name);
  if (!attached.accepted) {
    AddServerDiagnostics(attached.diagnostics, messages);
    return false;
  }

  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(attached.payload, &offset, &outcome) || outcome != "accepted" ||
      offset + 32 > attached.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  const auto session_uuid = GetUuid(attached.payload, offset);
  offset += 16;
  const auto user_uuid = GetUuid(attached.payload, offset);
  offset += 16;
  std::string database_path;
  std::string database_uuid;
  std::string attach_mode;
  if (!ReadString(attached.payload, &offset, &database_path) ||
      !ReadString(attached.payload, &offset, &database_uuid) ||
      !ReadString(attached.payload, &offset, &attach_mode) ||
      offset + 8 * 5 > attached.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  const auto catalog_generation = GetU64(attached.payload, offset);
  offset += 8;
  const auto attach_security_epoch = GetU64(attached.payload, offset);
  offset += 8;
  const auto policy_generation = GetU64(attached.payload, offset);
  offset += 8;
  const auto name_resolution_epoch = GetU64(attached.payload, offset);
  offset += 8;
  const auto descriptor_epoch = GetU64(attached.payload, offset);
  offset += 8;
  std::string attach_detail;
  std::string engine_health;
  if (!ReadString(attached.payload, &offset, &attach_detail) ||
      !ReadString(attached.payload, &offset, &engine_health) ||
      offset + 16 > attached.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  const auto local_transaction_id = GetU64(attached.payload, offset);
  offset += 8;
  const auto snapshot_visible_through_local_transaction_id = GetU64(attached.payload, offset);
  offset += 8;
  std::string transaction_uuid;
  std::string transaction_timestamp;
  if (!ReadString(attached.payload, &offset, &transaction_uuid) ||
      !ReadString(attached.payload, &offset, &transaction_timestamp)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID",
                  "The embedded attach result payload is malformed.");
    return false;
  }
  if (local_transaction_id == 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_TRANSACTION_REQUIRED",
                  "Accepted embedded attach did not publish the required active transaction.");
    return false;
  }

  const auto found = impl_->registry.sessions_by_uuid.find(
      scratchbird::server::UuidBytesToText(session_uuid));
  session->authenticated = true;
  session->session_uuid = scratchbird::server::UuidBytesToText(session_uuid);
  session->connection_uuid = found == impl_->registry.sessions_by_uuid.end()
                                 ? session->session_uuid
                                 : scratchbird::server::UuidBytesToText(found->second.connection_uuid);
  session->database_uuid = database_uuid;
  session->authenticated_user_uuid = scratchbird::server::UuidBytesToText(user_uuid);
  session->principal_claim = "sysarch";
  session->auth_provider_family = "embedded_sysarch";
  session->default_language = "en";
  session->dialect_profile_uuid = "sbsql_v3";
  session->search_path = {"sys", "public"};
  session->transaction_context = "always_active";
  session->local_transaction_id = local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = transaction_uuid;
  session->transaction_timestamp = transaction_timestamp;
  session->catalog_epoch = catalog_generation;
  session->security_policy_epoch = attach_security_epoch == 0 ? policy_generation : attach_security_epoch;
  session->descriptor_epoch = descriptor_epoch == 0 ? name_resolution_epoch : descriptor_epoch;
  return true;
#else
  (void)credentials;
  (void)session;
  AddDiagnostic(messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
  return false;
#endif
}

PublicNameResolutionResult EmbeddedEngineClient::ResolveNamePublic(
    const SessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserConfig& config) {
  PublicNameResolutionResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED",
        "ERROR",
        "public name resolution requires an embedded session",
        "sbp_sbsql.embedded"));
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kResolveNameRequest),
                         session);
  frame.payload = EncodeResolveNamePayload(session, presented_name, quoted, object_class, config);
  const auto encoded = scratchbird::server::ResolveNamePublicFrameForEmbedded(
      frame, impl_->engine_state, &impl_->registry);
  const auto decoded = scratchbird::server::sbps::DecodeFrameBytes(
      encoded, static_cast<std::uint32_t>(64u * 1024u * 1024u));
  if (!decoded.ok()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The embedded public name response frame is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  return DecodePublicNamePayload(decoded.frame->payload, "resolved");
#else
  (void)session;
  (void)presented_name;
  (void)quoted;
  (void)object_class;
  (void)config;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.UNAVAILABLE",
      "ERROR",
      "embedded engine support is not linked into this SBsql parser build",
      "sbp_sbsql.embedded"));
  return result;
#endif
}

PublicNameResolutionResult EmbeddedEngineClient::RenderUuidPublic(
    const SessionContext& session,
    std::string_view object_uuid) {
  PublicNameResolutionResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kRenderUuidRequest),
                         session);
  frame.payload = EncodeRenderUuidPayload(object_uuid);
  const auto encoded =
      scratchbird::server::RenderUuidPublicFrameForEmbedded(frame, &impl_->registry);
  const auto decoded = scratchbird::server::sbps::DecodeFrameBytes(
      encoded, static_cast<std::uint32_t>(64u * 1024u * 1024u));
  if (!decoded.ok()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The embedded UUID-rendering response frame is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  return DecodePublicNamePayload(decoded.frame->payload, "rendered");
#else
  (void)session;
  (void)object_uuid;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.UNAVAILABLE",
      "ERROR",
      "embedded engine support is not linked into this SBsql parser build",
      "sbp_sbsql.embedded"));
  return result;
#endif
}

ServerExecutionResult EmbeddedEngineClient::ExecuteSblr(
    const SessionContext& session,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested) {
  ServerExecutionResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kExecuteSblr),
                         session);
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      TextToUuid(session.session_uuid), {}, std::string(encoded_sblr_envelope), cursor_requested);
  return DecodeExecutePayload(
      scratchbird::server::HandleExecuteSblr(&impl_->registry, impl_->engine_state, frame));
#else
  (void)session;
  (void)encoded_sblr_envelope;
  (void)cursor_requested;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.UNAVAILABLE",
      "ERROR",
      "embedded engine support is not linked into this SBsql parser build",
      "sbp_sbsql.embedded"));
  return result;
#endif
}

ServerFetchResult EmbeddedEngineClient::FetchCursor(const SessionContext& session,
                                                    std::string_view cursor_uuid,
                                                    std::uint64_t max_rows,
                                                    std::uint64_t max_bytes,
                                                    std::uint32_t fetch_flags) {
  ServerFetchResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kFetch),
                         session);
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      TextToUuid(session.session_uuid), TextToUuid(cursor_uuid), max_rows, max_bytes, fetch_flags);
  auto operation = scratchbird::server::HandleFetch(&impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.payload.size() < 16 + 8) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.FETCH_RESULT_INVALID",
        "ERROR",
        "The embedded fetch result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  std::size_t offset = 0;
  result.cursor_uuid = scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
  offset += 16;
  result.row_count = GetU64(operation.payload, offset);
  offset += 8;
  if (!ReadString(operation.payload, &offset, &result.row_packet) ||
      offset >= operation.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.FETCH_RESULT_INVALID",
        "ERROR",
        "The embedded fetch result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  result.end_of_cursor = operation.payload[offset++] != 0;
  if (!ReadString(operation.payload, &offset, &result.detail)) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.FETCH_RESULT_INVALID",
        "ERROR",
        "The embedded fetch result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  result.accepted = true;
  return result;
#else
  (void)session;
  (void)cursor_uuid;
  (void)max_rows;
  (void)max_bytes;
  (void)fetch_flags;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.UNAVAILABLE",
      "ERROR",
      "embedded engine support is not linked into this SBsql parser build",
      "sbp_sbsql.embedded"));
  return result;
#endif
}

ServerCloseCursorResult EmbeddedEngineClient::CloseCursor(const SessionContext& session,
                                                          std::string_view cursor_uuid) {
  ServerCloseCursorResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kCloseCursor),
                         session);
  frame.payload = scratchbird::server::EncodeCloseCursorPayloadForTest(
      TextToUuid(session.session_uuid), TextToUuid(cursor_uuid));
  auto operation = scratchbird::server::HandleCloseCursor(&impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(operation.payload, &offset, &outcome) || offset + 16 > operation.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.CLOSE_CURSOR_RESULT_INVALID",
        "ERROR",
        "The embedded close-cursor result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  result.accepted = outcome == "accepted";
  result.cursor_uuid = scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
  offset += 16;
  (void)ReadString(operation.payload, &offset, &result.detail);
  return result;
#else
  (void)session;
  (void)cursor_uuid;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.UNAVAILABLE",
      "ERROR",
      "embedded engine support is not linked into this SBsql parser build",
      "sbp_sbsql.embedded"));
  return result;
#endif
}

ServerCloseCursorResult EmbeddedEngineClient::CancelCursor(const SessionContext& session,
                                                           std::string_view cursor_uuid) {
  ServerCloseCursorResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kCloseCursor),
                         session);
  frame.payload = scratchbird::server::EncodeCancelCursorPayloadForTest(
      TextToUuid(session.session_uuid), TextToUuid(cursor_uuid));
  auto operation = scratchbird::server::HandleCloseCursor(&impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(operation.payload, &offset, &outcome) || offset + 16 > operation.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.CLOSE_CURSOR_RESULT_INVALID",
        "ERROR",
        "The embedded cancel-cursor result payload is malformed.",
        "sbp_sbsql.embedded"));
    return result;
  }
  result.accepted = outcome == "accepted";
  result.cursor_uuid = scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
  offset += 16;
  (void)ReadString(operation.payload, &offset, &result.detail);
  return result;
#else
  (void)session;
  (void)cursor_uuid;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.UNAVAILABLE",
      "ERROR",
      "embedded engine support is not linked into this SBsql parser build",
      "sbp_sbsql.embedded"));
  return result;
#endif
}

ServerManagementResult EmbeddedEngineClient::Manage(const SessionContext& session,
                                                    std::string_view operation_key,
                                                    std::string_view target_uuid,
                                                    std::string_view mode,
                                                    std::string_view audit_reason,
                                                    std::uint64_t timeout_ms,
                                                    bool include_history) {
  (void)session;
  (void)target_uuid;
  (void)mode;
  (void)audit_reason;
  (void)timeout_ms;
  (void)include_history;
  ServerManagementResult result;
  result.operation_key = std::string(operation_key);
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.EMBEDDED.SERVER_MANAGEMENT_UNAVAILABLE",
      "ERROR",
      "server lifecycle management is not available in embedded single-database mode",
      "sbp_sbsql.embedded"));
  return result;
}

bool EmbeddedEngineClient::DisconnectSession(const SessionContext& session,
                                             MessageVectorSet* messages) {
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || session.session_uuid.empty()) return true;
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kDisconnectNotice),
                         session);
  auto operation = scratchbird::server::HandleDisconnectNotice(&impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, messages);
    return false;
  }
  return true;
#else
  (void)session;
  (void)messages;
  return true;
#endif
}

}  // namespace scratchbird::parser::sbsql
