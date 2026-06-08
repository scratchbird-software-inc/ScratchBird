// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_AUTH_SESSION_ATTACH

#include "session_registry.hpp"

#include "config_policy_security_lifecycle.hpp"
#include "engine_host.hpp"
#include "security/authentication_api.hpp"
#include "security/authorization_api.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>

namespace scratchbird::server {

namespace {

namespace engine_api = scratchbird::engine::internal_api;

constexpr std::uint32_t kSchemaAuthResultTestV1 = 3002;
constexpr std::uint32_t kSchemaAttachResultTestV1 = 3004;
constexpr std::uint32_t kSchemaDisconnectResultTestV1 = 3005;

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

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  return uuid;
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

std::array<std::uint8_t, 16> PrincipalUuidFor(const std::string& principal) {
  auto uuid = sbps::MakeUuidV7Bytes();
  if (!principal.empty()) {
    for (std::size_t i = 0; i < principal.size(); ++i) {
      uuid[i % uuid.size()] ^= static_cast<std::uint8_t>(principal[i]);
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
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
  for (const char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return {};
    if ((nibble % 2) == 0) {
      out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    }
    ++nibble;
  }
  return nibble == 32 ? out : std::array<std::uint8_t, 16>{};
}

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool OperationCancellationCanBeDeterministic(std::string_view operation_id) {
  return StartsWith(operation_id, "dml.select") ||
         StartsWith(operation_id, "query.select") ||
         operation_id == "observability.show_version" ||
         operation_id == "catalog.show_version";
}

bool CancellationOutcomeUnknown(const ServerRequestRecord& request,
                                const ServerSessionRecord& actor) {
  if (request.engine_result_retained) return true;
  const bool active_transaction =
      request.local_transaction_id_at_start != 0 ||
      actor.local_transaction_id != 0;
  if (!active_transaction) return false;
  return !OperationCancellationCanBeDeterministic(request.operation_id);
}

std::string FirstOpenDatabasePath(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return database.database_path;
  }
  return {};
}

std::string FirstOpenDatabaseUuid(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return database.database_uuid;
  }
  return {};
}

const HostedDatabaseSnapshot* FirstOpenDatabase(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return &database;
  }
  return nullptr;
}

void ApplyDatabaseHealthToSession(ServerSessionRecord* session,
                                  const HostedDatabaseSnapshot& database) {
  if (session == nullptr) return;
  session->database_engine_agent_state = database.database_engine_agent_state;
  session->database_engine_agent_health_generation =
      database.database_engine_agent_health_generation;
  session->database_engine_agent_ordinary_admission_allowed =
      database.database_engine_agent_ordinary_admission_allowed;
  session->database_engine_agent_health_json = database.database_engine_agent_health_json;
  session->config_source_epoch = database.config_source_epoch;
  session->config_reload_generation = database.config_reload_generation;
  session->capability_policy_generation =
      database.capability_policy_generation == 0 ? 1 : database.capability_policy_generation;
  session->policy_generation = database.policy_generation == 0 ? 1 : database.policy_generation;
  session->security_epoch = database.security_epoch == 0 ? 1 : database.security_epoch;
  session->security_provider_generation =
      database.security_provider_generation == 0 ? 1 : database.security_provider_generation;
  session->cache_invalidation_epoch =
      database.cache_invalidation_epoch == 0 ? 1 : database.cache_invalidation_epoch;
  session->config_policy_security_lifecycle_json =
      database.config_policy_security_lifecycle_json;
}

ServerDiagnostic AuthDiagnostic(std::string code, std::string message, std::string detail = {}) {
  std::vector<ServerDiagnosticField> fields;
  if (!detail.empty()) fields.push_back({"detail", detail});
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic DetachCleanupDiagnostic(std::string code,
                                         ServerDiagnosticSeverity severity,
                                         std::string message,
                                         std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic DblcAttachAdmissionDenied(std::string phase, std::string detail) {
  std::vector<ServerDiagnosticField> fields;
  fields.push_back({"phase", std::move(phase)});
  if (!detail.empty()) fields.push_back({"detail", std::move(detail)});
  return ServerDiagnostic{"ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
                          "ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
                          ServerDiagnosticSeverity::kError,
                          "Attach/auth/session admission failed after lifecycle classification.",
                          std::move(fields)};
}

void AddAttachAdmissionDenied(std::vector<ServerDiagnostic>* diagnostics,
                              std::string phase,
                              std::string detail) {
  diagnostics->push_back(DblcAttachAdmissionDenied(std::move(phase), std::move(detail)));
}

bool RequestedDatabaseMatches(const HostedDatabaseSnapshot& database,
                              const std::string& requested_database) {
  constexpr std::string_view kDevBootstrapPath = "dev_bootstrap_path:";
  std::string_view selector(requested_database);
  if (selector.starts_with(kDevBootstrapPath)) selector.remove_prefix(kDevBootstrapPath.size());
  return selector.empty() || selector == "default" ||
         selector == database.database_path ||
         selector == database.database_uuid;
}

bool AuthContextMatchesHostedDatabase(const ServerSessionRecord& session,
                                      const HostedDatabaseSnapshot& database) {
  if (!session.database_path.empty() && session.database_path != database.database_path) return false;
  if (!session.database_uuid.empty() && session.database_uuid != database.database_uuid) return false;
  return true;
}

ConfigPolicySecurityLifecycle ConfigPolicySecurityLifecycleFromDatabase(
    const HostedDatabaseSnapshot& database) {
  ConfigPolicySecurityLifecycle lifecycle;
  lifecycle.database_path = database.database_path;
  lifecycle.database_uuid = database.database_uuid;
  lifecycle.config_source = "hosted_database";
  lifecycle.config_source_epoch = database.config_source_epoch == 0 ? 1 : database.config_source_epoch;
  lifecycle.config_reload_generation =
      database.config_reload_generation == 0 ? 1 : database.config_reload_generation;
  lifecycle.capability_policy_generation =
      database.capability_policy_generation == 0 ? 1 : database.capability_policy_generation;
  lifecycle.policy_generation = database.policy_generation == 0 ? 1 : database.policy_generation;
  lifecycle.security_epoch = database.security_epoch == 0 ? 1 : database.security_epoch;
  lifecycle.provider_family = database.security_provider_family.empty()
                                  ? "local_password"
                                  : database.security_provider_family;
  lifecycle.provider_generation =
      database.security_provider_generation == 0 ? 1 : database.security_provider_generation;
  lifecycle.provider_state = ParseSecurityProviderLifecycleState(
      database.security_provider_state.empty() ? "healthy" : database.security_provider_state);
  lifecycle.provider_plugin_loaded =
      lifecycle.provider_state != SecurityProviderLifecycleState::kDisabled &&
      lifecycle.provider_state != SecurityProviderLifecycleState::kQuarantined;
  lifecycle.provider_started = lifecycle.provider_state != SecurityProviderLifecycleState::kLoaded &&
                               lifecycle.provider_plugin_loaded;
  lifecycle.provider_healthy = lifecycle.provider_state == SecurityProviderLifecycleState::kHealthy ||
                               lifecycle.provider_state == SecurityProviderLifecycleState::kStarted;
  lifecycle.default_policy_installed = database.default_policy_installed;
  lifecycle.cluster_authority_required = database.cluster_authority_required;
  lifecycle.cache_invalidation_epoch =
      database.cache_invalidation_epoch == 0 ? 1 : database.cache_invalidation_epoch;
  lifecycle.invalidated_cache_targets = {"sessions",
                                         "prepared_statements",
                                         "parser_pools",
                                         "capability_policy_cache",
                                         "listener_pools",
                                         "manager_routes",
                                         "ipc_channels",
                                         "metrics_descriptors",
                                         "security_assertion_caches"};
  return lifecycle;
}

bool AttachmentModeSupported(const std::string& mode) {
  return mode == "read_write" || mode == "read_only";
}

std::string CanonicalAttachMode(const std::string& mode) {
  return mode.empty() ? "read_write" : mode;
}

bool ConnectionHeaderMatchesPayload(const sbps::Frame& request,
                                    const std::array<std::uint8_t, 16>& payload_connection_uuid) {
  return sbps::IsZeroUuid(request.header.connection_uuid) ||
         request.header.connection_uuid == payload_connection_uuid;
}

bool ConnectionHeaderMatchesSession(const sbps::Frame& request,
                                    const ServerSessionRecord& session) {
  return sbps::IsZeroUuid(request.header.connection_uuid) ||
         sbps::IsZeroUuid(session.connection_uuid) ||
         request.header.connection_uuid == session.connection_uuid;
}

std::string EngineDiagnosticCode(const std::vector<engine_api::EngineApiDiagnostic>& diagnostics,
                                 std::string fallback) {
  if (!diagnostics.empty() && !diagnostics.front().code.empty()) return diagnostics.front().code;
  return fallback;
}

std::string EngineDiagnosticDetail(const std::vector<engine_api::EngineApiDiagnostic>& diagnostics,
                                   std::string fallback = {}) {
  if (!diagnostics.empty() && !diagnostics.front().detail.empty()) return diagnostics.front().detail;
  return fallback;
}

std::string EngineEvidenceValue(const engine_api::EngineApiResult& result,
                                std::string_view evidence_kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == evidence_kind) return evidence.evidence_id;
  }
  return {};
}

engine_api::EngineRequestContext EngineContextBase(const HostedEngineState& engine_state,
                                                   const sbps::Frame& request,
                                                   const std::string& language = "en") {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.request_id = UuidBytesToText(request.header.request_uuid);
  context.database_path = FirstOpenDatabasePath(engine_state);
  context.database_uuid.canonical = FirstOpenDatabaseUuid(engine_state);
  context.statement_uuid.canonical = context.request_id;
  context.statement_timestamp = CurrentUtcTimestampText();
  context.current_timestamp = context.statement_timestamp;
  context.current_monotonic_ns = CurrentMonotonicNsText();
  context.security_context_present = false;
  context.cluster_authority_available = false;
  context.language_context.language_tag = language.empty() ? "en" : language;
  context.language_context.default_language_tag = "en";
  return context;
}

engine_api::EngineTrustMode TrustModeForSession(const ServerSessionRecord& session) {
  return session.embedded_in_process ? engine_api::EngineTrustMode::embedded_in_process
                                     : engine_api::EngineTrustMode::server_isolated;
}

void AddMaterializedSessionGrant(engine_api::EngineMaterializedAuthorizationContext* authorization,
                                 const engine_api::EngineUuid& principal_uuid,
                                 std::string tag,
                                 bool deny) {
  if (authorization == nullptr || tag.empty()) return;
  std::string right = std::move(tag);
  engine_api::EngineUuid target_uuid;
  const std::size_t separator = right.find(':');
  if (separator != std::string::npos) {
    target_uuid.canonical = right.substr(separator + 1);
    right.resize(separator);
  }
  if (right.empty()) return;

  engine_api::EngineMaterializedAuthorizationGrant grant;
  grant.subject_uuid = principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid = std::move(target_uuid);
  grant.right = std::move(right);
  grant.deny = deny;
  grant.security_epoch = authorization->security_epoch;
  authorization->grants.push_back(std::move(grant));
}

engine_api::EngineMaterializedAuthorizationContext MaterializeSessionAuthorizationContext(
    const ServerSessionRecord& session,
    const engine_api::EngineRequestContext& context) {
  engine_api::EngineMaterializedAuthorizationContext authorization;
  authorization.authority_uuid = context.database_uuid;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = session.policy_generation;
  authorization.catalog_generation_id = context.catalog_generation_id;
  if (authorization.principal_uuid.canonical.empty() ||
      authorization.authority_uuid.canonical.empty() ||
      authorization.security_epoch == 0 ||
      authorization.policy_epoch == 0 ||
      authorization.catalog_generation_id == 0) {
    return authorization;
  }

  engine_api::EngineAuthorizationSubject principal;
  principal.subject_uuid = authorization.principal_uuid;
  principal.subject_kind = "principal";
  authorization.effective_subjects.push_back(std::move(principal));

  for (const auto& tag : session.engine_authorization_trace_tags) {
    authorization.evidence_tags.push_back(tag);
    if (StartsWith(tag, "deny:")) {
      AddMaterializedSessionGrant(&authorization,
                                  authorization.principal_uuid,
                                  tag.substr(std::string_view("deny:").size()),
                                  true);
    } else if (StartsWith(tag, "right:")) {
      AddMaterializedSessionGrant(&authorization,
                                  authorization.principal_uuid,
                                  tag.substr(std::string_view("right:").size()),
                                  false);
    }
  }
  if (!authorization.grants.empty()) {
    authorization.present = true;
    authorization.evidence_tags.push_back("server.session.materialized_authorization_context");
  }
  return authorization;
}

engine_api::EngineRequestContext EngineContextForSession(const ServerSessionRecord& session,
                                                         const HostedEngineState& engine_state,
                                                         const sbps::Frame& request) {
  auto context = EngineContextBase(engine_state, request, session.language_profile);
  context.trust_mode = TrustModeForSession(session);
  context.database_path = session.database_path.empty() ? context.database_path : session.database_path;
  context.database_uuid.canonical =
      session.database_uuid.empty() ? context.database_uuid.canonical : session.database_uuid;
  context.principal_uuid.canonical = UuidBytesToText(session.effective_user_uuid);
  context.session_uuid.canonical = UuidBytesToText(session.session_uuid);
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.local_transaction_id = session.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  context.transaction_timestamp = session.transaction_timestamp;
  context.application_name = session.application_name;
  context.security_context_present = true;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  context.trace_tags = session.engine_authorization_trace_tags;
  context.trace_tags.push_back("sb_server.session_registry");
  context.authorization_context = MaterializeSessionAuthorizationContext(session, context);
  return context;
}

void ApplyBeginTransactionResultToSession(
    const engine_api::EngineBeginTransactionResult& result,
    ServerSessionRecord* session) {
  if (session == nullptr) return;
  session->local_transaction_id = result.local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = result.transaction_uuid.canonical;
  session->transaction_timestamp = EngineEvidenceValue(result, "transaction_timestamp");
}

bool StartAlwaysActiveTransactionForSession(ServerSessionRecord* session,
                                            const HostedEngineState& engine_state,
                                            const sbps::Frame& request,
                                            std::string* diagnostic_code,
                                            std::string* diagnostic_detail) {
  if (session == nullptr) {
    if (diagnostic_code != nullptr) *diagnostic_code = "PARSER_SERVER_IPC.SESSION_REQUIRED";
    if (diagnostic_detail != nullptr) *diagnostic_detail = "session_required";
    return false;
  }
  engine_api::EngineBeginTransactionRequest begin;
  begin.context = EngineContextForSession(*session, engine_state, request);
  begin.context.local_transaction_id = 0;
  begin.context.transaction_uuid.canonical.clear();
  begin.context.snapshot_visible_through_local_transaction_id = 0;
  begin.context.transaction_timestamp.clear();
  begin.context.read_only_mode =
      session->attach_mode == "read_only" || session->default_transaction_read_only;
  begin.isolation_level = session->default_transaction_isolation_level;
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_only:") + (begin.context.read_only_mode ? "true" : "false"));
  begin.transaction_policy_profile.encoded_profiles.push_back(
      std::string("transaction_read_mode:") + (begin.context.read_only_mode ? "read_only" : "read_write"));
  const auto begun = engine_api::EngineBeginTransaction(begin);
  if (!begun.ok || begun.local_transaction_id == 0) {
    if (diagnostic_code != nullptr) {
      *diagnostic_code = EngineDiagnosticCode(begun.diagnostics,
                                              "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED");
    }
    if (diagnostic_detail != nullptr) {
      *diagnostic_detail = EngineDiagnosticDetail(begun.diagnostics,
                                                  "transaction_begin_failed");
    }
    return false;
  }
  ApplyBeginTransactionResultToSession(begun, session);
  return true;
}

std::optional<AuthHandoffPayload> DecodeAuthPayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 16 + 4) return std::nullopt;
  std::size_t offset = 0;
  AuthHandoffPayload auth;
  auth.connection_uuid = GetUuid(payload, offset);
  offset += 16;
  if (offset + 4 > payload.size()) return std::nullopt;
  auth.credential_evidence_present = payload[offset++] != 0;
  auth.credential_invalid = payload[offset++] != 0;
  auth.mfa_required = payload[offset++] != 0;
  auth.mfa_evidence_present = payload[offset++] != 0;
  if (!ReadString(payload, &offset, &auth.provider_family)) return std::nullopt;
  if (!ReadString(payload, &offset, &auth.principal_claim)) return std::nullopt;
  if (!ReadString(payload, &offset, &auth.requested_database)) return std::nullopt;
  if (!ReadString(payload, &offset, &auth.requested_language)) return std::nullopt;
  if (offset < payload.size() && !ReadString(payload, &offset, &auth.credential_evidence)) {
    return std::nullopt;
  }
  if (offset < payload.size() && !ReadString(payload, &offset, &auth.application_name)) {
    return std::nullopt;
  }
  return auth;
}

