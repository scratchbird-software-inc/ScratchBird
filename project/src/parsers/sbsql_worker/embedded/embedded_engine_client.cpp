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
#include <limits>
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

std::string NormalizeLanguageTag(std::string_view value) {
  return value.empty() ? "en" : std::string(value);
}

std::string LanguageProfileForTag(std::string_view value) {
  const std::string tag = NormalizeLanguageTag(value);
  if (tag == "en") return "sbsql.builtin.recovery.en";
  return "sbsql.language-profile." + tag;
}

std::string InputFallbackTagForTag(std::string_view value) {
  const std::string tag = NormalizeLanguageTag(value);
  return tag == "en" ? std::string{} : "en";
}

void ApplyEmbeddedLanguageContext(SessionContext* session,
                                  std::string_view requested_language_tag,
                                  std::uint64_t language_resource_epoch,
                                  std::uint64_t localized_name_epoch) {
  if (session == nullptr) return;
  session->default_language = "en";
  session->language_tag = NormalizeLanguageTag(requested_language_tag);
  session->language_profile = LanguageProfileForTag(session->language_tag);
  session->input_syntax_profile = "sbsql.syntax.standard";
  session->input_language_fallback_tag =
      InputFallbackTagForTag(session->language_tag);
  session->common_resource_hash = "builtin.common.sbsql.v1";
  session->resource_compatibility_identity = "sbsql.resource.compat.v1";
  session->resource_version_identity = "sbsql.resource-pack.v1";
  session->language_resource_epoch = language_resource_epoch;
  session->localized_name_epoch = localized_name_epoch;
  if (session->message_resource_epoch == 0) session->message_resource_epoch = 1;
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

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out->push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
  }
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
  const auto affected_rows = line_value("server_affected_rows");
  if (affected_rows) {
    result->affected_rows = parse_u64(*affected_rows);
    result->affected_rows_present = true;
  }
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

