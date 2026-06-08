// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_context_variables.hpp"

#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

SblrValue TextContextValue(std::string descriptor_id, std::string value_text) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.text_value = std::move(value_text);
  value.encoded_value = value.text_value;
  value.payload_kind = value.descriptor_id == "uuid" ? SblrValuePayloadKind::uuid_text :
                       value.descriptor_id == "timestamp_tz" ? SblrValuePayloadKind::temporal_text :
                       value.descriptor_id == "uint128" ? SblrValuePayloadKind::high_precision_numeric_text :
                       SblrValuePayloadKind::text;
  value.is_null = false;
  return value;
}

SblrValue NullContextValue(std::string descriptor_id) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

SblrValue BoolContextValue(bool bool_value) {
  SblrValue value;
  value.descriptor_id = "boolean";
  value.payload_kind = SblrValuePayloadKind::boolean;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = bool_value ? 1 : 0;
  value.encoded_value = bool_value ? "TRUE" : "FALSE";
  value.text_value = bool_value ? "TRUE" : "FALSE";
  return value;
}

SblrValue UInt64ContextValue(std::uint64_t uint_value) {
  SblrValue value;
  value.descriptor_id = "uint64";
  value.is_null = false;
  value.text_value = std::to_string(uint_value);
  value.encoded_value = value.text_value;
  value.payload_kind = SblrValuePayloadKind::unsigned_integer;
  value.has_uint64_value = true;
  value.uint64_value = uint_value;
  if (uint_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    value.has_int64_value = true;
    value.int64_value = static_cast<std::int64_t>(uint_value);
  }
  return value;
}

SblrResult ContextResult(std::string_view variable_id, SblrValue value) {
  SblrResult out = MakeSblrSuccess(std::string(variable_id));
  out.scalar_values.push_back(std::move(value));
  return out;
}

SblrResult ContextFailure(std::string_view variable_id,
                          const SblrExecutionContext& context,
                          std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_CONTEXT_VARIABLE_UNAVAILABLE", context, std::move(detail));
  diagnostic.fields.push_back({"variable_id", std::string(variable_id)});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(variable_id), std::move(diagnostic));
}

SblrResult RequiredTextContext(std::string_view variable_id,
                               const SblrExecutionContext& context,
                               std::string descriptor_id,
                               const std::string& value_text,
                               std::string detail) {
  if (value_text.empty()) {
    return ContextFailure(variable_id, context, std::move(detail));
  }
  return ContextResult(variable_id, TextContextValue(std::move(descriptor_id), value_text));
}

bool MatchesAny(std::string_view value, std::initializer_list<std::string_view> candidates) {
  for (const auto candidate : candidates) {
    if (value == candidate) return true;
  }
  return false;
}

std::string LowerAscii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const auto ch : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string NormalizeSessionConfigName(std::string_view name) {
  auto normalized = LowerAscii(name);
  if (normalized == "time zone") return "timezone";
  return normalized;
}

std::optional<std::string> SessionConfigValue(const SblrExecutionContext& context,
                                              std::string_view name) {
  const auto normalized = NormalizeSessionConfigName(name);
  if (context.session_runtime_state) {
    for (auto it = context.session_runtime_state->config_entries.rbegin();
         it != context.session_runtime_state->config_entries.rend();
         ++it) {
      if (NormalizeSessionConfigName(it->name) == normalized) {
        return it->value;
      }
    }
  }
  if (normalized == "timezone") return std::string("UTC");
  return std::nullopt;
}

}  // namespace