std::map<std::string, std::string> ParseAuthEvidenceFields(std::string_view evidence) {
  std::map<std::string, std::string> fields;
  std::size_t cursor = 0;
  while (cursor < evidence.size()) {
    const std::size_t end = evidence.find(';', cursor);
    const std::string_view part =
        evidence.substr(cursor,
                        end == std::string_view::npos ? evidence.size() - cursor
                                                       : end - cursor);
    const std::size_t eq = part.find('=');
    if (eq != std::string_view::npos && eq != 0 && eq + 1 < part.size()) {
      fields.emplace(std::string(part.substr(0, eq)), std::string(part.substr(eq + 1)));
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return fields;
}

struct TlsTransportDenial {
  bool denied = false;
  std::string code;
  std::string detail;
};

TlsTransportDenial TlsTransportDenialFromEvidence(const AuthHandoffPayload& auth) {
  const auto fields = ParseAuthEvidenceFields(auth.credential_evidence);
  auto value = [&](const std::string& key) -> std::string {
    const auto found = fields.find(key);
    return found == fields.end() ? std::string{} : found->second;
  };
  const std::string tls_downgrade = value("tls_downgrade");
  const std::string tls_required = value("tls_required");
  const std::string tls_negotiated = value("tls_negotiated");
  if (tls_downgrade == "true" ||
      (tls_required == "true" &&
       (tls_negotiated.empty() || tls_negotiated == "cleartext" ||
        tls_negotiated == "none"))) {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED",
            "tls_downgrade_refused"};
  }
  const std::string cert_status = value("tls_client_cert_status");
  if (cert_status == "wrong_ca" || cert_status == "invalid_ca") {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CLIENT_CA_INVALID",
            "tls_client_ca_invalid"};
  }
  if (cert_status == "expired") {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CLIENT_CERT_EXPIRED",
            "tls_client_cert_expired"};
  }
  const std::string channel_binding = value("tls_channel_binding_status");
  if (channel_binding == "mismatch") {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISMATCH",
            "tls_channel_binding_mismatch"};
  }
  if (tls_required == "true" &&
      (channel_binding.empty() ||
       (channel_binding != "ok" && channel_binding != "matched" &&
        channel_binding != "verified"))) {
    return {true,
            "SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISSING",
            "tls_channel_binding_missing"};
  }
  return {};
}

std::optional<AttachPayload> DecodeAttachPayload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 32) return std::nullopt;
  std::size_t offset = 0;
  AttachPayload attach;
  attach.connection_uuid = GetUuid(payload, offset);
  offset += 16;
  attach.auth_context_uuid = GetUuid(payload, offset);
  offset += 16;
  if (!ReadString(payload, &offset, &attach.requested_database)) return std::nullopt;
  if (!ReadString(payload, &offset, &attach.requested_attachment_mode)) return std::nullopt;
  return attach;
}

std::vector<std::uint8_t> EncodeAuthResultPayload(const std::string& outcome,
                                                  const ServerSessionRecord* session,
                                                  const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->auth_context_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->session_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->principal_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->effective_user_uuid);
  PutU64(&out, session == nullptr ? 0 : session->security_epoch);
  PutString(&out, detail);
  PutString(&out, session == nullptr ? "" : session->database_engine_agent_health_json);
  return out;
}

std::vector<std::uint8_t> EncodeAttachResultPayload(const std::string& outcome,
                                                    const ServerSessionRecord* session,
                                                    const std::string& detail = {}) {
  std::vector<std::uint8_t> out;
  PutString(&out, outcome);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->session_uuid);
  PutUuid(&out, session == nullptr ? std::array<std::uint8_t, 16>{} : session->effective_user_uuid);
  PutString(&out, session == nullptr ? "" : session->database_path);
  PutString(&out, session == nullptr ? "" : session->database_uuid);
  PutString(&out, session == nullptr ? "" : session->attach_mode);
  PutU64(&out, session == nullptr ? 0 : session->catalog_generation);
  PutU64(&out, session == nullptr ? 0 : session->security_epoch);
  PutU64(&out, session == nullptr ? 0 : session->policy_generation);
  PutU64(&out, session == nullptr ? 0 : session->name_resolution_epoch);
  PutU64(&out, session == nullptr ? 0 : session->resource_epoch);
  PutString(&out, detail);
  PutString(&out, session == nullptr ? "" : session->database_engine_agent_health_json);
  PutU64(&out, session == nullptr ? 0 : session->local_transaction_id);
  PutU64(&out, session == nullptr ? 0 : session->snapshot_visible_through_local_transaction_id);
  PutString(&out, session == nullptr ? "" : session->transaction_uuid);
  PutString(&out, session == nullptr ? "" : session->transaction_timestamp);
  return out;
}

std::string AuthContextKey(const std::array<std::uint8_t, 16>& uuid) {
  return UuidBytesToText(uuid);
}

void RecordFinality(ServerSessionRegistry* registry,
                    const sbps::Frame& request,
                    const std::array<std::uint8_t, 16>& session_uuid,
                    const std::array<std::uint8_t, 16>& auth_context_uuid,
                    std::string operation,
                    std::string state,
                    std::string detail = {}) {
  ServerFinalityRecord finality;
  finality.finality_token_uuid = sbps::MakeUuidV7Bytes();
  finality.request_uuid = request.header.request_uuid;
  finality.session_uuid = session_uuid;
  finality.auth_context_uuid = auth_context_uuid;
  finality.operation = std::move(operation);
  finality.state = std::move(state);
  finality.detail = std::move(detail);
  registry->finality_by_request_uuid[UuidBytesToText(finality.request_uuid)] = std::move(finality);
}

void UpsertRequestFinality(ServerSessionRegistry* registry,
                           const ServerRequestRecord& request) {
  if (registry == nullptr) return;
  ServerFinalityRecord finality;
  finality.finality_token_uuid = request.finality_token_uuid;
  finality.request_uuid = request.request_uuid;
  finality.session_uuid = request.session_uuid;
  finality.auth_context_uuid = request.auth_context_uuid;
  finality.operation = request.operation_id.empty() ? request.request_kind : request.operation_id;
  finality.state = ServerRequestLifecycleStateName(request.state);
  finality.detail = request.detail;
  registry->finality_by_request_uuid[UuidBytesToText(finality.request_uuid)] = std::move(finality);
}

bool TerminalRequestState(ServerRequestLifecycleState state) {
  return state == ServerRequestLifecycleState::kCompleted ||
         state == ServerRequestLifecycleState::kCancelled ||
         state == ServerRequestLifecycleState::kTimedOut ||
         state == ServerRequestLifecycleState::kDrained ||
         state == ServerRequestLifecycleState::kDisconnected ||
         state == ServerRequestLifecycleState::kUnknownOutcome ||
         state == ServerRequestLifecycleState::kFailed;
}

bool RequestTargetMatches(const ServerRequestRecord& request, const std::string& target_uuid) {
  if (target_uuid.empty()) return true;
  return UuidBytesToText(request.request_uuid) == target_uuid ||
         UuidBytesToText(request.finality_token_uuid) == target_uuid ||
         UuidBytesToText(request.prepared_statement_uuid) == target_uuid ||
         UuidBytesToText(request.cursor_uuid) == target_uuid;
}

std::string RequestLifecycleRecordJson(const ServerRequestRecord& request) {
  std::ostringstream out;
  out << "{\"request_uuid\":\"" << UuidBytesToText(request.request_uuid)
      << "\",\"finality_token_uuid\":\"" << UuidBytesToText(request.finality_token_uuid)
      << "\",\"session_uuid\":\"" << UuidBytesToText(request.session_uuid)
      << "\",\"request_kind\":\"" << JsonEscape(request.request_kind)
      << "\",\"operation_id\":\"" << JsonEscape(request.operation_id)
      << "\",\"state\":\"" << ServerRequestLifecycleStateName(request.state)
      << "\",\"detail\":\"" << JsonEscape(request.detail)
      << "\",\"prepared_statement_uuid\":\"" << UuidBytesToText(request.prepared_statement_uuid)
      << "\",\"cursor_uuid\":\"" << UuidBytesToText(request.cursor_uuid)
      << "\",\"local_transaction_id_at_start\":" << request.local_transaction_id_at_start
      << ",\"snapshot_visible_through_local_transaction_id\":"
      << request.snapshot_visible_through_local_transaction_id
      << ",\"fetch_timeout_ms\":" << request.fetch_timeout_ms
      << ",\"cancel_timeout_ms\":" << request.cancel_timeout_ms
      << ",\"drain_timeout_ms\":" << request.drain_timeout_ms
      << ",\"authorization_proven\":" << (request.authorization_proven ? "true" : "false")
      << ",\"transaction_finality_preserved\":"
      << (request.transaction_finality_preserved ? "true" : "false")
      << ",\"engine_result_retained\":"
      << (request.engine_result_retained ? "true" : "false") << "}";
  return out.str();
}