constexpr std::uint32_t kExecuteSblrV1PayloadSchema = 4003;

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
  std::string detail;
  if (!ReadString(operation.payload, &offset, &detail) ||
      offset >= operation.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "ERROR",
        "The embedded execute cursor descriptor trailer is absent.",
        "sbp_sbsql.embedded"));
    return result;
  }
  const bool cursor_present =
      result.cursor_uuid != "00000000-0000-0000-0000-000000000000";
  const std::uint8_t descriptor_present = operation.payload[offset++];
  if (descriptor_present != (cursor_present ? 1 : 0)) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SERVER.STREAM.DESCRIPTOR_INVALID", "ERROR",
        "The embedded cursor result descriptor presence is inconsistent.",
        "sbp_sbsql.embedded"));
    return result;
  }
  if (descriptor_present != 0) {
    if (offset + 16 + 2 + 8 + 16 * 5 + 8 + 8 != operation.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SERVER.STREAM.DESCRIPTOR_INVALID", "ERROR",
          "The embedded cursor stream descriptor is malformed.",
          "sbp_sbsql.embedded"));
      return result;
    }
    auto& descriptor = result.cursor_stream_descriptor;
    descriptor.present = true;
    descriptor.stream_descriptor_uuid =
        scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
    offset += 16;
    descriptor.descriptor_version = GetU16(operation.payload, offset);
    offset += 2;
    descriptor.descriptor_generation = GetU64(operation.payload, offset);
    offset += 8;
    descriptor.cursor_uuid =
        scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
    offset += 16;
    descriptor.execution_uuid =
        scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
    offset += 16;
    descriptor.result_set_uuid =
        scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
    offset += 16;
    descriptor.row_descriptor_uuid =
        scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
    offset += 16;
    descriptor.snapshot_uuid =
        scratchbird::server::UuidBytesToText(GetUuid(operation.payload, offset));
    offset += 16;
    descriptor.max_chunk_rows = GetU64(operation.payload, offset);
    offset += 8;
    descriptor.max_chunk_bytes = GetU64(operation.payload, offset);
    offset += 8;
    if (!descriptor.complete() || descriptor.cursor_uuid != result.cursor_uuid) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SERVER.STREAM.DESCRIPTOR_INVALID", "ERROR",
          "The embedded cursor stream descriptor does not bind the cursor.",
          "sbp_sbsql.embedded"));
      return result;
    }
  }
  if (offset != operation.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "ERROR",
        "The embedded execute result contains trailing bytes.",
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
  std::array<std::uint8_t, 16> parser_package_uuid{};
  std::array<std::uint8_t, 16> dialect_profile_uuid{};

  void EnsureEmbeddedParserIdentity() {
    if (parser_package_uuid == std::array<std::uint8_t, 16>{}) {
      parser_package_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
    }
    if (dialect_profile_uuid == std::array<std::uint8_t, 16>{}) {
      dialect_profile_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
    }
  }

  void AdmitEmbeddedParserIdentity(
      const std::array<std::uint8_t, 16>& connection_uuid) {
    EnsureEmbeddedParserIdentity();
    scratchbird::server::ServerAdmittedParserChannelIdentity identity;
    identity.parser_package_uuid = parser_package_uuid;
    identity.dialect_profile_uuid = dialect_profile_uuid;
    identity.parser_package_version_major =
        config.parser_api_major == 0 ? kSbsqlWorkerParserApiCurrentMajor
                                     : config.parser_api_major;
    identity.parser_package_version_minor = 0;
    identity.parser_package_version_patch = 0;
    registry.admitted_parser_identity_by_connection_uuid.insert_or_assign(
        scratchbird::server::UuidBytesToText(connection_uuid), identity);
  }

  bool PublishCanonicalNativeSessionIdentity(SessionContext* session) {
    if (session == nullptr || session->session_uuid.empty()) return false;
    const auto found = registry.sessions_by_uuid.find(session->session_uuid);
    if (found == registry.sessions_by_uuid.end()) return false;
    EnsureEmbeddedParserIdentity();
    if (found->second.admitted_parser_package_uuid ==
        std::array<std::uint8_t, 16>{}) {
      found->second.admitted_parser_package_uuid = parser_package_uuid;
      found->second.admitted_dialect_profile_uuid = dialect_profile_uuid;
      found->second.admitted_parser_package_version_major =
          config.parser_api_major == 0 ? kSbsqlWorkerParserApiCurrentMajor
                                       : config.parser_api_major;
      found->second.admitted_parser_package_version_minor = 0;
      found->second.admitted_parser_package_version_patch = 0;
    }
    // Embedded direct execution does not negotiate a physical SBPS socket,
    // but it invokes the same authenticated handlers and exact schemas.  Mark
    // only the capabilities actually provided by this in-process route.
    found->second.transaction_routing_v2_negotiated = true;
    found->second.relation_descriptor_projection_v3_negotiated = true;
    session->admitted_parser_package_uuid = scratchbird::server::UuidBytesToText(
        found->second.admitted_parser_package_uuid);
    session->admitted_dialect_profile_uuid = scratchbird::server::UuidBytesToText(
        found->second.admitted_dialect_profile_uuid);
    session->admitted_parser_package_version_major =
        found->second.admitted_parser_package_version_major;
    session->admitted_parser_package_version_minor =
        found->second.admitted_parser_package_version_minor;
    session->admitted_parser_package_version_patch =
        found->second.admitted_parser_package_version_patch;
    session->transaction_routing_v2_negotiated = true;
    session->relation_descriptor_projection_v3_negotiated = true;
    return true;
  }

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
    // Database creation is exclusively owned by the approved embedded first-
    // principal bootstrap path shared by SBsql and SBsec. An embedded client
    // may open an existing database, but it must never turn a connection
    // attempt into create-time publication.
    server_config.database_auto_create = false;
    server_config.allow_uncredentialed_fixture_database =
        config.allow_uncredentialed_fixture_database;
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
  impl_->AdmitEmbeddedParserIdentity(connection_uuid);
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
  ApplyEmbeddedLanguageContext(session,
                               credentials.requested_language,
                               descriptor_epoch == 0 ? name_resolution_epoch
                                                     : descriptor_epoch,
                               name_resolution_epoch);
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
  if (!impl_->PublishCanonicalNativeSessionIdentity(session)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "embedded native parser admission identity was not bound to the authenticated session");
    session->authenticated = false;
    return false;
  }
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
  ApplyEmbeddedLanguageContext(session,
                               credentials.requested_language,
                               descriptor_epoch == 0 ? name_resolution_epoch
                                                     : descriptor_epoch,
                               name_resolution_epoch);
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
  if (!impl_->PublishCanonicalNativeSessionIdentity(session)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "embedded native parser admission identity was not bound to the authenticated session");
    session->authenticated = false;
    return false;
  }
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
  const bool require_relation_descriptor =
      object_class == "relation" || object_class == "table";
  ipc::ParserTransactionSelector transaction;
  transaction.local_transaction_id = session.local_transaction_id;
  transaction.transaction_uuid = session.transaction_uuid;
  if (require_relation_descriptor &&
      (!session.transaction_routing_v2_negotiated ||
       !session.relation_descriptor_projection_v3_negotiated ||
       !transaction.present())) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_NOT_NEGOTIATED",
        "ERROR",
        "Embedded relation resolution requires the negotiated transaction-bound V3 projection route.",
        "sbp_sbsql.embedded"));
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kResolveNameRequest),
                         session);
  frame.header.payload_schema_id = require_relation_descriptor
      ? scratchbird::server::sbps::kSchemaResolveNameRequestV3
      : scratchbird::server::sbps::kSchemaResolveNameRequestV1;
  frame.payload = require_relation_descriptor
      ? ipc::EncodeResolveNameRequestPayloadV3(
            session, presented_name, quoted, object_class, config, transaction,
            0x01u)
      : EncodeResolveNamePayload(session, presented_name, quoted, object_class,
                                 config);
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
  if (require_relation_descriptor) {
    ipc::PublicNameResolutionResult projected;
    ipc::DecodeResolveNameResultPayloadV3(decoded.frame->payload, true,
                                          &projected);
    return projected;
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

std::vector<PublicNameResolutionResult>
EmbeddedEngineClient::ResolveRelationDescriptorsPublic(
    const SessionContext& session,
    const std::vector<ipc::PublicRelationResolutionRequest>& requests,
    const ParserConfig& config) {
  std::vector<PublicNameResolutionResult> results;
  results.reserve(requests.size());
  // Each request still crosses the exact production V3 frame codec and server
  // admission path. The engine-side statement metadata view is immutable and
  // generation-keyed, so all members borrow one validated transaction/MGA
  // cohort without introducing parser-owned descriptor authority.
  for (const auto& request : requests) {
    results.push_back(ResolveNamePublic(session,
                                        request.presented_name,
                                        request.quoted,
                                        request.object_class,
                                        config));
  }
  return results;
}

PublicNameResolutionResult EmbeddedEngineClient::RenderUuidPublic(
    const SessionContext& session,
    std::string_view object_uuid) {
  PublicNameResolutionResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kRenderUuidRequest),
                         session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::kSchemaRenderUuidRequestV1;
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

ipc::ServerStatementContextResult
EmbeddedEngineClient::AcquireNativeStatementContext(
    const SessionContext& session,
    const ipc::ParserTransactionSelector& transaction) {
  ipc::ServerStatementContextResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || !session.transaction_routing_v2_negotiated ||
      !session.relation_descriptor_projection_v3_negotiated ||
      !transaction.present() ||
      TextToUuid(session.session_uuid) == std::array<std::uint8_t, 16>{} ||
      TextToUuid(session.connection_uuid) == std::array<std::uint8_t, 16>{} ||
      TextToUuid(transaction.transaction_uuid) ==
          std::array<std::uint8_t, 16>{}) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_IDENTITY_INVALID",
                  "embedded native statement-context acquisition requires "
                  "an authenticated exact transaction route");
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::
                                 kAcquireStatementContextRequest),
                         session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::kSchemaAcquireStatementContextRequestV11;
  frame.payload = ipc::EncodeNativeStatementContextRequestPayloadV11(
      session, transaction);
  const auto operation = scratchbird::server::HandleAcquireStatementContext(
      &impl_->registry, impl_->engine_state, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_schema_id !=
          scratchbird::server::sbps::
              kSchemaAcquireStatementContextResultV11 ||
      !ipc::DecodeNativeStatementContextResultPayloadV11(
          operation.payload, &result.context) ||
      result.context.transaction.local_transaction_id !=
          transaction.local_transaction_id ||
      result.context.transaction.transaction_uuid !=
          transaction.transaction_uuid) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
                  "the embedded engine-issued native statement context was "
                  "malformed or did not match the requested transaction");
    result.context = {};
    return result;
  }
  result.accepted = true;