const std::vector<SblrContextVariableEntry>& StandardSblrContextVariableRegistry() {
  static const std::vector<SblrContextVariableEntry> vars = {
      {"ctx_current_user_uuid", "019b6cf8-b000-740c-add4-4ac7f3cf1904", "security", "CURRENT_USER_UUID", "uuid", "self"},
      {"ctx_current_role_uuid", "019b6cf8-b000-77ed-a794-1bbeb27dc114", "security", "CURRENT_ROLE_UUID", "uuid", "self"},
      {"ctx_current_group_uuid_set", "019b6cf8-b000-7968-afb5-98d99a3f8ceb", "security", "CURRENT_GROUP_UUID_SET", "uuid_array", "policy_gated"},
      {"ctx_current_schema_uuid", "019b6cf8-b000-736e-a86b-f2b968da482d", "schema", "CURRENT_SCHEMA_UUID", "uuid", "self"},
      {"ctx_current_database_uuid", "019b6cf8-b000-7ebc-a37d-ebe685cf140a", "database", "CURRENT_DATABASE_UUID", "uuid", "self"},
      {"ctx_current_cluster_uuid", "019b6cf8-b000-702d-aac7-748980637d4e", "cluster", "CURRENT_CLUSTER_UUID", "uuid", "cluster_policy"},
      {"ctx_current_node_uuid", "019b6cf8-b000-71c5-acef-9952f5fc85f7", "node", "CURRENT_NODE_UUID", "uuid", "self"},
      {"ctx_current_session_uuid", "019b6cf8-b000-7eae-a9d1-d3d8e83f84d6", "session", "CURRENT_SESSION_UUID", "uuid", "self"},
      {"ctx_current_attachment_uuid", "019b6cf8-b000-737e-ac48-c49abfdb84eb", "session", "CURRENT_ATTACHMENT_UUID", "uuid", "self"},
      {"ctx_current_transaction_uuid", "019b6cf8-b000-7001-a841-b06863305d2c", "transaction", "CURRENT_TRANSACTION_UUID", "uuid", "self"},
      {"ctx_current_local_transaction_id", "019b6cf8-b000-7a9c-adbc-c7ecf9766993", "transaction", "CURRENT_LOCAL_TRANSACTION_ID", "uint64", "self"},
      {"ctx_current_statement_uuid", "019b6cf8-b000-791a-ae03-da4eb6dfb07a", "statement", "CURRENT_STATEMENT_UUID", "uuid", "self"},
      {"ctx_statement_timestamp", "019b6cf8-b000-71fc-a68a-a60373f5f3c9", "time", "STATEMENT_TIMESTAMP", "timestamp_tz", "self"},
      {"ctx_transaction_timestamp", "019b6cf8-b000-7cf6-a94b-b87e46ecfd39", "time", "TRANSACTION_TIMESTAMP", "timestamp_tz", "self"},
      {"ctx_current_timestamp", "019b6cf8-b000-7a98-a164-2862f244dcbe", "time", "CURRENT_TIMESTAMP", "timestamp_tz", "self"},
      {"ctx_current_monotonic_ns", "019b6cf8-b000-7d8a-ae59-6c256e1cca5f", "time", "CURRENT_MONOTONIC_NS", "uint128", "self"},
      {"ctx_parser_profile_uuid", "019b6cf8-b000-724a-a9a1-dcdfbb8fc457", "parser", "CURRENT_PARSER_PROFILE_UUID", "uuid", "self"},
      {"ctx_client_protocol_uuid", "019b6cf8-b000-7051-a081-500a1099c7eb", "parser", "CURRENT_CLIENT_PROTOCOL_UUID", "uuid", "self"},
      {"ctx_restricted_open_mode", "019b6cf8-b000-76b6-a820-7eb9409d5605", "mode", "CURRENT_RESTRICTED_OPEN_MODE", "boolean", "self"},
      {"ctx_read_only_mode", "019b6cf8-b000-7d2c-aad7-d3d2633c2caa", "mode", "CURRENT_READ_ONLY_MODE", "boolean", "self"},
      {"ctx_current_sqlstate", "019b6cf8-b000-7d6e-9a86-c74277d99930", "diagnostic", "CURRENT_SQLSTATE", "character", "self"},
      {"ctx_current_diagnostic_id", "019b6cf8-b000-791f-bdf4-ec2264bbf3fd", "diagnostic", "CURRENT_DIAGNOSTIC_ID", "character", "self"},
      {"ctx_last_row_count", "019b6cf8-b000-7e42-9472-beb8d6691579", "diagnostic", "LAST_ROW_COUNT", "uint64", "self"},
      {"ctx_last_identity_value", "019b6cf8-b000-7e21-bf7a-fe75f261be07", "diagnostic", "LAST_IDENTITY_VALUE", "character", "self"},
      {"ctx_current_engine_version", "019b6cf8-b000-78a6-8f0e-7b0a9f5e7f10", "runtime", "CURRENT_ENGINE_VERSION", "character", "self"},
      {"ctx_current_timezone", "019b6cf8-b000-79c1-b2e4-8c34f6c71622", "session", "CURRENT_TIMEZONE", "character", "self"},
      {"ctx_current_transaction_isolation", "019b6cf8-b000-768d-a5f0-21c29f529d21", "transaction", "CURRENT_TRANSACTION_ISOLATION", "character", "self"},
      {"context.current_user", "018f0000-0000-7000-8000-000000002001", "legacy", "CURRENT_USER", "uuid", "self"},
      {"context.current_transaction", "018f0000-0000-7000-8000-000000002002", "legacy", "CURRENT_TRANSACTION", "uuid", "self"},
      {"context.current_role", "018f0000-0000-7000-8000-000000002003", "legacy", "CURRENT_ROLE", "uuid", "self"},
      {"context.session_dialect", "018f0000-0000-7000-8000-000000002004", "legacy", "SESSION_DIALECT", "uuid", "self"},
      {"context.database_uuid", "018f0000-0000-7000-8000-000000002005", "legacy", "DATABASE_UUID", "uuid", "self"},
      {"context.node_uuid", "018f0000-0000-7000-8000-000000002006", "legacy", "NODE_UUID", "uuid", "self"},
  };
  return vars;
}