ServerDiagnostic RequestLifecycleDiagnostic(std::string code,
                                            ServerDiagnosticSeverity severity,
                                            std::string message,
                                            std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic DriverTransactionDiagnostic(std::string code,
                                             std::string message,
                                             std::string detail,
                                             std::string sqlstate,
                                             std::string finality_state) {
  const std::string message_key = code;
  std::vector<ServerDiagnosticField> fields{{"detail", std::move(detail)},
                                            {"sqlstate", std::move(sqlstate)},
                                            {"finality_state", std::move(finality_state)},
                                            {"retryability", "no_hidden_retry"},
                                            {"mga_finality_authority", "engine"}};
  return ServerDiagnostic{std::move(code),
                          message_key,
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

ServerDiagnostic TransactionPressureDiagnostic(std::string code,
                                               std::string message,
                                               std::string detail,
                                               bool mutates_transaction,
                                               bool opens_replacement_boundary) {
  const std::string message_key = code;
  std::vector<ServerDiagnosticField> fields{
      {"detail", std::move(detail)},
      {"mga_finality_authority", "engine"},
      {"agent_authority", "policy_and_server_gate"},
      {"parser_finality_authority", "false"},
      {"client_state_authority", "false"},
      {"mutates_transaction", mutates_transaction ? "true" : "false"},
      {"opens_replacement_boundary", opens_replacement_boundary ? "true" : "false"}};
  return ServerDiagnostic{std::move(code),
                          message_key,
                          mutates_transaction ? ServerDiagnosticSeverity::kWarning
                                              : ServerDiagnosticSeverity::kInfo,
                          std::move(message),
                          std::move(fields)};
}

std::string DetachCleanupDetail(std::string disconnect_reason,
                                std::uint64_t sessions_removed,
                                std::uint64_t auth_contexts_removed,
                                std::uint64_t prepared_tombstoned,
                                std::uint64_t cursors_tombstoned,
                                std::uint64_t engine_results_released,
                                std::uint64_t request_finality_records_updated,
                                std::uint64_t active_local_transaction_id) {
  std::ostringstream out;
  out << "disconnect_reason=" << disconnect_reason
      << ";sessions_removed=" << sessions_removed
      << ";auth_contexts_removed=" << auth_contexts_removed
      << ";prepared_tombstoned=" << prepared_tombstoned
      << ";cursors_tombstoned=" << cursors_tombstoned
      << ";engine_results_released=" << engine_results_released
      << ";request_finality_records_updated=" << request_finality_records_updated
      << ";disconnect_does_not_commit=true"
      << ";disconnect_does_not_rollback=true"
      << ";mga_finality_authority=engine";
  if (active_local_transaction_id != 0) {
    out << ";active_local_transaction_id=" << active_local_transaction_id
        << ";active_transaction_outcome=unknown_preserved";
  } else {
    out << ";active_transaction_outcome=none";
  }
  return out.str();
}

bool IsZeroUuidBytes(const std::array<std::uint8_t, 16>& uuid) {
  return std::all_of(uuid.begin(), uuid.end(), [](std::uint8_t value) {
    return value == 0;
  });
}

bool UuidMatchesIfPresent(const std::array<std::uint8_t, 16>& expected,
                          const std::array<std::uint8_t, 16>& actual) {
  return IsZeroUuidBytes(expected) || expected == actual;
}

bool ContainsUuid(const std::vector<std::array<std::uint8_t, 16>>& values,
                  const std::array<std::uint8_t, 16>& target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

ServerDiagnostic SessionControlDiagnostic(std::string code,
                                          ServerDiagnosticSeverity severity,
                                          std::string message,
                                          std::string detail,
                                          std::vector<ServerDiagnosticField> fields = {}) {
  if (!detail.empty()) fields.push_back({"detail", detail});
  fields.push_back({"server_session_registry_authority", "true"});
  fields.push_back({"parser_session_authority", "false"});
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

ServerSessionBindingControlResult SessionControlRejected(
    std::string code,
    std::string detail,
    std::string message,
    std::vector<ServerDiagnosticField> fields = {}) {
  ServerSessionBindingControlResult result;
  result.diagnostic_code = code;
  result.detail = detail;
  result.authorization_denied = code == "SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED";
  result.replay_rejected = code == "SERVER.SESSION_CONTROL.REPLAY_REFUSED" ||
                           code == "SERVER.SESSION_TAKEOVER.REPLAY_REFUSED";
  result.diagnostics.push_back(SessionControlDiagnostic(std::move(code),
                                                        ServerDiagnosticSeverity::kError,
                                                        std::move(message),
                                                        std::move(detail),
                                                        std::move(fields)));
  return result;
}

bool SessionControlAuthorized(const ServerSessionControlAuthority& authority,
                              bool allowed) {
  return authority.authenticated && allowed && authority.sequence != 0 &&
         !authority.authority_class.empty() && !authority.actor_token.empty();
}

void MirrorAuthContext(ServerSessionRegistry* registry, const ServerSessionRecord& session) {
  if (registry == nullptr || IsZeroUuidBytes(session.auth_context_uuid)) return;
  const auto key = AuthContextKey(session.auth_context_uuid);
  if (registry->auth_contexts_by_uuid.find(key) != registry->auth_contexts_by_uuid.end()) {
    registry->auth_contexts_by_uuid[key] = session;
  }
}

std::map<std::string, ServerSessionRecord>::iterator FindMutableSessionByBindingTarget(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& catalog_session_id,
    const std::array<std::uint8_t, 16>& protocol_session_id) {
  if (registry == nullptr) return {};
  if (!IsZeroUuidBytes(catalog_session_id)) {
    return registry->sessions_by_uuid.find(UuidBytesToText(catalog_session_id));
  }
  if (!IsZeroUuidBytes(protocol_session_id)) {
    for (auto it = registry->sessions_by_uuid.begin(); it != registry->sessions_by_uuid.end(); ++it) {
      if (it->second.session_binding_present &&
          it->second.protocol_session_id == protocol_session_id) {
        return it;
      }
    }
  }
  return registry->sessions_by_uuid.end();
}

std::map<std::string, ServerSessionRecord>::const_iterator FindSessionByBindingTarget(
    const ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& catalog_session_id,
    const std::array<std::uint8_t, 16>& protocol_session_id) {
  if (!IsZeroUuidBytes(catalog_session_id)) {
    return registry.sessions_by_uuid.find(UuidBytesToText(catalog_session_id));
  }
  if (!IsZeroUuidBytes(protocol_session_id)) {
    for (auto it = registry.sessions_by_uuid.begin(); it != registry.sessions_by_uuid.end(); ++it) {
      if (it->second.session_binding_present &&
          it->second.protocol_session_id == protocol_session_id) {
        return it;
      }
    }
  }
  return registry.sessions_by_uuid.end();
}

bool TakeoverClaimsMatch(const ServerSessionRecord& session,
                         const ServerSessionTakeoverRequest& request,
                         std::string* detail) {
  if (!session.session_binding_present) {
    if (detail != nullptr) *detail = "session_binding_required";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimCatalogSessionId) &&
      request.catalog_session_id != session.catalog_session_id) {
    if (detail != nullptr) *detail = "catalog_session_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimAuthkeyId) &&
      !UuidMatchesIfPresent(request.authkey_id, session.authkey_id)) {
    if (detail != nullptr) *detail = "authkey_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimAuthenticatedPrincipalId) &&
      !UuidMatchesIfPresent(request.authenticated_principal_id, session.principal_uuid)) {
    if (detail != nullptr) *detail = "authenticated_principal_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimSessionUserId) &&
      !UuidMatchesIfPresent(request.session_user_id, session.effective_user_uuid)) {
    if (detail != nullptr) *detail = "session_user_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimActiveRoleId) &&
      !UuidMatchesIfPresent(request.active_role_id, session.active_role_uuid)) {
    if (detail != nullptr) *detail = "active_role_id_mismatch";
    return false;
  }
  if ((request.mask & kServerTakeoverClaimCurrentTxnId) &&
      request.current_txn_id != session.local_transaction_id) {
    if (detail != nullptr) *detail = "current_txn_id_mismatch";
    return false;
  }
  for (const auto& group : request.group_ids) {
    if (!ContainsUuid(session.effective_group_uuids, group)) {
      if (detail != nullptr) *detail = "effective_group_id_mismatch";
      return false;
    }
  }
  if (detail != nullptr) *detail = "takeover_claims_match";
  return true;
}

}  // namespace

const char* ServerChannelStateName(ServerChannelState state) {
  switch (state) {
    case ServerChannelState::kProtocolAdmitted: return "protocol_admitted";
    case ServerChannelState::kAuthPending: return "auth_pending";
    case ServerChannelState::kAttachPending: return "attach_pending";
    case ServerChannelState::kSessionBound: return "session_bound";
    case ServerChannelState::kReady: return "ready";
    case ServerChannelState::kDraining: return "draining";
    case ServerChannelState::kDetached: return "detached";
    case ServerChannelState::kClosed: return "closed";
    case ServerChannelState::kFailed: return "failed";
  }
  return "failed";
}

const char* ServerRequestLifecycleStateName(ServerRequestLifecycleState state) {
  switch (state) {
    case ServerRequestLifecycleState::kAdmitted: return "admitted";
    case ServerRequestLifecycleState::kActive: return "active";
    case ServerRequestLifecycleState::kCursorOpen: return "cursor_open";
    case ServerRequestLifecycleState::kCompleted: return "completed";
    case ServerRequestLifecycleState::kCancelled: return "cancelled";
    case ServerRequestLifecycleState::kTimedOut: return "timed_out";
    case ServerRequestLifecycleState::kDrained: return "drained";
    case ServerRequestLifecycleState::kDisconnected: return "disconnected";
    case ServerRequestLifecycleState::kUnknownOutcome: return "unknown_outcome";
    case ServerRequestLifecycleState::kFailed: return "failed";
  }
  return "failed";
}

const char* ServerDriverTransactionEventName(ServerDriverTransactionEvent event) {
  switch (event) {
    case ServerDriverTransactionEvent::kAttachInitialBoundary:
      return "attach_initial_boundary";
    case ServerDriverTransactionEvent::kAutocommitStatementSucceeded:
      return "autocommit_statement_succeeded";
    case ServerDriverTransactionEvent::kAutocommitStatementFailed:
      return "autocommit_statement_failed";
    case ServerDriverTransactionEvent::kCommitCompleted: return "commit_completed";
    case ServerDriverTransactionEvent::kRollbackCompleted: return "rollback_completed";
    case ServerDriverTransactionEvent::kPrepareTransactionCompleted:
      return "prepare_transaction_completed";
    case ServerDriverTransactionEvent::kCancelStatement: return "cancel_statement";
    case ServerDriverTransactionEvent::kResetSession: return "reset_session";
    case ServerDriverTransactionEvent::kReconnectAfterDisconnect:
      return "reconnect_after_disconnect";
    case ServerDriverTransactionEvent::kPoolReturn: return "pool_return";
    case ServerDriverTransactionEvent::kSavepointOperation: return "savepoint_operation";
    case ServerDriverTransactionEvent::kXaRecoverPrepared: return "xa_recover_prepared";
    case ServerDriverTransactionEvent::kDormantDetach: return "dormant_detach";
    case ServerDriverTransactionEvent::kDormantReattach: return "dormant_reattach";
    case ServerDriverTransactionEvent::kRetryAfterUnknownFinality:
      return "retry_after_unknown_finality";
  }
  return "unknown_driver_transaction_event";
}

const char* ServerTransactionPressureActionName(ServerTransactionPressureAction action) {
  switch (action) {
    case ServerTransactionPressureAction::kNoAction:
      return "no_action";
    case ServerTransactionPressureAction::kWarnNotify:
      return "warn_notify";
    case ServerTransactionPressureAction::kRequestRestart:
      return "request_restart";
    case ServerTransactionPressureAction::kRequestReauth:
      return "request_reauth";
    case ServerTransactionPressureAction::kRequestCancel:
      return "request_cancel";
    case ServerTransactionPressureAction::kForceRollback:
      return "force_rollback";
    case ServerTransactionPressureAction::kForceCommit:
      return "force_commit";
    case ServerTransactionPressureAction::kForceRestart:
      return "force_restart";
  }
  return "unknown_transaction_pressure_action";
}

ServerDriverTransactionDecision ClassifyDriverTransactionEvent(
    const ServerSessionRecord& session,
    const ServerDriverTransactionDecisionInput& input) {
  const bool active_transaction = input.active_transaction || session.local_transaction_id != 0;
  auto decision = ServerDriverTransactionDecision{};
  decision.action = ServerDriverTransactionEventName(input.event);

  auto reject = [&](std::string code,
                    std::string message,
                    std::string detail,
                    std::string sqlstate,
                    std::string finality_state) {
    decision.accepted = false;
    decision.driver_may_retry = false;
    decision.hidden_retry_forbidden = true;
    decision.diagnostic_code = code;
    decision.sqlstate = sqlstate;
    decision.finality_state = finality_state;
    decision.must_query_engine_finality =
        finality_state == "unknown_until_engine_finality_report";
    decision.diagnostics.push_back(DriverTransactionDiagnostic(std::move(code),
                                                               std::move(message),
                                                               std::move(detail),
                                                               std::move(sqlstate),
                                                               std::move(finality_state)));
  };

  auto finality_unknown = [&]() {
    reject("SERVER.DRIVER_TX.FINALITY_UNKNOWN",
           "The driver-visible transaction outcome is unknown until engine MGA finality is queried.",
           "unknown_until_engine_finality_report",
           "08007",
           "unknown_until_engine_finality_report");
    decision.requires_explicit_engine_recovery = input.prepared_transaction_present;
  };

  auto require_active = [&]() -> bool {
    if (active_transaction) return true;
    reject("SERVER.DRIVER_TX.ACTIVE_TRANSACTION_REQUIRED",
           "The driver-visible operation requires an active engine-owned MGA transaction.",
           "active_transaction_required",
           "25000",
           "no_state_change");
    return false;
  };

  switch (input.event) {
    case ServerDriverTransactionEvent::kAttachInitialBoundary:
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_ACTIVE";
      decision.action = "open_initial_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kAutocommitStatementSucceeded:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_COMMITTED";
      decision.finality_state = "committed_by_engine_inventory";
      decision.action = "commit_statement_boundary_and_open_next";
      return decision;

    case ServerDriverTransactionEvent::kAutocommitStatementFailed:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_ROLLED_BACK";
      decision.finality_state = "rolled_back_by_engine_inventory";
      decision.action = "rollback_statement_boundary_and_open_next";
      return decision;

    case ServerDriverTransactionEvent::kCommitCompleted:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.durable_state = "TX_DURABLE_COMMITTED";
      decision.finality_state = "committed_by_engine_inventory";
      decision.action = "commit_and_open_next_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kRollbackCompleted:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.durable_state = "TX_DURABLE_ROLLED_BACK";
      decision.finality_state = "rolled_back_by_engine_inventory";
      decision.action = "rollback_and_open_next_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kPrepareTransactionCompleted:
      if (!require_active()) return decision;
      if (!input.engine_finality_known) {
        finality_unknown();
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_PREPARED_HANDOFF";
      decision.durable_state = "TX_DURABLE_PREPARED";
      decision.finality_state = "prepared_by_engine_inventory";
      decision.action = "prepare_handoff_and_open_next_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kCancelStatement:
      if (!input.engine_finality_known) {
        finality_unknown();
        decision.accepted = true;
        decision.preserves_current_boundary = active_transaction;
        decision.sqlstate = "57014";
        return decision;
      }
      decision.accepted = true;
      decision.preserves_current_boundary = active_transaction;
      decision.sqlstate = "57014";
      decision.finality_state = active_transaction
                                    ? "statement_aborted_transaction_state_reported_by_engine"
                                    : "no_state_change";
      decision.action = "cancel_statement_preserve_mga_boundary";
      return decision;

    case ServerDriverTransactionEvent::kResetSession:
      if (active_transaction || input.active_cursor) {
        reject("SERVER.DRIVER_TX.RESET_REQUIRES_CLEAN_BOUNDARY",
               "Session reset requires a clean engine-reported transaction boundary.",
               active_transaction ? "active_transaction_blocks_reset"
                                  : "active_cursor_blocks_reset",
               "25001",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.opens_replacement_boundary = true;
      decision.action = "reset_session_after_clean_engine_boundary";
      return decision;

    case ServerDriverTransactionEvent::kReconnectAfterDisconnect:
      decision.accepted = true;
      decision.invalidates_session = true;
      decision.opens_replacement_boundary = !input.explicit_dormant_token;
      decision.boundary_state = input.explicit_dormant_token ? "TX_BOUNDARY_DORMANT_RETAINED"
                                                            : "TX_BOUNDARY_ACTIVE";
      decision.action = input.explicit_dormant_token
                            ? "explicit_dormant_reattach_required"
                            : "new_connection_no_implicit_reattach";
      if (active_transaction || input.prepared_transaction_present || !input.engine_finality_known) {
        decision.must_query_engine_finality = true;
        decision.finality_state = "unknown_until_engine_finality_report";
        decision.requires_explicit_engine_recovery = input.prepared_transaction_present;
        decision.sqlstate = "08007";
      }
      return decision;

    case ServerDriverTransactionEvent::kPoolReturn:
      if (active_transaction || input.active_cursor) {
        reject("SERVER.DRIVER_TX.POOL_RETURN_REQUIRES_CLEAN_BOUNDARY",
               "A connection cannot return to a pool while transaction or cursor state is active.",
               active_transaction ? "active_transaction_blocks_pool_return"
                                  : "active_cursor_blocks_pool_return",
               "25001",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.action = "pool_return_after_commit_or_rollback_boundary";
      return decision;

    case ServerDriverTransactionEvent::kSavepointOperation:
      if (!require_active()) return decision;
      decision.accepted = true;
      decision.preserves_current_boundary = true;
      decision.action = "savepoint_is_transaction_local_no_independent_authority";
      return decision;

    case ServerDriverTransactionEvent::kXaRecoverPrepared:
      if (!input.prepared_transaction_present) {
        reject("SERVER.DRIVER_TX.PREPARED_TRANSACTION_NOT_FOUND",
               "Prepared transaction recovery did not find engine-owned prepared state.",
               "prepared_transaction_not_found",
               "42704",
               "no_state_change");
        return decision;
      }
      if (!input.xa_recovery_enabled || !input.cluster_authority_active) {
        reject("SERVER.DRIVER_TX.XA_LIMBO_RECOVERY_REQUIRED",
               "XA or prepared transaction recovery requires explicit engine recovery authority.",
               "prepared_or_limbo_recovery_requires_engine_authority",
               "08007",
               "unknown_until_engine_finality_report");
        decision.must_query_engine_finality = true;
        decision.requires_explicit_engine_recovery = true;
        return decision;
      }
      decision.accepted = true;
      decision.requires_explicit_engine_recovery = true;
      decision.finality_state = "decision_pending";
      decision.durable_state = "TX_DURABLE_PREPARED";
      decision.action = "prepared_recovery_delegated_to_engine_authority";
      return decision;

    case ServerDriverTransactionEvent::kDormantDetach:
      if (!require_active()) return decision;
      if (input.active_cursor) {
        reject("SERVER.DRIVER_TX.DORMANT_DETACH_CURSOR_ACTIVE",
               "Dormant detach requires all statement and cursor state to be closed.",
               "active_cursor_blocks_dormant_detach",
               "25001",
               "no_state_change");
        return decision;
      }
      if (!input.dormant_reattach_enabled) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_POLICY_REFUSED",
               "Dormant detach or reattach is disabled by engine policy.",
               "dormant_reattach_disabled",
               "08004",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.preserves_current_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_DORMANT_RETAINED";
      decision.durable_state = "TX_DURABLE_DORMANT";
      decision.action = "explicit_dormant_detach_preserves_engine_transaction";
      return decision;

    case ServerDriverTransactionEvent::kDormantReattach:
      if (!input.explicit_dormant_token) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_TOKEN_REQUIRED",
               "Dormant reattach requires an explicit engine-issued dormant token.",
               "explicit_dormant_token_required",
               "08003",
               "no_state_change");
        return decision;
      }
      if (!input.dormant_reattach_enabled) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_POLICY_REFUSED",
               "Dormant detach or reattach is disabled by engine policy.",
               "dormant_reattach_disabled",
               "08004",
               "no_state_change");
        return decision;
      }
      if (!input.server_admitted_reattach) {
        reject("SERVER.DRIVER_TX.DORMANT_REATTACH_REFUSED",
               "The engine refused the dormant reattach request.",
               "dormant_reattach_not_admitted",
               "08003",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.preserves_current_boundary = true;
      decision.boundary_state = "TX_BOUNDARY_ACTIVE";
      decision.durable_state = "TX_DURABLE_ACTIVE";
      decision.action = "explicit_dormant_reattach_admitted_by_engine";
      return decision;

    case ServerDriverTransactionEvent::kRetryAfterUnknownFinality:
      if (!input.engine_finality_known) {
        reject("SERVER.DRIVER_TX.RETRY_REQUIRES_FINALITY_QUERY",
               "Retry after unknown finality requires an engine finality query first.",
               "retry_after_unknown_finality_forbidden",
               "08007",
               "unknown_until_engine_finality_report");
        decision.must_query_engine_finality = true;
        return decision;
      }
      if (input.statement_has_side_effects && !input.engine_reported_idempotent) {
        reject("SERVER.DRIVER_TX.HIDDEN_RETRY_FORBIDDEN",
               "The driver must not silently retry side-effecting work.",
               "side_effecting_retry_forbidden",
               "40003",
               "no_state_change");
        return decision;
      }
      if (!input.caller_acknowledged_retry_boundary) {
        reject("SERVER.DRIVER_TX.RETRY_REQUIRES_CALLER_ACK",
               "Retry requires caller acknowledgement of the fresh transaction boundary.",
               "caller_acknowledgement_required",
               "40003",
               "no_state_change");
        return decision;
      }
      decision.accepted = true;
      decision.driver_may_retry = true;
      decision.opens_replacement_boundary = true;
      decision.action = "caller_controlled_retry_at_fresh_mga_boundary";
      return decision;
  }

  reject("SERVER.DRIVER_TX.EVENT_UNKNOWN",
         "The driver transaction event is unknown.",
         "event_unknown",
         "HY000",
         "no_state_change");
  return decision;
}

ServerTransactionPressureControlDecision ClassifyServerTransactionPressureControl(
    const ServerSessionRecord& session,
    const ServerTransactionPressureControlInput& input) {
  ServerTransactionPressureControlDecision decision;
  const bool active_transaction =
      input.active_transaction ||
      input.current_local_transaction_id != 0 ||
      session.local_transaction_id != 0;
  const std::uint64_t current_local_transaction_id =
      input.current_local_transaction_id != 0
          ? input.current_local_transaction_id
          : session.local_transaction_id;

  auto evidence = [&](std::string action_name) {
    std::ostringstream out;
    out << "action=" << action_name
        << ";stable_session_id=" << input.stable_session_id
        << ";current_local_transaction_id=" << current_local_transaction_id
        << ";replacement_local_transaction_id=" << input.replacement_local_transaction_id
        << ";agent_authoritative=" << (input.agent_authoritative ? "true" : "false")
        << ";policy_authorized=" << (input.policy_authorized ? "true" : "false")
        << ";session_authorization_bound="
        << (input.session_authorization_bound ? "true" : "false")
        << ";mga_finality_authority=engine"
        << ";parser_finality_authority=false"
        << ";client_state_authority=false";
    if (!input.evidence_id.empty()) {
      out << ";agent_evidence_id=" << input.evidence_id;
    }
    return out.str();
  };

  auto finish = [&](bool accepted,
                    std::string code,
                    std::string message,
                    std::string detail,
                    bool mutates_transaction,
                    bool opens_replacement_boundary) {
    decision.accepted = accepted;
    decision.diagnostic_code = code;
    decision.detail = detail;
    decision.mutates_transaction = mutates_transaction;
    decision.opens_replacement_boundary = opens_replacement_boundary;
    decision.evidence = evidence(ServerTransactionPressureActionName(input.action));
    if (opens_replacement_boundary) {
      decision.evidence += ";always_active_transaction_replacement=true";
      decision.evidence += ";replacement_transaction_rule=must_open_before_client_ready";
    }
    decision.diagnostics.push_back(TransactionPressureDiagnostic(
        std::move(code),
        std::move(message),
        std::move(detail),
        mutates_transaction,
        opens_replacement_boundary));
    return decision;
  };

  auto deny_non_authoritative = [&](std::string detail) {
    decision.denied_non_authoritative = true;
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.DENIED_NON_AUTHORITATIVE",
                  "The transaction pressure action lacked server-authoritative session, policy, or transaction evidence.",
                  std::move(detail),
                  false,
                  false);
  };

  if (!input.agent_authoritative) {
    return deny_non_authoritative("agent_authoritative_required");
  }
  if (!input.policy_authorized) {
    return deny_non_authoritative("policy_authorized_required");
  }
  if (!input.session_authorization_bound) {
    return deny_non_authoritative("session_bound_authorization_required");
  }
  if (input.action != ServerTransactionPressureAction::kNoAction &&
      !active_transaction) {
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.ACTIVE_TRANSACTION_REQUIRED",
                  "Transaction pressure action requires an active engine-owned transaction.",
                  "active_transaction_required",
                  false,
                  false);
  }

  switch (input.action) {
    case ServerTransactionPressureAction::kNoAction:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.NO_ACTION",
                    "The transaction pressure manager took no action.",
                    "no_action",
                    false,
                    false);
    case ServerTransactionPressureAction::kWarnNotify:
      decision.notifies_client = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.WARN_NOTIFY",
                    "The server may notify the session about long idle transaction pressure.",
                    "warn_notify",
                    false,
                    false);
    case ServerTransactionPressureAction::kRequestRestart:
      decision.requests_client_action = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.REQUEST_RESTART",
                    "The server may request a client-visible transaction restart.",
                    "request_restart",
                    false,
                    false);
    case ServerTransactionPressureAction::kRequestReauth:
      decision.requests_client_action = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.REQUEST_REAUTH",
                    "The server may request session reauthentication before transaction pressure action.",
                    "request_reauth",
                    false,
                    false);
    case ServerTransactionPressureAction::kRequestCancel:
      decision.requests_client_action = true;
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.REQUEST_CANCEL",
                    "The server may request cancellation of the transaction pressure blocker.",
                    "request_cancel",
                    false,
                    false);
    case ServerTransactionPressureAction::kForceRollback:
    case ServerTransactionPressureAction::kForceCommit:
    case ServerTransactionPressureAction::kForceRestart:
      break;
  }

  if (!input.engine_finality_known) {
    decision.must_query_engine_finality = true;
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.FINALITY_UNKNOWN",
                  "Forced transaction pressure action requires known engine finality.",
                  "unknown_until_engine_finality_report",
                  false,
                  false);
  }
  if (!input.force_authority_gate) {
    return deny_non_authoritative("force_authority_gate_required");
  }
  if (!input.replacement_transaction_bound ||
      input.replacement_local_transaction_id == 0 ||
      input.replacement_local_transaction_id == current_local_transaction_id) {
    return finish(false,
                  "SERVER.TRANSACTION_PRESSURE.REPLACEMENT_TRANSACTION_REQUIRED",
                  "Forced transaction pressure action must bind a replacement transaction before client-ready state.",
                  "replacement_transaction_required",
                  false,
                  false);
  }

  switch (input.action) {
    case ServerTransactionPressureAction::kForceRollback:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.FORCE_ROLLBACK_REPLACEMENT",
                    "The server accepted a policy-authorized forced rollback with replacement transaction binding.",
                    "force_rollback_replacement",
                    true,
                    true);
    case ServerTransactionPressureAction::kForceCommit:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.FORCE_COMMIT_REPLACEMENT",
                    "The server accepted a policy-authorized forced commit with replacement transaction binding.",
                    "force_commit_replacement",
                    true,
                    true);
    case ServerTransactionPressureAction::kForceRestart:
      return finish(true,
                    "SERVER.TRANSACTION_PRESSURE.FORCE_RESTART_REPLACEMENT",
                    "The server accepted a policy-authorized forced restart with replacement transaction binding.",
                    "force_restart_replacement",
                    true,
                    true);
    case ServerTransactionPressureAction::kNoAction:
    case ServerTransactionPressureAction::kWarnNotify:
    case ServerTransactionPressureAction::kRequestRestart:
    case ServerTransactionPressureAction::kRequestReauth:
    case ServerTransactionPressureAction::kRequestCancel:
      break;
  }
  return finish(false,
                "SERVER.TRANSACTION_PRESSURE.UNKNOWN_ACTION",
                "The transaction pressure action was unknown.",
                "unknown_action",
                false,
                false);
}