#else
  (void)session;
  (void)transaction;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerLiteralBindingResult
EmbeddedEngineClient::NegotiateLiteralDescriptors(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbln) {
  ipc::ServerLiteralBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_sbln.size() < 128) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "the embedded literal descriptor request is malformed");
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::
                                 kNegotiateLiteralDescriptorsRequest),
                         session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::
          kSchemaNegotiateLiteralDescriptorsRequestV1;
  frame.payload = canonical_sbln;
  const auto operation = scratchbird::server::HandleNegotiateLiteralDescriptors(
      &impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_schema_id !=
      scratchbird::server::sbps::
          kSchemaNegotiateLiteralDescriptorsResultV1) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.LITERAL_RESULT_SCHEMA_MISMATCH",
                  "the embedded literal negotiation result schema is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_sbln;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerLiteralBindingResult
EmbeddedEngineClient::IssueContextualTextLiteralProfiles(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbtlnr) {
  ipc::ServerLiteralBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_sbtlnr.size() < 410 ||
      canonical_sbtlnr.size() > 65536) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "the embedded contextual TEXT literal profile request is malformed");
    return result;
  }
  auto frame = BaseFrame(698, session);
  frame.header.stream_id = 1;
  frame.header.payload_schema_id = 7711;
  frame.payload = canonical_sbtlnr;
  const auto operation =
      scratchbird::server::HandleIssueContextualTextLiteralProfiles(
          &impl_->registry, impl_->engine_state, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_message_type != 699 ||
      operation.response_schema_id != 7712) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.LITERAL_RESULT_SCHEMA_MISMATCH",
                  "the embedded contextual TEXT literal profile result is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_sbtlnr;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerLiteralBindingResult EmbeddedEngineClient::FinalizeLiteralBinding(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_sblf) {
  ipc::ServerLiteralBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_sblf.size() < 208) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "the embedded literal finalization request is malformed");
    return result;
  }
  auto frame = BaseFrame(40, session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::kSchemaFinalizeLiteralBindingRequestV1;
  frame.payload = canonical_sblf;
  const auto operation = scratchbird::server::HandleFinalizeLiteralBinding(
      &impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_schema_id !=
      scratchbird::server::sbps::kSchemaFinalizeLiteralBindingResultV1) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.LITERAL_RESULT_SCHEMA_MISMATCH",
                  "the embedded literal finalization result schema is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_sblf;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerParameterBindingResult
EmbeddedEngineClient::NegotiateParameterDescriptors(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbpr) {
  ipc::ServerParameterBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_sbpr.size() < 136 ||
      canonical_sbpr.size() > 98416) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "the embedded parameter descriptor request is malformed");
    return result;
  }
  auto frame = BaseFrame(42, session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::
          kSchemaNegotiateParameterDescriptorsRequestV1;
  frame.payload = canonical_sbpr;
  const auto operation =
      scratchbird::server::HandleNegotiateParameterDescriptors(
          &impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_message_type != 43 ||
      operation.response_schema_id !=
          scratchbird::server::sbps::
              kSchemaNegotiateParameterDescriptorsResultV1) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PARAMETER_RESULT_SCHEMA_MISMATCH",
                  "the embedded parameter negotiation result is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_sbpr;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerParameterBindingResult
EmbeddedEngineClient::FinalizeParameterBinding(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbpf) {
  ipc::ServerParameterBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_sbpf.size() < 280 ||
      canonical_sbpf.size() > 426192) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "the embedded parameter finalization request is malformed");
    return result;
  }
  auto frame = BaseFrame(44, session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::kSchemaFinalizeParameterBindingRequestV1;
  frame.payload = canonical_sbpf;
  const auto operation = scratchbird::server::HandleFinalizeParameterBinding(
      &impl_->registry, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_message_type != 45 ||
      operation.response_schema_id !=
          scratchbird::server::sbps::kSchemaFinalizeParameterBindingResultV1) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PARAMETER_RESULT_SCHEMA_MISMATCH",
                  "the embedded parameter finalization result is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_sbpf;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerBulkImportBindResult EmbeddedEngineClient::BindBulkImportStream(
    const SessionContext& session,
    const scratchbird::wire::sbps_bulk_import::Bind& bind) {
  ipc::ServerBulkImportBindResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated) {
    AddDiagnostic(&result.messages, "SECURITY.ACCESS_DENIED",
                  "embedded bulk import bind requires authentication");
    return result;
  }
  std::vector<std::uint8_t> payload;
  std::string detail;
  if (!scratchbird::wire::sbps_bulk_import::EncodeBind(bind, &payload, &detail)) {
    AddDiagnostic(&result.messages, "SBLR.OPERAND_INVALID",
                  detail.empty() ? "embedded bulk import bind is malformed" : detail);
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kBulkImportStreamBind), session);
  frame.header.payload_schema_id = scratchbird::server::sbps::kSchemaBulkImportStreamBindV1;
  frame.payload = std::move(payload);
  const auto operation = scratchbird::server::HandleBindBulkImportStream(
      &impl_->registry, impl_->engine_state, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_message_type !=
          static_cast<std::uint16_t>(
              scratchbird::server::sbps::MessageType::kBulkImportStreamBindAck) ||
      operation.response_schema_id !=
          scratchbird::server::sbps::kSchemaBulkImportStreamBindAckV1) {
    result.outcome_unknown = true;
    AddDiagnostic(&result.messages, "BULK.IMPORT.RECOVERY_CONFLICT",
                  "embedded bulk import bind response is not exactly correlated");
    return result;
  }
  if (!scratchbird::wire::sbps_bulk_import::DecodeBindAck(
          operation.payload.data(), operation.payload.size(), &result.binding, &detail) ||
      result.binding.authenticated_receipt_uuid != bind.authenticated_receipt_uuid ||
      result.binding.structural_occurrence != bind.structural_occurrence ||
      result.binding.import_occurrence != bind.import_occurrence ||
      result.binding.syntax_demand_sha256 != bind.syntax_demand_sha256) {
    result.outcome_unknown = true;
    AddDiagnostic(&result.messages, "BULK.IMPORT.RECOVERY_CONFLICT",
                  detail.empty() ? "embedded bulk import bind acknowledgement is invalid" : detail);
    return result;
  }
  result.accepted = true;