SblrResult ResolveSblrContextVariable(std::string_view variable_id, const SblrExecutionContext& context) {
  if (MatchesAny(variable_id,
                 {"ctx_current_user_uuid", "context.current_user", "CURRENT_USER", "USER", "SESSION_USER", "SYSTEM_USER",
                  "firebird.current_user", "postgres.current_user", "postgres.session_user", "mysql.current_user", "mysql.user"})) {
    return RequiredTextContext(variable_id, context, "uuid", context.user_uuid, "current user UUID is not present in the execution context");
  }
  if (MatchesAny(variable_id, {"ctx_current_role_uuid", "context.current_role", "CURRENT_ROLE", "firebird.current_role"})) {
    return context.current_role_uuid.empty() ? ContextResult(variable_id, NullContextValue("uuid"))
                                             : ContextResult(variable_id, TextContextValue("uuid", context.current_role_uuid));
  }
  if (variable_id == "ctx_current_group_uuid_set") {
    if (!context.security_context_present) {
      return ContextFailure(variable_id, context, "effective group UUID set requires an active security context");
    }
    return context.current_group_uuid_set.empty() ? ContextResult(variable_id, NullContextValue("uuid_array"))
                                                  : ContextResult(variable_id, TextContextValue("uuid_array", context.current_group_uuid_set));
  }
  if (MatchesAny(variable_id, {"ctx_current_schema_uuid", "CURRENT_SCHEMA", "postgres.current_schema"})) {
    return RequiredTextContext(variable_id, context, "uuid", context.current_schema_uuid, "current schema UUID is not present in the execution context");
  }
  if (MatchesAny(variable_id,
                 {"ctx_current_database_uuid", "context.database_uuid", "CURRENT_DATABASE", "DATABASE", "DATABASE()",
                  "postgres.current_database", "mysql.database"})) {
    return RequiredTextContext(variable_id, context, "uuid", context.database_uuid, "database UUID is not present in the execution context");
  }
  if (variable_id == "ctx_current_cluster_uuid") {
    return context.cluster_uuid.empty() ? ContextResult(variable_id, NullContextValue("uuid"))
                                        : ContextResult(variable_id, TextContextValue("uuid", context.cluster_uuid));
  }
  if (variable_id == "ctx_current_node_uuid" || variable_id == "context.node_uuid") {
    return RequiredTextContext(variable_id, context, "uuid", context.node_uuid, "node UUID is not present in the execution context");
  }
  if (MatchesAny(variable_id, {"ctx_current_session_uuid", "SESSION_ID", "CURRENT_SESSION_ID", "CURRENT_SESSION_UUID", "oracle.sessionid"})) {
    return RequiredTextContext(variable_id, context, "uuid", context.session_uuid, "session UUID is not present in the execution context");
  }
  if (variable_id == "ctx_current_attachment_uuid") {
    return RequiredTextContext(variable_id, context, "uuid", context.attachment_uuid, "attachment UUID is not present in the execution context");
  }
  if (MatchesAny(variable_id, {"ctx_current_transaction_uuid", "context.current_transaction", "CURRENT_TRANSACTION", "firebird.current_transaction"})) {
    return context.transaction_uuid.empty() ? ContextResult(variable_id, NullContextValue("uuid"))
                                            : ContextResult(variable_id, TextContextValue("uuid", context.transaction_uuid));
  }
  if (MatchesAny(variable_id, {"ctx_current_local_transaction_id", "TRANSACTION_ID", "CURRENT_TRANSACTION_ID", "firebird.transaction_id"})) {
    return context.local_transaction_id == 0 ? ContextResult(variable_id, NullContextValue("uint64"))
                                             : ContextResult(variable_id, UInt64ContextValue(context.local_transaction_id));
  }
  if (variable_id == "ctx_current_statement_uuid") {
    return RequiredTextContext(variable_id, context, "uuid", context.statement_uuid, "statement UUID is not present in the execution context");
  }
  if (MatchesAny(variable_id, {"ctx_statement_timestamp", "STATEMENT_TIMESTAMP"})) {
    return RequiredTextContext(variable_id, context, "timestamp_tz", context.statement_timestamp, "statement timestamp is not present in the execution context");
  }
  if (MatchesAny(variable_id, {"ctx_transaction_timestamp", "TRANSACTION_TIMESTAMP"})) {
    return context.transaction_timestamp.empty() ? ContextResult(variable_id, NullContextValue("timestamp_tz"))
                                                 : ContextResult(variable_id, TextContextValue("timestamp_tz", context.transaction_timestamp));
  }
  if (MatchesAny(variable_id,
                 {"ctx_current_timestamp", "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME", "LOCALTIME", "LOCALTIMESTAMP",
                  "NOW", "now", "postgres.now", "mysql.now", "sqlite.current_timestamp"})) {
    return RequiredTextContext(variable_id, context, "timestamp_tz", context.current_timestamp, "current timestamp provider value is not present in the execution context");
  }
  if (variable_id == "ctx_current_monotonic_ns") {
    return RequiredTextContext(variable_id, context, "uint128", context.current_monotonic_ns, "monotonic timestamp provider value is not present in the execution context");
  }
  if (variable_id == "ctx_parser_profile_uuid" || variable_id == "context.session_dialect") {
    return RequiredTextContext(variable_id, context, "uuid", context.parser_profile_uuid, "parser profile UUID is not present in the execution context");
  }
  if (variable_id == "ctx_client_protocol_uuid") {
    return context.client_protocol_uuid.empty() ? ContextResult(variable_id, NullContextValue("uuid"))
                                                : ContextResult(variable_id, TextContextValue("uuid", context.client_protocol_uuid));
  }
  if (variable_id == "ctx_restricted_open_mode") {
    return ContextResult(variable_id, BoolContextValue(context.restricted_open_mode));
  }
  if (variable_id == "ctx_read_only_mode") {
    return ContextResult(variable_id, BoolContextValue(context.read_only_mode));
  }
  if (MatchesAny(variable_id, {"ctx_current_sqlstate", "SQLSTATE", "CURRENT_SQLSTATE"})) {
    return context.current_sqlstate.empty() ? ContextResult(variable_id, NullContextValue("character"))
                                            : ContextResult(variable_id, TextContextValue("character", context.current_sqlstate));
  }
  if (MatchesAny(variable_id, {"ctx_current_diagnostic_id", "CURRENT_DIAGNOSTIC_ID", "SB_DIAGNOSTIC_ID"})) {
    return context.current_diagnostic_id.empty() ? ContextResult(variable_id, NullContextValue("character"))
                                                 : ContextResult(variable_id, TextContextValue("character", context.current_diagnostic_id));
  }
  if (MatchesAny(variable_id, {"ctx_last_row_count", "ROW_COUNT", "ROW_COUNT()", "FOUND_ROWS", "sqlite.changes"})) {
    return context.last_row_count_present ? ContextResult(variable_id, UInt64ContextValue(context.last_row_count))
                                          : ContextResult(variable_id, NullContextValue("uint64"));
  }
  if (MatchesAny(variable_id,
                 {"ctx_last_identity_value", "LAST_INSERT_ID", "LAST_INSERT_ID()", "last_insert_rowid", "sqlite.last_insert_rowid",
                  "IDENTITY_VAL_LOCAL"})) {
    return context.last_identity_value_present ? ContextResult(variable_id, TextContextValue("character", context.last_identity_value))
                                               : ContextResult(variable_id, NullContextValue("character"));
  }
  if (MatchesAny(variable_id,
                 {"ctx_current_engine_version", "CURRENT_ENGINE_VERSION", "VERSION", "ENGINE_VERSION", "firebird.engine_version"})) {
    return ContextResult(variable_id, TextContextValue("character", "ScratchBird 0.1.0"));
  }
  if (MatchesAny(variable_id,
                 {"ctx_current_timezone", "CURRENT_TIMEZONE", "TIMEZONE", "SESSION_TIMEZONE", "mysql.time_zone"})) {
    return ContextResult(variable_id,
                         TextContextValue("character",
                                          SessionConfigValue(context, "timezone").value_or("UTC")));
  }
  if (MatchesAny(variable_id,
                 {"ctx_current_transaction_isolation", "CURRENT_TRANSACTION_ISOLATION",
                  "TRANSACTION_ISOLATION", "TX_ISOLATION", "mysql.transaction_isolation"})) {
    return ContextResult(variable_id,
                         TextContextValue("character",
                                          context.transaction_isolation_level.empty()
                                              ? "read_committed"
                                              : context.transaction_isolation_level));
  }
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_CONTEXT_VARIABLE_UNKNOWN", context,
                                              "context variable is not registered");
  diagnostic.fields.push_back({"variable_id", std::string(variable_id)});
  return MakeSblrFailure(SblrStatusCode::unsupported_feature, std::string(variable_id), std::move(diagnostic));
}

}  // namespace scratchbird::engine::sblr