std::string ServerDriverTransactionDecisionJson(
    const ServerDriverTransactionDecision& decision) {
  std::ostringstream out;
  out << "{\"accepted\":" << (decision.accepted ? "true" : "false")
      << ",\"driver_may_retry\":" << (decision.driver_may_retry ? "true" : "false")
      << ",\"hidden_retry_forbidden\":"
      << (decision.hidden_retry_forbidden ? "true" : "false")
      << ",\"must_query_engine_finality\":"
      << (decision.must_query_engine_finality ? "true" : "false")
      << ",\"opens_replacement_boundary\":"
      << (decision.opens_replacement_boundary ? "true" : "false")
      << ",\"preserves_current_boundary\":"
      << (decision.preserves_current_boundary ? "true" : "false")
      << ",\"invalidates_session\":" << (decision.invalidates_session ? "true" : "false")
      << ",\"requires_explicit_engine_recovery\":"
      << (decision.requires_explicit_engine_recovery ? "true" : "false")
      << ",\"sqlstate\":\"" << JsonEscape(decision.sqlstate)
      << "\",\"diagnostic_code\":\"" << JsonEscape(decision.diagnostic_code)
      << "\",\"finality_state\":\"" << JsonEscape(decision.finality_state)
      << "\",\"boundary_state\":\"" << JsonEscape(decision.boundary_state)
      << "\",\"durable_state\":\"" << JsonEscape(decision.durable_state)
      << "\",\"action\":\"" << JsonEscape(decision.action) << "\"}";
  return out.str();
}