#else
  (void)session; (void)bind;
  AddDiagnostic(&result.messages, "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerBulkImportChunkResult EmbeddedEngineClient::AppendBulkImportStream(const SessionContext& session, const scratchbird::wire::sbps_bulk_import::Chunk& chunk) {
  ipc::ServerBulkImportChunkResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  std::vector<std::uint8_t> payload;
  std::string detail;
  if (!session.authenticated || !scratchbird::wire::sbps_bulk_import::EncodeChunk(chunk, &payload, &detail)) { AddDiagnostic(&result.messages, "SBLR.OPERAND_INVALID", detail.empty() ? "bulk import chunk is malformed" : detail); return result; }
  auto frame = BaseFrame(static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kBulkImportStreamChunk), session); frame.header.payload_schema_id = scratchbird::server::sbps::kSchemaBulkImportStreamChunkV1; frame.payload = std::move(payload);
  const auto operation = scratchbird::server::HandleAppendBulkImportStream(&impl_->registry, impl_->engine_state, frame);
  if (!operation.accepted) { AddServerDiagnostics(operation.diagnostics, &result.messages); return result; }
  if (operation.response_message_type != static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kBulkImportStreamChunkAck) || operation.response_schema_id != scratchbird::server::sbps::kSchemaBulkImportStreamChunkAckV1) { result.outcome_unknown = true; AddDiagnostic(&result.messages, "BULK.IMPORT.RECOVERY_CONFLICT", "embedded bulk import chunk response is not exactly correlated"); return result; }
  const bool extent_valid =
      chunk.byte_offset <= std::numeric_limits<std::uint64_t>::max() -
                               chunk.chunk_payload.size();
  if (!scratchbird::wire::sbps_bulk_import::DecodeChunkAck(operation.payload.data(), operation.payload.size(), &result.acknowledgement, &detail) || !extent_valid || result.acknowledgement.stream_uuid != chunk.stream_uuid || result.acknowledgement.stream_generation != chunk.stream_generation || result.acknowledgement.accepted_sequence != chunk.chunk_sequence || result.acknowledgement.accepted_total_bytes != chunk.byte_offset + chunk.chunk_payload.size() || result.acknowledgement.accepted_chain_sha256 != chunk.chunk_chain_sha256) { result.outcome_unknown = true; AddDiagnostic(&result.messages, "BULK.IMPORT.RECOVERY_CONFLICT", detail.empty() ? "bulk import chunk acknowledgement is not exactly correlated" : detail); return result; }
  result.accepted = true;