std::string UuidBytesToText(const std::array<std::uint8_t, 16>& uuid) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(hex[(uuid[i] >> 4u) & 0x0fu]);
    out.push_back(hex[uuid[i] & 0x0fu]);
  }
  return out;
}

std::vector<std::uint8_t> EncodeAuthHandoffPayloadForTest(const std::string& principal,
                                                          bool credential_valid,
                                                          bool mfa_required,
                                                          bool mfa_present,
                                                          const std::string& principal_uuid,
                                                          const std::string& storage_authority) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  out.push_back(credential_valid ? 1 : 0);
  out.push_back(credential_valid ? 0 : 1);
  out.push_back(mfa_required ? 1 : 0);
  out.push_back(mfa_present ? 1 : 0);
  PutString(&out, "local_password");
  PutString(&out, principal);
  PutString(&out, "default");
  PutString(&out, "en");
  const std::string verifier = credential_valid
      ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  std::string evidence = "scheme=local_password_v1;principal=" + principal;
  if (!principal_uuid.empty()) {
    evidence += ";principal_uuid=" + principal_uuid;
    evidence += ";storage_authority=" +
                (storage_authority.empty() ? std::string("mga_security_principal_lifecycle")
                                           : storage_authority);
    evidence += ";authorization_tags=right:CONNECT,right:OBS_MANAGEMENT_CONTROL";
  }
  evidence += ";verifier=" + verifier;
  PutString(&out, evidence);
  return out;
}

std::vector<std::uint8_t> EncodeAttachPayloadForTest(
    const std::array<std::uint8_t, 16>& auth_context_uuid,
    const std::string& mode) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  PutUuid(&out, auth_context_uuid);
  PutString(&out, "default");
  PutString(&out, mode);
  return out;
}

std::optional<std::array<std::uint8_t, 16>> DecodeAuthContextUuidForTest(
    const std::vector<std::uint8_t>& auth_result_payload) {
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(auth_result_payload, &offset, &outcome)) return std::nullopt;
  if (offset + 16 > auth_result_payload.size()) return std::nullopt;
  return GetUuid(auth_result_payload, offset);
}

std::optional<std::array<std::uint8_t, 16>> DecodeSessionUuidForTest(
    const std::vector<std::uint8_t>& attach_result_payload) {
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(attach_result_payload, &offset, &outcome)) return std::nullopt;
  if (offset + 16 > attach_result_payload.size()) return std::nullopt;
  return GetUuid(attach_result_payload, offset);
}

ServerRequestRecord RegisterServerRequestLifecycle(ServerSessionRegistry* registry,
                                                   const sbps::Frame& request,
                                                   const ServerSessionRecord& session,
                                                   std::string request_kind,
                                                   std::string operation_id) {
  ServerRequestRecord record;
  record.request_uuid = sbps::IsZeroUuid(request.header.request_uuid)
                            ? sbps::MakeUuidV7Bytes()
                            : request.header.request_uuid;
  record.finality_token_uuid = sbps::MakeUuidV7Bytes();
  record.session_uuid = session.session_uuid;
  record.auth_context_uuid = session.auth_context_uuid;
  record.request_kind = std::move(request_kind);
  record.operation_id = std::move(operation_id);
  record.state = ServerRequestLifecycleState::kActive;
  record.detail = "request_admitted";
  record.local_transaction_id_at_start = session.local_transaction_id;
  record.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  record.authorization_proven = true;
  if (registry != nullptr) {
    registry->requests_by_uuid[UuidBytesToText(record.request_uuid)] = record;
    UpsertRequestFinality(registry, record);
  }
  return record;
}

void UpdateServerRequestLifecycleOperation(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    std::string operation_id) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.operation_id = std::move(operation_id);
  UpsertRequestFinality(registry, found->second);
}

void LinkServerRequestPreparedStatement(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.prepared_statement_uuid = prepared_statement_uuid;
  UpsertRequestFinality(registry, found->second);
}

void LinkServerRequestCursor(ServerSessionRegistry* registry,
                             const std::array<std::uint8_t, 16>& request_uuid,
                             const std::array<std::uint8_t, 16>& cursor_uuid,
                             bool engine_result_retained) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.cursor_uuid = cursor_uuid;
  found->second.engine_result_retained = engine_result_retained;
  found->second.state = ServerRequestLifecycleState::kCursorOpen;
  found->second.detail = engine_result_retained ? "cursor_open_engine_result_retained"
                                                : "cursor_open";
  UpsertRequestFinality(registry, found->second);
}

void CompleteServerRequestLifecycle(ServerSessionRegistry* registry,
                                    const std::array<std::uint8_t, 16>& request_uuid,
                                    ServerRequestLifecycleState state,
                                    std::string detail) {
  if (registry == nullptr) return;
  auto found = registry->requests_by_uuid.find(UuidBytesToText(request_uuid));
  if (found == registry->requests_by_uuid.end()) return;
  found->second.state = state;
  found->second.detail = std::move(detail);
  found->second.transaction_finality_preserved = true;
  UpsertRequestFinality(registry, found->second);
}

std::optional<ServerRequestRecord> FindServerRequestLifecycle(
    const ServerSessionRegistry& registry,
    const std::string& target_uuid) {
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (UuidBytesToText(request.request_uuid) == target_uuid ||
        UuidBytesToText(request.finality_token_uuid) == target_uuid ||
        UuidBytesToText(request.prepared_statement_uuid) == target_uuid) {
      return request;
    }
  }
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (UuidBytesToText(request.cursor_uuid) == target_uuid &&
        (request.state == ServerRequestLifecycleState::kCursorOpen ||
         request.state == ServerRequestLifecycleState::kActive)) {
      return request;
    }
  }
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (RequestTargetMatches(request, target_uuid)) return request;
  }
  return std::nullopt;
}

std::string ServerRequestLifecycleRecordsJson(const ServerSessionRegistry& registry,
                                              const std::string& target_uuid,
                                              bool include_history) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& [_, request] : registry.requests_by_uuid) {
    if (!RequestTargetMatches(request, target_uuid)) continue;
    if (!include_history && TerminalRequestState(request.state)) continue;
    if (!first) out << ',';
    first = false;
    out << RequestLifecycleRecordJson(request);
  }
  out << "]";
  return out.str();
}

ServerRequestLifecycleResult CancelServerRequestLifecycle(
    ServerSessionRegistry* registry,
    const std::string& target_uuid,
    const ServerSessionRecord& actor,
    bool authorization_proven,
    std::uint64_t cancel_timeout_ms) {
  ServerRequestLifecycleResult result;
  if (registry == nullptr) {
    result.error = true;
    result.outcome = "registry_unavailable";
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "PARSER_SERVER_IPC.REQUEST_REGISTRY_UNAVAILABLE",
        ServerDiagnosticSeverity::kError,
        "Request lifecycle registry is unavailable."));
    return result;
  }

  std::vector<std::string> matched_keys;
  for (const auto& [key, request] : registry->requests_by_uuid) {
    if (RequestTargetMatches(request, target_uuid)) {
      matched_keys.push_back(key);
    }
  }
  if (matched_keys.empty()) {
    result.accepted = true;
    result.unknown_outcome = true;
    result.outcome = "unknown_finality";
    result.records_json =
        "[{\"requested_target_uuid\":\"" + JsonEscape(target_uuid) +
        "\",\"finality_state\":\"unknown\",\"diagnostic_code\":\"SERVER.REQUEST.FINALITY_UNKNOWN\"}]";
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "SERVER.REQUEST.FINALITY_UNKNOWN",
        ServerDiagnosticSeverity::kWarning,
        "The requested request finality token is unknown.",
        {{"target_uuid", target_uuid}}));
    return result;
  }

  auto& request = registry->requests_by_uuid[matched_keys.front()];
  if (request.session_uuid != actor.session_uuid && !authorization_proven) {
    result.error = true;
    result.outcome = "authorization_required";
    result.records_json = "[" + RequestLifecycleRecordJson(request) + "]";
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "SECURITY.AUTHORIZATION.DENIED",
        ServerDiagnosticSeverity::kError,
        "Cancelling another session request requires engine authorization.",
        {{"target_request_uuid", UuidBytesToText(request.request_uuid)}}));
    return result;
  }

  bool any_unknown_outcome = false;
  for (const auto& key : matched_keys) {
    auto& matched = registry->requests_by_uuid[key];
    if (matched.session_uuid != actor.session_uuid && !authorization_proven) continue;
    matched.cancel_timeout_ms = cancel_timeout_ms == 0 ? matched.cancel_timeout_ms : cancel_timeout_ms;
    const bool unknown_outcome = CancellationOutcomeUnknown(matched, actor);
    any_unknown_outcome = any_unknown_outcome || unknown_outcome;
    matched.state = unknown_outcome ? ServerRequestLifecycleState::kUnknownOutcome
                                    : ServerRequestLifecycleState::kCancelled;
    matched.detail = unknown_outcome
                         ? "cancel_requested_outcome_unknown_preserved"
                         : "cancel_requested_completed";
    matched.transaction_finality_preserved = true;
    matched.authorization_proven = authorization_proven || matched.session_uuid == actor.session_uuid;
    UpsertRequestFinality(registry, matched);
  }

  request = registry->requests_by_uuid[matched_keys.front()];
  if (!sbps::IsZeroUuid(request.cursor_uuid)) {
    auto cursor_it = registry->cursors_by_uuid.find(UuidBytesToText(request.cursor_uuid));
    if (cursor_it != registry->cursors_by_uuid.end()) {
      auto& cursor = cursor_it->second;
      if (cursor.engine_result != nullptr) {
        (void)sb_engine_result_release(cursor.engine_result);
        cursor.engine_result = nullptr;
      }
      cursor.finality_state = any_unknown_outcome ? "cancelled_unknown_outcome" : "cancelled";
      cursor.finality_reason = any_unknown_outcome
                                   ? "cancel_requested_outcome_unknown_preserved"
                                   : "cancel_requested_completed";
      cursor.exhausted = true;
      cursor.closed = true;
    }
  }
  result.accepted = true;
  result.unknown_outcome = any_unknown_outcome;
  result.outcome = ServerRequestLifecycleStateName(request.state);
  result.record = request;
  result.records_json = ServerRequestLifecycleRecordsJson(*registry, target_uuid, true);
  if (any_unknown_outcome) {
    result.diagnostics.push_back(RequestLifecycleDiagnostic(
        "PARSER_SERVER_IPC.DISCONNECT_OUTCOME_UNKNOWN",
        ServerDiagnosticSeverity::kWarning,
        "Request cancellation preserved unknown transaction or engine result outcome under MGA authority.",
        {{"request_uuid", UuidBytesToText(request.request_uuid)},
         {"finality_token_uuid", UuidBytesToText(request.finality_token_uuid)},
         {"mga_finality_authority", "engine"}}));
  }
  return result;
}

void MarkServerRequestTimedOutByCursor(ServerSessionRegistry* registry,
                                       const std::array<std::uint8_t, 16>& cursor_uuid,
                                       std::string detail) {
  if (registry == nullptr) return;
  for (auto& [_, request] : registry->requests_by_uuid) {
    if (request.cursor_uuid != cursor_uuid) continue;
    if (TerminalRequestState(request.state) &&
        request.state != ServerRequestLifecycleState::kCompleted) {
      continue;
    }
    const bool transaction_outcome_risk =
        request.local_transaction_id_at_start != 0 &&
        !OperationCancellationCanBeDeterministic(request.operation_id);
    const bool unknown_outcome = transaction_outcome_risk ||
                                 request.engine_result_retained;
    request.state = unknown_outcome ? ServerRequestLifecycleState::kUnknownOutcome
                                    : ServerRequestLifecycleState::kTimedOut;
    request.detail = unknown_outcome
                         ? detail + "_outcome_unknown_preserved"
                         : detail;
    request.transaction_finality_preserved = true;
    UpsertRequestFinality(registry, request);
  }
}

void MarkServerRequestClosedByCursor(ServerSessionRegistry* registry,
                                     const std::array<std::uint8_t, 16>& cursor_uuid,
                                     ServerRequestLifecycleState state,
                                     std::string detail) {
  if (registry == nullptr) return;
  for (auto& [_, request] : registry->requests_by_uuid) {
    if (request.cursor_uuid != cursor_uuid) continue;
    if (TerminalRequestState(request.state) &&
        request.state != ServerRequestLifecycleState::kCompleted) {
      continue;
    }
    request.state = state;
    request.detail = detail;
    request.transaction_finality_preserved = true;
    UpsertRequestFinality(registry, request);
  }
}