#else
  (void)session; (void)chunk; AddDiagnostic(&result.messages, "SBSQL.EMBEDDED.UNAVAILABLE", "embedded engine support is unavailable");
#endif
  return result;
}

ipc::ServerBulkImportSealResult EmbeddedEngineClient::SealBulkImportStream(const SessionContext& session, const scratchbird::wire::sbps_bulk_import::Seal& seal) {
  ipc::ServerBulkImportSealResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  std::vector<std::uint8_t> payload; std::string detail;
  if (!session.authenticated || !scratchbird::wire::sbps_bulk_import::EncodeSeal(seal, &payload, &detail)) { AddDiagnostic(&result.messages, "SBLR.OPERAND_INVALID", detail.empty() ? "bulk import seal is malformed" : detail); return result; }
  auto frame = BaseFrame(static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kBulkImportStreamSeal), session); frame.header.payload_schema_id = scratchbird::server::sbps::kSchemaBulkImportStreamSealV1; frame.payload = std::move(payload);
  const auto operation = scratchbird::server::HandleSealBulkImportStream(&impl_->registry, impl_->engine_state, frame);
  if (!operation.accepted) { AddServerDiagnostics(operation.diagnostics, &result.messages); return result; }
  if (operation.response_message_type != static_cast<std::uint16_t>(scratchbird::server::sbps::MessageType::kBulkImportStreamSealAck) || operation.response_schema_id != scratchbird::server::sbps::kSchemaBulkImportStreamSealAckV1) { result.outcome_unknown = true; AddDiagnostic(&result.messages, "BULK.IMPORT.RECOVERY_CONFLICT", "embedded bulk import seal response is not exactly correlated"); return result; }
  if (!scratchbird::wire::sbps_bulk_import::DecodeSealAck(operation.payload.data(), operation.payload.size(), &result.acknowledgement, &detail) || result.acknowledgement.stream_uuid != seal.stream_uuid || result.acknowledgement.stream_generation != seal.stream_generation || result.acknowledgement.chunk_count != seal.final_chunk_count || result.acknowledgement.total_stream_bytes != seal.total_stream_bytes || result.acknowledgement.final_chain_sha256 != seal.final_chain_sha256 || result.acknowledgement.content_sha256 != seal.content_sha256) { result.outcome_unknown = true; AddDiagnostic(&result.messages, "BULK.IMPORT.RECOVERY_CONFLICT", detail.empty() ? "bulk import seal acknowledgement is not exactly correlated" : detail); return result; }
  result.accepted = true;