SessionOperationResult HandleAuthHandoff(ServerSessionRegistry* registry,
                                         const HostedEngineState& engine_state,
                                         const sbps::Frame& request) {
  registry->channel_state = ServerChannelState::kAuthPending;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAuthResult);
  result.response_schema_id = kSchemaAuthResultTestV1;
  const auto decoded = DecodeAuthPayload(request.payload);
  if (!decoded) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_HANDOFF_INVALID",
                                                "The authentication handoff payload is malformed."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "malformed_auth_payload");
    result.payload = EncodeAuthResultPayload("error", nullptr, "malformed_auth_payload");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "malformed_auth_payload");
    return result;
  }
  const HostedDatabaseSnapshot* database = FirstOpenDatabase(engine_state);
  if (database == nullptr) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_DATABASE_UNAVAILABLE",
                                                "No hosted database is available for authentication."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "database_unavailable");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "database_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "database_unavailable");
    return result;
  }
  if (!RequestedDatabaseMatches(*database, decoded->requested_database)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_DATABASE_MISMATCH",
                                                "Authentication requested a database that is not associated with this server route."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "database_route_mismatch");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "database_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "database_route_mismatch");
    return result;
  }
  if (!ConnectionHeaderMatchesPayload(request, decoded->connection_uuid)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                                                "Authentication route association did not match the parser connection."));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "connection_route_mismatch");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "connection_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "connection_route_mismatch");
    return result;
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                                "Standalone authentication cannot proceed through cluster lifecycle authority.",
                                                "cluster_authority_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "cluster_authority_unavailable");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "cluster_authority_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "cluster_authority_unavailable");
    return result;
  }
  const auto lifecycle = ConfigPolicySecurityLifecycleFromDatabase(*database);
  const auto lifecycle_admission = ValidateConfigPolicySecurityAdmission(
      lifecycle,
      lifecycle.capability_policy_generation,
      lifecycle.policy_generation,
      lifecycle.security_epoch,
      lifecycle.provider_generation,
      decoded->provider_family.empty() ? "local_password" : decoded->provider_family,
      "engine");
  if (!lifecycle_admission.ok()) {
    result.diagnostics.push_back(lifecycle_admission.diagnostic);
    AddAttachAdmissionDenied(&result.diagnostics,
                             "auth_handoff",
                             lifecycle_admission.diagnostic.fields.empty()
                                 ? lifecycle_admission.diagnostic.code
                                 : lifecycle_admission.diagnostic.fields.front().value);
    result.payload =
        EncodeAuthResultPayload("rejected", nullptr, lifecycle_admission.diagnostic.code);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   {},
                   "auth_handoff",
                   "rejected",
                   lifecycle_admission.diagnostic.code);
    return result;
  }
  engine_api::EngineAuthenticateRequest auth_request;
  auth_request.context = EngineContextBase(engine_state, request, decoded->requested_language);
  auth_request.context.security_epoch = lifecycle.security_epoch;
  auth_request.context.catalog_generation_id = lifecycle.policy_generation;
  auth_request.provider_family = decoded->provider_family.empty() ? "local_password" : decoded->provider_family;
  auth_request.principal_claim = decoded->principal_claim;
  auth_request.credential_evidence = decoded->credential_evidence;
  auth_request.credential_evidence_present =
      decoded->credential_evidence_present || !decoded->credential_evidence.empty();
  auth_request.credential_invalid_claim = decoded->credential_invalid;
  auth_request.mfa_evidence_present = decoded->mfa_evidence_present;
  auth_request.target_database.uuid.canonical = auth_request.context.database_uuid.canonical;
  auth_request.target_database.object_kind = "database";
  auth_request.target_object.uuid.canonical = auth_request.context.database_uuid.canonical;
  auth_request.target_object.object_kind = "security_authority";
  auth_request.option_envelopes.push_back("provider:" + auth_request.provider_family);
  auth_request.option_envelopes.push_back("principal:" + auth_request.principal_claim);
  auth_request.option_envelopes.push_back("auth_authority:engine");
  auth_request.option_envelopes.push_back(
      "policy_generation_current:" + std::to_string(lifecycle.policy_generation));
  auth_request.option_envelopes.push_back(
      "policy_generation_observed:" + std::to_string(lifecycle.policy_generation));
  auth_request.option_envelopes.push_back(
      "security_epoch_current:" + std::to_string(lifecycle.security_epoch));
  auth_request.option_envelopes.push_back(
      "security_epoch_observed:" + std::to_string(lifecycle.security_epoch));
  auth_request.option_envelopes.push_back(
      "provider_generation_current:" + std::to_string(lifecycle.provider_generation));
  auth_request.option_envelopes.push_back(
      "provider_generation_observed:" + std::to_string(lifecycle.provider_generation));
  auth_request.option_envelopes.push_back(
      "provider_lifecycle_state:" + std::string(SecurityProviderLifecycleStateName(
                                      lifecycle.provider_state)));
  auth_request.option_envelopes.push_back(
      lifecycle.default_policy_installed ? "default_policy_installed:true"
                                         : "default_policy_installed:false");
  auth_request.option_envelopes.push_back(
      "cache_invalidation_epoch:" + std::to_string(lifecycle.cache_invalidation_epoch));
  if (auth_request.credential_evidence_present) {
    auth_request.option_envelopes.push_back("credential_evidence_present:true");
  }
  if (decoded->credential_invalid) {
    auth_request.option_envelopes.push_back("credential_transport_flag:invalid");
  }
  if (decoded->mfa_required) {
    auth_request.option_envelopes.push_back("mfa_required:true");
  }
  if (decoded->mfa_evidence_present) {
    auth_request.option_envelopes.push_back("mfa_evidence_present:true");
    auth_request.option_envelopes.push_back("mfa:present");
  }
  const auto tls_denial = TlsTransportDenialFromEvidence(*decoded);
  if (tls_denial.denied) {
    auth_request.credential_invalid_claim = true;
    auth_request.option_envelopes.push_back("credential:invalid");
    auth_request.option_envelopes.push_back("tls_transport_denial:" + tls_denial.detail);
    auth_request.option_envelopes.push_back("transport_security_evidence:present");
  }
  const auto auth_result = engine_api::EngineAuthenticate(auth_request);
  if (!auth_result.ok || !auth_result.authenticated) {
    const auto detail = tls_denial.denied
                            ? tls_denial.detail
                            : EngineDiagnosticDetail(auth_result.diagnostics, "credential_rejected");
    if (detail == "mfa_evidence_required") {
      result.diagnostics.push_back(AuthDiagnostic("SECURITY.AUTHENTICATION.CHALLENGE_REQUIRED",
                                                  "Additional authentication evidence is required.",
                                                  detail));
      AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", detail);
      result.payload = EncodeAuthResultPayload("challenge_required", nullptr, detail);
      result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
      registry->channel_state = ServerChannelState::kAuthPending;
      result.accepted = false;
      RecordFinality(registry, request, {}, {}, "auth_handoff", "challenge_required", detail);
      return result;
    }
    result.diagnostics.push_back(AuthDiagnostic(
        tls_denial.denied
            ? tls_denial.code
            : EngineDiagnosticCode(auth_result.diagnostics, "SECURITY.AUTHENTICATION.FAILED"),
        tls_denial.denied
            ? "TLS transport evidence was denied by engine authentication."
            : "Authentication failed.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", detail);
    result.payload = EncodeAuthResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", detail);
    return result;
  }

  ServerSessionRecord session;
  session.connection_uuid = sbps::IsZeroUuid(request.header.connection_uuid)
                                ? decoded->connection_uuid
                                : request.header.connection_uuid;
  session.auth_context_uuid =
      TextToUuid(auth_result.connection_security_context.connection_uuid.canonical);
  session.session_uuid = session.auth_context_uuid;
  session.principal_uuid =
      TextToUuid(auth_result.connection_security_context.effective_user_uuid.canonical);
  session.effective_user_uuid = session.principal_uuid;
  if (sbps::IsZeroUuid(session.auth_context_uuid) || sbps::IsZeroUuid(session.principal_uuid)) {
    result.diagnostics.push_back(AuthDiagnostic("SECURITY.AUTHENTICATION.RESULT_INVALID",
                                                "The engine authentication result did not include a valid session or principal UUID.",
                                                "engine_identity_invalid"));
    AddAttachAdmissionDenied(&result.diagnostics, "auth_handoff", "engine_identity_invalid");
    result.payload = EncodeAuthResultPayload("rejected", nullptr, "engine_identity_invalid");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "auth_handoff", "rejected", "engine_identity_invalid");
    return result;
  }
  session.principal_claim = decoded->principal_claim;
  session.application_name = decoded->application_name;
  session.provider_family = auth_request.provider_family;
  session.database_path = FirstOpenDatabasePath(engine_state);
  session.database_uuid = FirstOpenDatabaseUuid(engine_state);
  ApplyDatabaseHealthToSession(&session, *database);
  session.security_epoch = auth_result.connection_security_context.security_epoch == 0
                               ? session.security_epoch
                               : std::max(session.security_epoch,
                                          auth_result.connection_security_context.security_epoch);
  session.policy_generation = auth_result.connection_security_context.policy_epoch == 0
                                  ? session.policy_generation
                                  : std::max(session.policy_generation,
                                             auth_result.connection_security_context.policy_epoch);
  session.engine_authorization_trace_tags =
      auth_result.connection_security_context.authorization_trace_tags;
  registry->auth_contexts_by_uuid[AuthContextKey(session.auth_context_uuid)] = session;
  result.session_uuid = session.session_uuid;
  result.payload = EncodeAuthResultPayload("accepted", &session);
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.accepted = true;
  registry->channel_state = ServerChannelState::kAttachPending;
  RecordFinality(registry,
                 request,
                 session.session_uuid,
                 session.auth_context_uuid,
                 "auth_handoff",
                 "accepted");
  return result;
}

SessionOperationResult HandleAttachDatabase(ServerSessionRegistry* registry,
                                            const HostedEngineState& engine_state,
                                            const sbps::Frame& request) {
  registry->channel_state = ServerChannelState::kAttachPending;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAttachResult);
  result.response_schema_id = kSchemaAttachResultTestV1;
  const auto decoded = DecodeAttachPayload(request.payload);
  if (!decoded || sbps::IsZeroUuid(decoded->auth_context_uuid)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.AUTH_HANDOFF_REQUIRED",
                                                "Database attach requires an accepted authentication context."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "auth_context_required");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "auth_context_required");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry, request, {}, {}, "attach_database", "rejected", "auth_context_required");
    return result;
  }
  auto found = registry->auth_contexts_by_uuid.find(AuthContextKey(decoded->auth_context_uuid));
  if (found == registry->auth_contexts_by_uuid.end()) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.SESSION_NOT_BOUND",
                                                "The authentication context is unknown or expired."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "auth_context_unknown");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "auth_context_unknown");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "auth_context_unknown");
    return result;
  }
  if (registry->sessions_by_uuid.find(UuidBytesToText(found->second.session_uuid)) !=
      registry->sessions_by_uuid.end()) {
    result.diagnostics.push_back(AuthDiagnostic(
        "PARSER_SERVER_IPC.AUTH_CONTEXT_REPLAY_REFUSED",
        "The authentication context has already been consumed by an attached session.",
        "auth_context_replay_refused"));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "auth_context_replay_refused");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "auth_context_replay_refused");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   found->second.session_uuid,
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "auth_context_replay_refused");
    return result;
  }
  const HostedDatabaseSnapshot* database = FirstOpenDatabase(engine_state);
  if (database == nullptr) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_UNAVAILABLE",
                                                "No hosted database is available for attach."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "database_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "database_unavailable");
    return result;
  }
  auto session = found->second;
  if (!ConnectionHeaderMatchesSession(request, session)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH",
                                                "Attach route association did not match the authenticated parser connection."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "connection_route_mismatch");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "connection_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "connection_route_mismatch");
    return result;
  }
  if (!RequestedDatabaseMatches(*database, decoded->requested_database) ||
      !AuthContextMatchesHostedDatabase(session, *database)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_MISMATCH",
                                                "Attach requested a database that is not associated with this authenticated route."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "database_route_mismatch");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   {},
                   decoded->auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "database_route_mismatch");
    return result;
  }
  const std::string attach_mode = CanonicalAttachMode(decoded->requested_attachment_mode);
  if (!AttachmentModeSupported(attach_mode)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_MODE_UNSUPPORTED",
                                                "The requested database attachment mode is not supported."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "attachment_mode_unsupported");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "attachment_mode_unsupported");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "attachment_mode_unsupported");
    return result;
  }
  if ((database->read_only || database->state == HostedDatabaseState::kReadOnly) &&
      attach_mode == "read_write") {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_MODE_DENIED",
                                                "The hosted database admits only read-only attachments."));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "read_only_database");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "read_only_database");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "read_only_database");
    return result;
  }
  if (database->state == HostedDatabaseState::kMaintenance ||
      database->state == HostedDatabaseState::kRestrictedOpen ||
      database->state == HostedDatabaseState::kQuarantined ||
      database->state == HostedDatabaseState::kFailed ||
      database->state == HostedDatabaseState::kDetached) {
    const std::string detail = database->state == HostedDatabaseState::kMaintenance
        ? "maintenance_admission_required"
        : database->state == HostedDatabaseState::kRestrictedOpen
            ? "restricted_open_admission_required"
            : "database_lifecycle_state_not_attachable";
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
                                                "The hosted database lifecycle state does not admit ordinary attachments.",
                                                detail));
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   detail);
    return result;
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                                "Standalone attach cannot proceed through cluster lifecycle authority.",
                                                "cluster_authority_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", "cluster_authority_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "cluster_authority_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   "cluster_authority_unavailable");
    return result;
  }
  const auto lifecycle = ConfigPolicySecurityLifecycleFromDatabase(*database);
  const auto lifecycle_admission = ValidateConfigPolicySecurityAdmission(
      lifecycle,
      session.capability_policy_generation,
      session.policy_generation,
      session.security_epoch,
      session.security_provider_generation,
      session.provider_family.empty() ? "local_password" : session.provider_family,
      "engine");
  if (!lifecycle_admission.ok()) {
    result.diagnostics.push_back(lifecycle_admission.diagnostic);
    AddAttachAdmissionDenied(&result.diagnostics,
                             "attach_database",
                             lifecycle_admission.diagnostic.code);
    result.payload =
        EncodeAttachResultPayload("rejected", nullptr, lifecycle_admission.diagnostic.code);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   lifecycle_admission.diagnostic.code);
    return result;
  }
  session.attach_mode = attach_mode;
  session.database_path = database->database_path;
  session.database_uuid = database->database_uuid;
  ApplyDatabaseHealthToSession(&session, *database);
  engine_api::EngineAuthorizeRequest authorize;
  authorize.context = EngineContextForSession(session, engine_state, request);
  authorize.required_right = "CONNECT";
  authorize.target_database.uuid.canonical = session.database_uuid;
  authorize.target_database.object_kind = "database";
  authorize.target_object.uuid.canonical = session.database_uuid;
  authorize.target_object.object_kind = "database";
  const auto authz_result = engine_api::EngineAuthorize(authorize);
  if (!authz_result.ok || !authz_result.authorized) {
    const auto detail = EngineDiagnosticDetail(authz_result.diagnostics, "connect_denied");
    result.diagnostics.push_back(AuthDiagnostic(
        EngineDiagnosticCode(authz_result.diagnostics, "SECURITY.AUTHORIZATION.DENIED"),
        "Database attach was not authorized by the engine.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                 detail);
    return result;
  }
  std::string transaction_diagnostic_code;
  std::string transaction_diagnostic_detail;
  if (!StartAlwaysActiveTransactionForSession(&session,
                                              engine_state,
                                              request,
                                              &transaction_diagnostic_code,
                                              &transaction_diagnostic_detail)) {
    const std::string detail = transaction_diagnostic_detail.empty()
                                   ? "transaction_begin_failed"
                                   : transaction_diagnostic_detail;
    result.diagnostics.push_back(AuthDiagnostic(
        transaction_diagnostic_code.empty() ? "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"
                                            : transaction_diagnostic_code,
        "Database attach could not create the required active transaction.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "attach_database", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    RecordFinality(registry,
                   request,
                   session.session_uuid,
                   session.auth_context_uuid,
                   "attach_database",
                   "rejected",
                   detail);
    return result;
  }
  registry->sessions_by_uuid[UuidBytesToText(session.session_uuid)] = session;
  registry->channel_state = ServerChannelState::kReady;
  result.accepted = true;
  result.session_uuid = session.session_uuid;
  result.payload = EncodeAttachResultPayload("accepted", &session);
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  RecordFinality(registry,
                 request,
                 session.session_uuid,
                 session.auth_context_uuid,
                 "attach_database",
                 "accepted");
  return result;
}

SessionOperationResult HandleEmbeddedSysarchAttach(ServerSessionRegistry* registry,
                                                   const HostedEngineState& engine_state,
                                                   std::string requested_database,
                                                   std::string application_name) {
  if (registry == nullptr) {
    SessionOperationResult result;
    result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAttachResult);
    result.response_schema_id = kSchemaAttachResultTestV1;
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.SESSION_REGISTRY_REQUIRED",
                                                "Embedded attach requires a session registry.",
                                                "session_registry_required"));
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "session_registry_required");
    return result;
  }
  registry->channel_state = ServerChannelState::kAttachPending;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kAttachResult);
  result.response_schema_id = kSchemaAttachResultTestV1;
  const HostedDatabaseSnapshot* database = FirstOpenDatabase(engine_state);
  if (database == nullptr) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_UNAVAILABLE",
                                                "No hosted database is available for embedded attach.",
                                                "database_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", "database_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }
  if (!RequestedDatabaseMatches(*database, requested_database)) {
    result.diagnostics.push_back(AuthDiagnostic("PARSER_SERVER_IPC.ATTACH_DATABASE_MISMATCH",
                                                "Embedded attach requested a different database.",
                                                "database_route_mismatch"));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", "database_route_mismatch");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "database_route_mismatch");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }
  if (database->cluster_structures_present || database->cluster_authority_required) {
    result.diagnostics.push_back(AuthDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                                "Embedded attach cannot proceed through cluster lifecycle authority.",
                                                "cluster_authority_unavailable"));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", "cluster_authority_unavailable");
    result.payload = EncodeAttachResultPayload("rejected", nullptr, "cluster_authority_unavailable");
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }

  ServerSessionRecord session;
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = PrincipalUuidFor("sysarch");
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "sysarch";
  session.provider_family = "embedded_sysarch";
  session.application_name = application_name.empty() ? "sb_isql" : std::move(application_name);
  session.database_path = database->database_path;
  session.database_uuid = database->database_uuid;
  session.attach_mode = "read_write";
  session.embedded_in_process = true;
  session.engine_authorization_trace_tags = {"embedded_sysarch", "sb_isql"};
  ApplyDatabaseHealthToSession(&session, *database);

  sbps::Frame attach_frame;
  attach_frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  attach_frame.header.connection_uuid = session.connection_uuid;
  attach_frame.header.session_uuid = session.session_uuid;
  std::string transaction_diagnostic_code;
  std::string transaction_diagnostic_detail;
  if (!StartAlwaysActiveTransactionForSession(&session,
                                              engine_state,
                                              attach_frame,
                                              &transaction_diagnostic_code,
                                              &transaction_diagnostic_detail)) {
    const std::string detail = transaction_diagnostic_detail.empty()
                                   ? "transaction_begin_failed"
                                   : transaction_diagnostic_detail;
    result.diagnostics.push_back(AuthDiagnostic(
        transaction_diagnostic_code.empty() ? "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"
                                            : transaction_diagnostic_code,
        "Embedded attach could not create the required active transaction.",
        detail));
    AddAttachAdmissionDenied(&result.diagnostics, "embedded_attach", detail);
    result.payload = EncodeAttachResultPayload("rejected", nullptr, detail);
    result.frame_flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
    registry->channel_state = ServerChannelState::kFailed;
    return result;
  }

  registry->sessions_by_uuid[UuidBytesToText(session.session_uuid)] = session;
  registry->auth_contexts_by_uuid[AuthContextKey(session.auth_context_uuid)] = session;
  registry->channel_state = ServerChannelState::kReady;
  result.accepted = true;
  result.session_uuid = session.session_uuid;
  result.payload = EncodeAttachResultPayload("accepted", &session);
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  return result;
}

SessionOperationResult HandleDisconnectNotice(ServerSessionRegistry* registry,
                                              const sbps::Frame& request) {
  registry->channel_state = ServerChannelState::kDraining;
  SessionOperationResult result;
  result.response_message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  result.response_schema_id = kSchemaDisconnectResultTestV1;
  std::array<std::uint8_t, 16> session_uuid = request.header.session_uuid;
  if (sbps::IsZeroUuid(session_uuid) && request.payload.size() >= 16) {
    session_uuid = GetUuid(request.payload, 0);
  }
  std::string disconnect_reason = "parser_disconnect_notice";
  if (request.payload.size() > 16) {
    std::size_t offset = 16;
    (void)ReadString(request.payload, &offset, &disconnect_reason);
    if (disconnect_reason.empty()) disconnect_reason = "parser_disconnect_notice";
  }
  const auto key = UuidBytesToText(session_uuid);
  const auto session_found = registry->sessions_by_uuid.find(key);
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::uint64_t active_local_transaction_id = 0;
  std::uint64_t rolled_back_local_transaction_id = 0;
  if (session_found != registry->sessions_by_uuid.end()) {
    auth_context_uuid = session_found->second.auth_context_uuid;
    active_local_transaction_id = session_found->second.local_transaction_id;
    if (active_local_transaction_id != 0 && disconnect_reason != "parser_killed") {
      engine_api::EngineRollbackTransactionRequest rollback;
      rollback.context = EngineContextForSession(session_found->second,
                                                 HostedEngineState{},
                                                 request);
      const auto rolled_back = engine_api::EngineRollbackTransaction(rollback);
      if (rolled_back.ok) {
        rolled_back_local_transaction_id = active_local_transaction_id;
        active_local_transaction_id = 0;
      }
    }
  }
  const auto erased = registry->sessions_by_uuid.erase(key);
  std::uint64_t auth_contexts_removed = 0;
  if (erased != 0) {
    auth_contexts_removed += registry->auth_contexts_by_uuid.erase(AuthContextKey(auth_context_uuid));
    for (auto it = registry->auth_contexts_by_uuid.begin();
         it != registry->auth_contexts_by_uuid.end();) {
      if (it->second.session_uuid == session_uuid) {
        it = registry->auth_contexts_by_uuid.erase(it);
        ++auth_contexts_removed;
      } else {
        ++it;
      }
    }
  }
  std::uint64_t prepared_tombstoned = 0;
  for (auto& [_, prepared] : registry->prepared_by_uuid) {
    if (prepared.session_uuid == session_uuid) {
      if (!prepared.closed) ++prepared_tombstoned;
      prepared.closed = true;
    }
  }
  std::uint64_t cursors_tombstoned = 0;
  std::uint64_t engine_results_released = 0;
  std::uint64_t request_finality_records_updated = 0;
  for (auto& [_, cursor] : registry->cursors_by_uuid) {
    if (cursor.session_uuid == session_uuid) {
      const bool had_engine_result = cursor.engine_result != nullptr;
      if (cursor.engine_result != nullptr) {
        (void)sb_engine_result_release(cursor.engine_result);
        cursor.engine_result = nullptr;
        ++engine_results_released;
      }
      if (!cursor.closed) ++cursors_tombstoned;
      cursor.finality_state = disconnect_reason == "parser_killed" ? "parser_killed" : "parser_disconnected";
      cursor.finality_reason = disconnect_reason;
      cursor.exhausted = true;
      cursor.closed = true;
      if (!sbps::IsZeroUuid(cursor.request_uuid)) {
        auto request_it = registry->requests_by_uuid.find(UuidBytesToText(cursor.request_uuid));
        if (request_it != registry->requests_by_uuid.end() &&
            !TerminalRequestState(request_it->second.state)) {
          const bool unknown_outcome = active_local_transaction_id != 0 || had_engine_result ||
                                       request_it->second.engine_result_retained;
          request_it->second.state = unknown_outcome
                                         ? ServerRequestLifecycleState::kUnknownOutcome
                                         : ServerRequestLifecycleState::kDisconnected;
          request_it->second.detail = unknown_outcome
                                          ? "parser_disconnect_outcome_unknown_preserved"
                                          : "parser_disconnect_resource_closed";
          request_it->second.transaction_finality_preserved = true;
          UpsertRequestFinality(registry, request_it->second);
          ++request_finality_records_updated;
        }
      }
    }
  }
  for (auto& [_, request_record] : registry->requests_by_uuid) {
    if (request_record.session_uuid != session_uuid ||
        TerminalRequestState(request_record.state)) {
      continue;
    }
    const bool unknown_outcome = active_local_transaction_id != 0 ||
                                 request_record.engine_result_retained;
    request_record.state = unknown_outcome ? ServerRequestLifecycleState::kUnknownOutcome
                                           : ServerRequestLifecycleState::kDisconnected;
    request_record.detail = unknown_outcome
                                ? "parser_disconnect_outcome_unknown_preserved"
                                : "parser_disconnect_resource_closed";
    request_record.transaction_finality_preserved = true;
    UpsertRequestFinality(registry, request_record);
    ++request_finality_records_updated;
  }
  const auto cleanup_detail = DetachCleanupDetail(disconnect_reason,
                                                  erased,
                                                  auth_contexts_removed,
                                                  prepared_tombstoned,
                                                  cursors_tombstoned,
                                                  engine_results_released,
                                                  request_finality_records_updated,
                                                  active_local_transaction_id);
  if (erased != 0) {
    result.diagnostics.push_back(DetachCleanupDiagnostic(
        "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE",
        ServerDiagnosticSeverity::kInfo,
        "Detach cleanup released session-scoped runtime resources deterministically.",
        {{"detail", cleanup_detail},
         {"session_uuid", key},
         {"auth_contexts_removed", std::to_string(auth_contexts_removed)},
         {"prepared_tombstoned", std::to_string(prepared_tombstoned)},
         {"cursors_tombstoned", std::to_string(cursors_tombstoned)},
         {"engine_results_released", std::to_string(engine_results_released)},
         {"request_finality_records_updated", std::to_string(request_finality_records_updated)}}));
    if (rolled_back_local_transaction_id != 0) {
      result.diagnostics.push_back(DetachCleanupDiagnostic(
          "ENGINE.DBLC_DETACH_TRANSACTION_ROLLED_BACK",
          ServerDiagnosticSeverity::kInfo,
          "Orderly parser disconnect rolled back the active MGA transaction before detaching the session.",
          {{"detail", cleanup_detail},
           {"local_transaction_id", std::to_string(rolled_back_local_transaction_id)},
           {"mga_finality_authority", "engine"}}));
    }
    if (active_local_transaction_id != 0) {
      result.diagnostics.push_back(DetachCleanupDiagnostic(
          "ENGINE.DBLC_TRANSACTION_OUTCOME_UNKNOWN",
          ServerDiagnosticSeverity::kWarning,
          "Parser disconnect detached the session but did not commit or roll back the active MGA transaction.",
          {{"detail", cleanup_detail},
           {"local_transaction_id", std::to_string(active_local_transaction_id)},
           {"mga_finality_authority", "engine"}}));
    }
  }
  std::vector<std::uint8_t> out;
  PutString(&out, erased == 0 ? "session_not_found" : "detached");
  PutUuid(&out, session_uuid);
  PutString(&out, cleanup_detail);
  result.payload = std::move(out);
  result.session_uuid = session_uuid;
  result.frame_flags = sbps::kFlagResponse | sbps::kFlagFinal;
  result.accepted = erased != 0;
  registry->channel_state = erased == 0 ? ServerChannelState::kFailed : ServerChannelState::kDetached;
  RecordFinality(registry,
                 request,
                 session_uuid,
                 auth_context_uuid,
                 "disconnect_notice",
                 erased == 0 ? "session_not_found" : "detached",
                 cleanup_detail);
  return result;
}