#else
  (void)session; (void)seal; AddDiagnostic(&result.messages, "SBSQL.EMBEDDED.UNAVAILABLE", "embedded engine support is unavailable");
#endif
  return result;
}

ipc::ServerVariableBindingResult EmbeddedEngineClient::CoordinateBulkImportStream(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_request) {
  ipc::ServerVariableBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_request.size() != 64) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "the embedded bulk import stream request is malformed");
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::
                                 kCoordinateBulkImportStreamRequest),
                         session);
  frame.header.payload_schema_id =
      scratchbird::server::sbps::kSchemaCoordinateBulkImportStreamRequestV1;
  frame.payload = canonical_request;
  const auto operation = scratchbird::server::HandleCoordinateBulkImportStream(
      &impl_->registry, impl_->engine_state, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_schema_id !=
          scratchbird::server::sbps::kSchemaCoordinateBulkImportStreamResultV1 ||
      operation.payload.size() != 424) {
    result.outcome_unknown = true;
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.BULK_IMPORT_STREAM_RESULT_SCHEMA_MISMATCH",
                  "the embedded bulk import stream result is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_request;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerVariableBindingResult
EmbeddedEngineClient::CoordinateDmlUpdateRowsBind(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_request) {
  ipc::ServerVariableBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || canonical_request.size() < 52 ||
      canonical_request.size() > 65536) {
    AddDiagnostic(&result.messages, "SBLR.OPERAND_INVALID",
                  "the embedded DML UPDATE binding request is malformed");
    return result;
  }
  auto frame = BaseFrame(
      static_cast<std::uint16_t>(
          scratchbird::server::sbps::MessageType::
              kCoordinateDmlUpdateRowsBindRequest),
      session);
  frame.header.payload_schema_id = scratchbird::server::sbps::
      kSchemaCoordinateDmlUpdateRowsBindRequestV1;
  frame.payload = canonical_request;
  const auto operation = scratchbird::server::
      HandleCoordinateDmlUpdateRowsBind(&impl_->registry,
                                        impl_->engine_state, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_schema_id != scratchbird::server::sbps::
          kSchemaCoordinateDmlUpdateRowsBindResultV1 ||
      operation.payload.size() != 24) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.DML_UPDATE_BIND_RESULT_SCHEMA_MISMATCH",
        "the embedded DML UPDATE binding result is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_request;
  AddDiagnostic(&result.messages, "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ipc::ServerVariableBindingResult
EmbeddedEngineClient::CoordinateDmlPlanImportRowsBind(
    const SessionContext& session,
    const std::vector<std::uint8_t>& canonical_request) {
  ipc::ServerVariableBindingResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  constexpr std::size_t kRequestHeaderBytes = 120;
  constexpr std::size_t kMappingRecordBytes = 24;
  constexpr std::size_t kMaximumMappings = 262144;
  constexpr std::size_t kMaximumRequestBytes =
      kRequestHeaderBytes + kMaximumMappings * kMappingRecordBytes;
  if (!session.authenticated ||
      canonical_request.size() < kRequestHeaderBytes ||
      canonical_request.size() > kMaximumRequestBytes) {
    AddDiagnostic(&result.messages, "SBLR.OPERAND_INVALID",
                  "the embedded DML plan-import binding request is malformed");
    return result;
  }
  auto frame = BaseFrame(
      static_cast<std::uint16_t>(
          scratchbird::server::sbps::MessageType::
              kCoordinateDmlPlanImportRowsBindRequest),
      session);
  frame.header.payload_schema_id = scratchbird::server::sbps::
      kSchemaCoordinateDmlPlanImportRowsBindRequestV1;
  frame.payload = canonical_request;
  const auto operation = scratchbird::server::
      HandleCoordinateDmlPlanImportRowsBind(&impl_->registry,
                                            impl_->engine_state, frame);
  if (!operation.accepted) {
    AddServerDiagnostics(operation.diagnostics, &result.messages);
    return result;
  }
  if (operation.response_message_type != static_cast<std::uint16_t>(
          scratchbird::server::sbps::MessageType::
              kCoordinateDmlPlanImportRowsBindResult) ||
      operation.response_schema_id != scratchbird::server::sbps::
          kSchemaCoordinateDmlPlanImportRowsBindResultV1 ||
      operation.payload.size() != 24) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.DML_PLAN_IMPORT_ROWS_BIND_RESULT_SCHEMA_MISMATCH",
        "the embedded DML plan-import binding result is invalid");
    return result;
  }
  result.accepted = true;
  result.canonical_payload = operation.payload;
#else
  (void)session;
  (void)canonical_request;
  AddDiagnostic(&result.messages, "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ServerExecutionResult
EmbeddedEngineClient::ExecuteCanonicalSblrWithDataPacket(
    const SessionContext& session,
    const ipc::ParserStatementContext& statement_context,
    const ipc::ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  ServerExecutionResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!session.authenticated || !session.transaction_routing_v2_negotiated ||
      !statement_context.complete() || !submission.complete() ||
      submission.statement_uuid != statement_context.statement_uuid) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.CANONICAL_STATEMENT_CONTEXT_INVALID",
                  "embedded canonical execution requires the exact acquired "
                  "statement and active transaction context");
    return result;
  }
  std::uint32_t request_schema_id = 0;
  auto payload = ipc::EncodeCanonicalExecuteRequestPayload(
      session, statement_context, submission, data_packet, cursor_requested,
      &request_schema_id);
  if (payload.empty() || request_schema_id == 0) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.CANONICAL_STATEMENT_CONTEXT_INVALID",
                  "the embedded canonical execute request could not be encoded");
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::
                                 kExecuteSblr),
                         session);
  frame.header.payload_schema_id = request_schema_id;
  frame.payload = std::move(payload);
  const auto operation = scratchbird::server::HandleExecuteSblr(
      &impl_->registry, impl_->engine_state, frame);
  if (!ipc::DecodeCanonicalExecuteResultPayload(
          operation.payload, frame.header.request_uuid, &result,
          &result.messages)) {
    if (!operation.diagnostics.empty()) {
      AddServerDiagnostics(operation.diagnostics, &result.messages);
    }
    if (result.messages.diagnostics.empty()) {
      AddDiagnostic(&result.messages,
                    "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                    "the embedded canonical execute result payload is malformed");
    }
    result.accepted = false;
  }
#else
  (void)session;
  (void)statement_context;
  (void)submission;
  (void)data_packet;
  (void)cursor_requested;
  AddDiagnostic(&result.messages,
                "SBSQL.EMBEDDED.UNAVAILABLE",
                "embedded engine support is not linked into this SBsql parser build");
#endif
  return result;
}