ServerSessionBindingControlResult ApplyServerSessionBindingReport(
    ServerSessionRegistry* registry,
    const ServerSessionBindingReport& report,
    const ServerSessionControlAuthority& authority) {
  if (registry == nullptr) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REGISTRY_REQUIRED",
                                  "session_registry_required",
                                  "Session binding requires a server session registry.");
  }
  if (!SessionControlAuthorized(authority, authority.may_report_binding)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "binding_report_authority_required",
                                  "SESSION_BINDING_REPORT requires authenticated session-control authority.",
                                  {{"operation", "session_binding_report"}});
  }
  if (IsZeroUuidBytes(report.catalog_session_id)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.TARGET_REQUIRED",
                                  "catalog_session_id_required",
                                  "SESSION_BINDING_REPORT requires a catalog session id.");
  }
  auto session_it = FindMutableSessionByBindingTarget(registry,
                                                      report.catalog_session_id,
                                                      report.protocol_session_id);
  if (session_it == registry->sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_BINDING.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "SESSION_BINDING_REPORT target session is not active.",
                                  {{"catalog_session_id", UuidBytesToText(report.catalog_session_id)}});
  }
  auto& session = session_it->second;
  if (authority.sequence <= session.session_binding_control_sequence) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REPLAY_REFUSED",
                                  "binding_sequence_replay",
                                  "SESSION_BINDING_REPORT sequence was already consumed.",
                                  {{"sequence", std::to_string(authority.sequence)},
                                   {"last_sequence", std::to_string(session.session_binding_control_sequence)}});
  }
  if (!UuidMatchesIfPresent(report.attachment_id, session.connection_uuid)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.ROUTE_MISMATCH",
                                  "attachment_id_mismatch",
                                  "SESSION_BINDING_REPORT attachment did not match the server session.",
                                  {{"session_uuid", session_it->first}});
  }
  if (!UuidMatchesIfPresent(report.authenticated_principal_id, session.principal_uuid)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.PRINCIPAL_MISMATCH",
                                  "authenticated_principal_id_mismatch",
                                  "SESSION_BINDING_REPORT principal did not match the server session.",
                                  {{"session_uuid", session_it->first}});
  }
  if (!UuidMatchesIfPresent(report.session_user_id, session.effective_user_uuid)) {
    return SessionControlRejected("SERVER.SESSION_BINDING.USER_MISMATCH",
                                  "session_user_id_mismatch",
                                  "SESSION_BINDING_REPORT user did not match the server session.",
                                  {{"session_uuid", session_it->first}});
  }
  if (report.current_txn_id != session.local_transaction_id) {
    return SessionControlRejected("SERVER.SESSION_BINDING.TRANSACTION_MISMATCH",
                                  "current_txn_id_mismatch",
                                  "SESSION_BINDING_REPORT transaction id did not match the engine-owned session transaction.",
                                  {{"session_uuid", session_it->first},
                                   {"reported_txn_id", std::to_string(report.current_txn_id)},
                                   {"server_txn_id", std::to_string(session.local_transaction_id)}});
  }

  session.session_binding_present = true;
  session.attachment_id = report.attachment_id;
  session.catalog_session_id = report.catalog_session_id;
  session.protocol_session_id = report.protocol_session_id;
  session.authkey_id = report.authkey_id;
  session.active_role_uuid = report.active_role_id;
  session.effective_group_uuids = report.effective_group_ids;
  if (!IsZeroUuidBytes(report.transaction_uuid)) {
    session.transaction_uuid = UuidBytesToText(report.transaction_uuid);
  }
  session.session_binding_generation += 1;
  session.session_binding_control_sequence = authority.sequence;
  session.session_binding_authority_class = authority.authority_class;
  session.session_binding_actor_token = authority.actor_token;
  registry->channel_state = ServerChannelState::kSessionBound;
  MirrorAuthContext(registry, session);

  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.mutated = true;
  result.target_session_uuid = session_it->first;
  result.detail = "session_binding_report_applied";
  return result;
}

ServerSessionBindingControlResult ClearServerSessionBinding(
    ServerSessionRegistry* registry,
    const ServerSessionTakeoverRequest& target,
    const ServerSessionControlAuthority& authority) {
  if (registry == nullptr) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REGISTRY_REQUIRED",
                                  "session_registry_required",
                                  "Session binding clear requires a server session registry.");
  }
  if (!SessionControlAuthorized(authority, authority.may_clear_binding)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "binding_clear_authority_required",
                                  "SESSION_BINDING_CLEAR requires authenticated session-control authority.",
                                  {{"operation", "session_binding_clear"}});
  }
  auto session_it = FindMutableSessionByBindingTarget(registry,
                                                      target.catalog_session_id,
                                                      target.protocol_session_id);
  if (session_it == registry->sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_BINDING.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "SESSION_BINDING_CLEAR target session is not active.");
  }
  auto& session = session_it->second;
  if (authority.sequence <= session.session_binding_control_sequence) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REPLAY_REFUSED",
                                  "binding_clear_sequence_replay",
                                  "SESSION_BINDING_CLEAR sequence was already consumed.",
                                  {{"sequence", std::to_string(authority.sequence)},
                                   {"last_sequence", std::to_string(session.session_binding_control_sequence)}});
  }
  session.session_binding_present = false;
  session.attachment_id = {};
  session.catalog_session_id = {};
  session.protocol_session_id = {};
  session.authkey_id = {};
  session.active_role_uuid = {};
  session.effective_group_uuids.clear();
  session.session_binding_generation += 1;
  session.session_binding_control_sequence = authority.sequence;
  session.session_binding_authority_class = authority.authority_class;
  session.session_binding_actor_token = authority.actor_token;
  registry->channel_state = ServerChannelState::kReady;
  MirrorAuthContext(registry, session);

  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.mutated = true;
  result.target_session_uuid = session_it->first;
  result.detail = "session_binding_cleared";
  return result;
}

ServerSessionBindingControlResult EvaluateServerSessionTakeoverProbe(
    const ServerSessionRegistry& registry,
    const ServerSessionTakeoverRequest& request,
    const ServerSessionControlAuthority& authority) {
  if (!SessionControlAuthorized(authority, authority.may_takeover)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "takeover_authority_required",
                                  "TAKEOVER_PROBE requires authenticated takeover authority.",
                                  {{"operation", "takeover_probe"}});
  }
  auto session_it = FindSessionByBindingTarget(registry,
                                               request.catalog_session_id,
                                               request.protocol_session_id);
  if (session_it == registry.sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "TAKEOVER_PROBE target session is not active.");
  }
  std::string detail;
  const bool claims_match = TakeoverClaimsMatch(session_it->second, request, &detail);
  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.target_session_uuid = session_it->first;
  result.detail = detail;
  if (session_it->second.session_binding_present) {
    result.probe_flags |= kServerTakeoverProbeSessionBound;
  }
  result.probe_flags |= kServerTakeoverProbeAuthorityAccepted;
  if (session_it->second.local_transaction_id != 0) {
    result.probe_flags |= kServerTakeoverProbeActiveTransaction;
  }
  if (claims_match) {
    result.probe_flags |= kServerTakeoverProbeTakeoverWouldPass;
    result.takeover_allowed = true;
  } else {
    result.diagnostic_code = "SERVER.SESSION_TAKEOVER.CLAIM_MISMATCH";
  }
  return result;
}

ServerSessionBindingControlResult ApplyServerSessionTakeoverRequest(
    ServerSessionRegistry* registry,
    const ServerSessionTakeoverRequest& request,
    const ServerSessionControlAuthority& authority) {
  if (registry == nullptr) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.REGISTRY_REQUIRED",
                                  "session_registry_required",
                                  "TAKEOVER_REQUEST requires a server session registry.");
  }
  if (!SessionControlAuthorized(authority, authority.may_takeover)) {
    return SessionControlRejected("SERVER.SESSION_CONTROL.AUTHORIZATION_DENIED",
                                  "takeover_authority_required",
                                  "TAKEOVER_REQUEST requires authenticated takeover authority.",
                                  {{"operation", "takeover_request"}});
  }
  auto session_it = FindMutableSessionByBindingTarget(registry,
                                                      request.catalog_session_id,
                                                      request.protocol_session_id);
  if (session_it == registry->sessions_by_uuid.end()) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.SESSION_NOT_FOUND",
                                  "session_not_found",
                                  "TAKEOVER_REQUEST target session is not active.");
  }
  auto& session = session_it->second;
  if (authority.sequence <= session.takeover_control_sequence) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.REPLAY_REFUSED",
                                  "takeover_sequence_replay",
                                  "TAKEOVER_REQUEST sequence was already consumed.",
                                  {{"sequence", std::to_string(authority.sequence)},
                                   {"last_sequence", std::to_string(session.takeover_control_sequence)}});
  }
  std::string detail;
  if (!TakeoverClaimsMatch(session, request, &detail)) {
    return SessionControlRejected("SERVER.SESSION_TAKEOVER.CLAIM_MISMATCH",
                                  detail,
                                  "TAKEOVER_REQUEST claims did not match the server-owned session binding.",
                                  {{"session_uuid", session_it->first}});
  }

  if ((request.mask & kServerTakeoverClaimAttachmentId) &&
      !IsZeroUuidBytes(request.attachment_id)) {
    session.attachment_id = request.attachment_id;
    session.connection_uuid = request.attachment_id;
  }
  if ((request.mask & kServerTakeoverClaimProtocolSessionId) &&
      !IsZeroUuidBytes(request.protocol_session_id)) {
    session.protocol_session_id = request.protocol_session_id;
  }
  session.takeover_generation += 1;
  session.takeover_control_sequence = authority.sequence;
  session.session_binding_authority_class = authority.authority_class;
  session.session_binding_actor_token = authority.actor_token;
  registry->channel_state = ServerChannelState::kSessionBound;
  MirrorAuthContext(registry, session);

  ServerSessionBindingControlResult result;
  result.accepted = true;
  result.mutated = true;
  result.takeover_allowed = true;
  result.target_session_uuid = session_it->first;
  result.detail = "takeover_accepted";
  return result;
}

std::string SessionRegistryStatusJson(const ServerSessionRegistry& registry) {
  std::ostringstream out;
  out << "{\"session_registry\":{\"channel_state\":\""
      << ServerChannelStateName(registry.channel_state) << "\",\"active_sessions\":"
      << registry.sessions_by_uuid.size() << ",\"auth_contexts\":"
      << registry.auth_contexts_by_uuid.size() << ",\"prepared_statements\":"
      << registry.prepared_by_uuid.size() << ",\"cursors\":"
      << registry.cursors_by_uuid.size() << ",\"requests\":"
      << registry.requests_by_uuid.size() << ",\"finality_records\":"
      << registry.finality_by_request_uuid.size() << ",\"sessions\":[";
  bool first = true;
  for (const auto& [_, session] : registry.sessions_by_uuid) {
    if (!first) out << ',';
    first = false;
    out << "{\"session_uuid\":\"" << UuidBytesToText(session.session_uuid)
        << "\",\"principal\":\"" << JsonEscape(session.principal_claim)
        << "\",\"database_path\":\"" << JsonEscape(session.database_path)
        << "\",\"attach_mode\":\"" << JsonEscape(session.attach_mode)
        << "\",\"session_binding_present\":"
        << (session.session_binding_present ? "true" : "false")
        << ",\"attachment_id\":\"" << UuidBytesToText(session.attachment_id)
        << "\",\"catalog_session_id\":\"" << UuidBytesToText(session.catalog_session_id)
        << "\",\"protocol_session_id\":\"" << UuidBytesToText(session.protocol_session_id)
        << "\",\"authkey_id\":\"" << UuidBytesToText(session.authkey_id)
        << "\",\"active_role_id\":\"" << UuidBytesToText(session.active_role_uuid)
        << "\",\"effective_group_count\":" << session.effective_group_uuids.size()
        << ",\"session_binding_generation\":" << session.session_binding_generation
        << ",\"session_binding_control_sequence\":"
        << session.session_binding_control_sequence
        << ",\"takeover_generation\":" << session.takeover_generation
        << ",\"takeover_control_sequence\":" << session.takeover_control_sequence
        << ",\"session_binding_authority_class\":\""
        << JsonEscape(session.session_binding_authority_class)
        << "\",\"session_binding_actor_token\":\""
        << JsonEscape(session.session_binding_actor_token)
        << "\",\"database_engine_agent_state\":\""
        << JsonEscape(session.database_engine_agent_state)
        << "\",\"database_engine_agent_health_generation\":"
        << session.database_engine_agent_health_generation
        << ",\"database_engine_agent_ordinary_admission_allowed\":"
        << (session.database_engine_agent_ordinary_admission_allowed ? "true" : "false")
        << ",\"database_engine_agent_health\":"
        << (session.database_engine_agent_health_json.empty()
                ? "{\"database_engine_agent\":{\"agent_state\":\"not_started\"}}"
                : session.database_engine_agent_health_json)
        << ",\"config_source_epoch\":" << session.config_source_epoch
        << ",\"config_reload_generation\":" << session.config_reload_generation
        << ",\"capability_policy_generation\":" << session.capability_policy_generation
        << ",\"policy_generation\":" << session.policy_generation
        << ",\"security_epoch\":" << session.security_epoch
        << ",\"security_provider_generation\":" << session.security_provider_generation
        << ",\"cache_invalidation_epoch\":" << session.cache_invalidation_epoch
        << ",\"config_policy_security_lifecycle\":"
        << (session.config_policy_security_lifecycle_json.empty()
                ? "{\"config_policy_security_lifecycle\":{\"present\":false}}"
                : session.config_policy_security_lifecycle_json)
        << "}";
  }
  out << "]}}\n";
  return out.str();
}

}  // namespace scratchbird::server