ServerExecutionResult EmbeddedEngineClient::ExecuteSblr(
    const SessionContext& session,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested) {
  return ExecuteSblrWithDataPacket(session, encoded_sblr_envelope, {}, cursor_requested);
}

ServerExecutionResult EmbeddedEngineClient::ExecuteSblrWithDataPacket(
    const SessionContext& session,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  ServerExecutionResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kExecuteSblr),
                         session);
  frame.header.payload_schema_id = kExecuteSblrV1PayloadSchema;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      TextToUuid(session.session_uuid),
      {},
      std::string(encoded_sblr_envelope),
      cursor_requested,
      data_packet);
  return DecodeExecutePayload(
      scratchbird::server::HandleExecuteSblr(&impl_->registry, impl_->engine_state, frame));
#else
  (void)session;
  (void)encoded_sblr_envelope;
  (void)data_packet;
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
                                                    const ipc::CursorStreamDescriptorV1& stream_descriptor,
                                                    std::uint64_t max_rows,
                                                    std::uint64_t max_bytes,
                                                    std::uint32_t fetch_flags) {
  ServerFetchResult result;
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  if (!stream_descriptor.complete() ||
      stream_descriptor.cursor_uuid != cursor_uuid || max_rows == 0 ||
      max_bytes == 0 || max_rows > stream_descriptor.max_chunk_rows ||
      max_bytes > stream_descriptor.max_chunk_bytes) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SERVER.STREAM.DESCRIPTOR_INVALID", "ERROR",
        "embedded fetch requires the exact live cursor stream descriptor",
        "sbp_sbsql.embedded"));
    return result;
  }
  auto frame = BaseFrame(static_cast<std::uint16_t>(
                             scratchbird::server::sbps::MessageType::kFetch),
                         session);
  PutUuid(&frame.payload, TextToUuid(session.session_uuid));
  PutUuid(&frame.payload, TextToUuid(cursor_uuid));
  PutU64(&frame.payload, max_rows);
  PutU64(&frame.payload, max_bytes);
  PutU32(&frame.payload, fetch_flags);
  PutUuid(&frame.payload, TextToUuid(stream_descriptor.stream_descriptor_uuid));
  PutU16(&frame.payload, stream_descriptor.descriptor_version);
  PutU64(&frame.payload, stream_descriptor.descriptor_generation);
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
