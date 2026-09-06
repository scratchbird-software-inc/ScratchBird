// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_to_sbsql.hpp"

#include "sblr_opcode_registry.hpp"
#include "sblr_opcode_stream.hpp"
#include "sblr_source_artifact_runtime.hpp"
#include "sblr_savepoint_runtime.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "sblr_transaction_rollback_runtime.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

SblrToSbsqlDiagnostic Diagnostic(std::string code, std::string message) {
  return SblrToSbsqlDiagnostic{std::move(code), std::move(message), true};
}

SblrToSbsqlResult Refuse(std::string code, std::string message) {
  SblrToSbsqlResult result;
  result.ok = false;
  result.diagnostics.push_back(Diagnostic(std::move(code), std::move(message)));
  return result;
}

const SblrOperand* FindOperand(const SblrOperationEnvelope& envelope,
                               std::string_view name) {
  for (const auto& operand : envelope.operands) {
    if (operand.name == name) return &operand;
  }
  return nullptr;
}

std::string_view OperandValue(const SblrOperationEnvelope& envelope,
                              std::string_view name) {
  const auto* operand = FindOperand(envelope, name);
  if (operand == nullptr) return {};
  if (!operand->value.empty()) return operand->value;
  if (operand->value_kind != SblrValueKind::literal_typed ||
      operand->value_body.size() < 24) {
    return {};
  }
  const auto size = scratchbird::engine::SblrReadU64(
      operand->value_body.data() + 16);
  if (size != operand->value_body.size() - 24) return {};
  return std::string_view(
      reinterpret_cast<const char*>(operand->value_body.data() + 24),
      static_cast<std::size_t>(size));
}

const SblrSourceSymbolArtifact* FindSymbol(const SblrOperationEnvelope& envelope,
                                           std::string_view symbol_kind) {
  for (const auto& symbol : envelope.source_artifact_map.symbols) {
    if (symbol.symbol_kind == symbol_kind) return &symbol;
  }
  return nullptr;
}

const SblrSourceSymbolArtifact* FindSymbolByStableKey(
    const SblrOperationEnvelope& envelope,
    std::string_view symbol_kind,
    std::string_view stable_key) {
  for (const auto& symbol : envelope.source_artifact_map.symbols) {
    if (symbol.symbol_kind == symbol_kind && symbol.stable_key == stable_key) {
      return &symbol;
    }
  }
  return nullptr;
}

const SblrSourceSymbolArtifact* RequiredSymbol(const SblrOperationEnvelope& envelope,
                                               std::string_view symbol_kind,
                                               SblrToSbsqlResult* result) {
  const auto* symbol = FindSymbol(envelope, symbol_kind);
  if (symbol != nullptr && !symbol->render_hint.empty()) return symbol;
  result->ok = false;
  result->diagnostics.push_back(Diagnostic(
      "SB_SBLR_TO_SBSQL_SYMBOL_REQUIRED",
      "SBLR-to-SBsql source-preserving render requires a retained source symbol"));
  return nullptr;
}

const SblrSourceSymbolArtifact* RequiredSymbolByStableKey(
    const SblrOperationEnvelope& envelope,
    std::string_view symbol_kind,
    std::string_view stable_key,
    SblrToSbsqlResult* result) {
  const auto* symbol = FindSymbolByStableKey(envelope, symbol_kind, stable_key);
  if (symbol != nullptr && !symbol->render_hint.empty()) return symbol;
  result->ok = false;
  result->diagnostics.push_back(Diagnostic(
      "SB_SBLR_TO_SBSQL_SYMBOL_REQUIRED",
      "SBLR-to-SBsql source-preserving render requires a retained source symbol"));
  return nullptr;
}

bool IsIdentifier(std::string_view value) {
  if (value.empty()) return false;
  const auto first = static_cast<unsigned char>(value.front());
  if (!std::isalpha(first) && first != '_') return false;
  for (const char ch : value.substr(1)) {
    const auto c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c) && c != '_') return false;
  }
  return true;
}

int HexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

bool ParseUuid(std::string_view text, SblrSourceArtifactUuidV1* uuid) {
  if (uuid == nullptr || text.size() != 36 || text[8] != '-' ||
      text[13] != '-' || text[18] != '-' || text[23] != '-') {
    return false;
  }
  std::size_t output = 0;
  bool nonzero = false;
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '-') {
      ++index;
      continue;
    }
    if (index + 1 >= text.size() || output == uuid->size()) return false;
    const auto high = HexNibble(text[index]);
    const auto low = HexNibble(text[index + 1]);
    if (high < 0 || low < 0) return false;
    (*uuid)[output] = static_cast<std::uint8_t>((high << 4) | low);
    nonzero = nonzero || (*uuid)[output] != 0;
    ++output;
    index += 2;
  }
  return output == uuid->size() && nonzero;
}

template <typename T>
bool NonZero(const T& value) {
  return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) {
    return byte != 0;
  });
}

std::string FormatUuid(const SblrSourceArtifactUuidV1& uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text.push_back('-');
    }
    text.push_back(kHex[uuid[index] >> 4U]);
    text.push_back(kHex[uuid[index] & 0x0fU]);
  }
  return text;
}

struct ExactSavepointRenderAuthority {
  SblrSourceArtifactUuidV1 savepoint_uuid{};
  SblrSourceArtifactUuidV1 transaction_uuid{};
};

std::optional<ExactSavepointRenderAuthority>
DecodeExactSavepointRenderAuthority(const SblrOperationEnvelope& envelope) {
  if (envelope.operands.size() != 1 ||
      envelope.operands.front().ordinal != 1 ||
      envelope.operands.front().name != "savepoint") {
    return std::nullopt;
  }
  const auto& operand = envelope.operands.front();
  ExactSavepointRenderAuthority authority;
  std::string detail;
  if (envelope.operation_id == "engine.op.txn_savepoint" &&
      envelope.opcode == "SBLR_TXN_SAVEPOINT" &&
      operand.type == "savepoint.descriptor" &&
      operand.value_kind == SblrValueKind::savepoint_descriptor) {
    SblrSavepointDescriptorV1 descriptor;
    if (!DecodeSblrSavepointDescriptorV1(
            operand.value_body.data(), operand.value_body.size(), &descriptor,
            &detail)) {
      return std::nullopt;
    }
    authority.savepoint_uuid = descriptor.savepoint_uuid;
    authority.transaction_uuid = descriptor.transaction_uuid;
  } else if (envelope.operation_id ==
                 "engine.op.txn_release_savepoint" &&
             envelope.opcode == "SBLR_TXN_RELEASE_SAVEPOINT" &&
             operand.type == "savepoint.release_handle" &&
             operand.value_kind == SblrValueKind::savepoint_release_handle) {
    SblrSavepointReleaseOperandV1 release;
    if (!DecodeSblrSavepointReleaseOperandV1(
            operand.value_body.data(), operand.value_body.size(), &release,
            &detail)) {
      return std::nullopt;
    }
    authority.savepoint_uuid = release.savepoint_uuid;
    authority.transaction_uuid = release.transaction_uuid;
  } else if (envelope.operation_id ==
                 "engine.op.txn_rollback_to_savepoint" &&
             envelope.opcode == "SBLR_TXN_ROLLBACK_TO_SAVEPOINT" &&
             operand.type == "savepoint.rollback_handle" &&
             operand.value_kind ==
                 SblrValueKind::savepoint_rollback_handle) {
    SblrSavepointRollbackOperandV1 rollback;
    if (!DecodeSblrSavepointRollbackOperandV1(
            operand.value_body.data(), operand.value_body.size(), &rollback,
            &detail)) {
      return std::nullopt;
    }
    authority.savepoint_uuid = rollback.savepoint_uuid;
    authority.transaction_uuid = rollback.transaction_uuid;
  } else {
    return std::nullopt;
  }
  if (!NonZero(authority.savepoint_uuid) ||
      !NonZero(authority.transaction_uuid)) {
    return std::nullopt;
  }
  return authority;
}

std::string FormatSha256(const SblrSourceArtifactSha256V1& sha256) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text = "sha256:";
  text.reserve(7 + sha256.size() * 2);
  for (const auto byte : sha256) {
    text.push_back(kHex[byte >> 4U]);
    text.push_back(kHex[byte & 0x0fU]);
  }
  return text;
}

std::string_view SymbolKindName(SblrSourceArtifactSymbolKindV1 kind) {
  switch (kind) {
    case SblrSourceArtifactSymbolKindV1::variable:
      return "variable";
    case SblrSourceArtifactSymbolKindV1::parameter:
      return "parameter";
    case SblrSourceArtifactSymbolKindV1::cursor:
      return "cursor";
    case SblrSourceArtifactSymbolKindV1::label:
      return "label";
    case SblrSourceArtifactSymbolKindV1::block_name:
      return "block_name";
    case SblrSourceArtifactSymbolKindV1::routine:
      return "routine";
    case SblrSourceArtifactSymbolKindV1::routine_argument:
      return "routine_argument";
    case SblrSourceArtifactSymbolKindV1::exception_handler:
      return "exception_handler";
    case SblrSourceArtifactSymbolKindV1::cte:
      return "cte";
    case SblrSourceArtifactSymbolKindV1::relation_alias:
      return "relation_alias";
    case SblrSourceArtifactSymbolKindV1::column_alias:
      return "column_alias";
    case SblrSourceArtifactSymbolKindV1::object_display_name:
      return "object_display_name";
    case SblrSourceArtifactSymbolKindV1::generated_temp:
      return "generated_temp";
  }
  return {};
}

std::string RenderSourceArtifactIdentifier(
    const SblrSourceArtifactSymbolV1& symbol) {
  if (!symbol.was_quoted) return symbol.raw_name_utf8;
  char open = '"';
  char close = '"';
  switch (symbol.quote_style) {
    case SblrSourceArtifactQuoteStyleV1::double_quote:
      break;
    case SblrSourceArtifactQuoteStyleV1::backtick:
      open = '`';
      close = '`';
      break;
    case SblrSourceArtifactQuoteStyleV1::bracket:
      open = '[';
      close = ']';
      break;
    case SblrSourceArtifactQuoteStyleV1::native_sbsql:
      break;
    case SblrSourceArtifactQuoteStyleV1::none:
      return {};
  }
  std::string rendered;
  rendered.reserve(symbol.raw_name_utf8.size() + 2);
  rendered.push_back(open);
  for (const char ch : symbol.raw_name_utf8) {
    rendered.push_back(ch);
    if (ch == close) rendered.push_back(ch);
  }
  rendered.push_back(close);
  return rendered;
}

bool IsObjectAuthorityOperand(std::string_view name) {
  constexpr std::array<std::string_view, 17> kAuthorityOperands{
      "object_uuid",           "target_object_uuid",
      "relation_object_uuid",  "target_relation_uuid",
      "relation_uuid",         "table_uuid",
      "parent_table_uuid",     "child_table_uuid",
      "index_object_uuid",     "index_uuid",
      "column_object_uuid",    "column_uuid",
      "target_column_uuid",    "parent_column_uuid",
      "child_column_uuid",
      "value_column_uuid",     "predicate_column_uuid"};
  return std::find(kAuthorityOperands.begin(), kAuthorityOperands.end(),
                   name) != kAuthorityOperands.end();
}

std::string ProjectSymbolAuthorityUuid(
    const SblrOperationEnvelope& envelope,
    const SblrSourceArtifactSymbolV1& symbol) {
  if (NonZero(symbol.related_object_uuid)) {
    return FormatUuid(symbol.related_object_uuid);
  }
  if (symbol.symbol_kind == SblrSourceArtifactSymbolKindV1::label) {
    const auto authority = DecodeExactSavepointRenderAuthority(envelope);
    if (authority.has_value() && envelope.operands.size() == 1 &&
        symbol.declaration_node_id ==
            static_cast<std::uint64_t>(envelope.operands.front().ordinal) +
                1U &&
        symbol.scope_node_id == 1) {
      return FormatUuid(authority->savepoint_uuid);
    }
  }

  struct StableKeyAuthority {
    std::string_view stable_key_operand;
    std::string_view authority_operand;
  };
  constexpr std::array<StableKeyAuthority, 10> kStableKeyAuthorities{{
      {"value_column_symbol_key", "value_column_uuid"},
      {"value_parameter_symbol_key", "value_parameter_uuid"},
      {"predicate_column_symbol_key", "predicate_column_uuid"},
      {"predicate_parameter_symbol_key", "predicate_parameter_uuid"},
      {"table_symbol_key", "target_object_uuid"},
      {"table_symbol_key", "relation_object_uuid"},
      {"column_symbol_key", "column_descriptor_uuid"},
      {"index_symbol_key", "index_object_uuid"},
      {"relation_symbol_key", "relation_object_uuid"},
      {"savepoint_symbol_key", "savepoint_authority_uuid"},
  }};
  for (const auto& mapping : kStableKeyAuthorities) {
    if (OperandValue(envelope, mapping.stable_key_operand) !=
        symbol.symbol_key) {
      continue;
    }
    SblrSourceArtifactUuidV1 authority_uuid{};
    if (ParseUuid(OperandValue(envelope, mapping.authority_operand),
                  &authority_uuid)) {
      return FormatUuid(authority_uuid);
    }
    continue;
  }

  std::string_view direct_authority_operand;
  switch (symbol.symbol_kind) {
    case SblrSourceArtifactSymbolKindV1::parameter:
      direct_authority_operand = "parameter_slot_uuid";
      break;
    case SblrSourceArtifactSymbolKindV1::column_alias:
      direct_authority_operand = "projection_alias_uuid";
      break;
    case SblrSourceArtifactSymbolKindV1::object_display_name:
      direct_authority_operand = "target_object_uuid";
      break;
    case SblrSourceArtifactSymbolKindV1::label:
      direct_authority_operand = "savepoint_authority_uuid";
      break;
    default:
      return {};
  }
  SblrSourceArtifactUuidV1 authority_uuid{};
  if (!ParseUuid(OperandValue(envelope, direct_authority_operand),
                 &authority_uuid)) {
    return {};
  }
  return FormatUuid(authority_uuid);
}

SblrSourceArtifactMap ToLegacySourceArtifact(
    const SblrOperationEnvelope& envelope,
    const SblrSourceArtifactMapV1& artifact) {
  SblrSourceArtifactMap legacy;
  legacy.policy_status =
      artifact.redaction_class == SblrSourceArtifactRedactionClassV1::none
          ? "non_authoritative_render_metadata"
          : "redacted_render_metadata";
  legacy.source_identity = FormatUuid(artifact.artifact_uuid);
  legacy.source_hash = FormatSha256(artifact.artifact_sha256);
  legacy.artifact_format = "sblr.source_artifact_map.v1";
  legacy.render_metadata_only = true;
  legacy.contains_sql_text = false;
  legacy.raw_sql_text_authoritative = false;
  for (const auto& symbol : artifact.symbols) {
    SblrSourceSymbolArtifact row;
    row.symbol_kind = std::string(SymbolKindName(symbol.symbol_kind));
    row.stable_key = symbol.symbol_key;
    row.resolved_uuid = ProjectSymbolAuthorityUuid(envelope, symbol);
    row.render_hint =
        symbol.symbol_kind == SblrSourceArtifactSymbolKindV1::label
            ? RenderSourceArtifactIdentifier(symbol)
            : symbol.raw_name_utf8;
    if (symbol.scope_node_id != 0) {
      row.scope = "node:" + std::to_string(symbol.scope_node_id);
    }
    row.source_hash = FormatSha256(symbol.record_sha256);
    row.authoritative = false;
    row.contains_sql_text = false;
    legacy.symbols.push_back(std::move(row));
  }
  for (const auto& hint : artifact.render_hints) {
    SblrOperationRenderHint row;
    row.hint_kind = "source_artifact_v1";
    if (hint.symbol_id != 0 && hint.symbol_id <= artifact.symbols.size()) {
      row.stable_key = artifact.symbols[hint.symbol_id - 1].symbol_key;
    } else {
      row.stable_key = "node." + std::to_string(hint.node_id);
    }
    row.value = hint.format_group.empty()
                    ? "structured_source_preserving_render"
                    : hint.format_group;
    row.authoritative = false;
    row.contains_sql_text = false;
    legacy.operation_render_hints.push_back(std::move(row));
  }
  return legacy;
}

std::string ParameterName(std::string_view render_hint) {
  if (!render_hint.empty() && render_hint.front() == ':') {
    render_hint.remove_prefix(1);
  }
  return std::string(render_hint);
}

bool RequireIdentifier(std::string_view value,
                       std::string_view role,
                       SblrToSbsqlResult* result) {
  if (IsIdentifier(value)) return true;
  result->ok = false;
  result->diagnostics.push_back(Diagnostic(
      "SB_SBLR_TO_SBSQL_SYMBOL_RENDER_HINT_INVALID",
      "SBLR-to-SBsql render hint is not a safe SBsql identifier for " +
          std::string(role)));
  return false;
}

bool IsRenderedIdentifier(std::string_view value) {
  if (IsIdentifier(value)) return true;
  if (value.size() < 2) return false;
  const char open = value.front();
  const char close = open == '[' ? ']' : open;
  if ((open != '"' && open != '`' && open != '[') ||
      value.back() != close) {
    return false;
  }
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte == 0 || byte < 0x20U || byte == 0x7fU) return false;
    if (value[index] != close) continue;
    if (index + 2 >= value.size() || value[index + 1] != close) {
      return false;
    }
    ++index;
  }
  return true;
}

bool RequireRenderedIdentifier(std::string_view value,
                               std::string_view role,
                               SblrToSbsqlResult* result) {
  if (IsRenderedIdentifier(value)) return true;
  result->ok = false;
  result->diagnostics.push_back(Diagnostic(
      "SB_SBLR_TO_SBSQL_SYMBOL_RENDER_HINT_INVALID",
      "SBLR-to-SBsql render hint is not a safe SBsql identifier for " +
          std::string(role)));
  return false;
}

void CopyValidationDiagnostics(const SblrEnvelopeValidationResult& validation,
                               SblrToSbsqlResult* result) {
  result->ok = false;
  for (const auto& diagnostic : validation.diagnostics) {
    result->diagnostics.push_back(
        Diagnostic(diagnostic.code, diagnostic.message));
  }
  if (result->diagnostics.empty()) {
    result->diagnostics.push_back(Diagnostic("SB_SBLR_TO_SBSQL_ENVELOPE_INVALID",
                                             "SBLR envelope is invalid"));
  }
}

bool ValidateSourcePolicy(const SblrOperationEnvelope& envelope,
                          SblrToSbsqlResult* result) {
  const auto& source = envelope.source_artifact_map;
  if (source.policy_status == "absent" ||
      (source.symbols.empty() && source.operation_render_hints.empty())) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
        "Source-preserving SBLR-to-SBsql conversion requires source artifacts"));
    return false;
  }
  if (source.policy_status == "redacted_render_metadata") {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REDACTED",
        "Source-preserving SBLR-to-SBsql conversion cannot use redacted artifacts"));
    return false;
  }
  if (source.contains_sql_text) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN",
        "Source-preserving SBLR-to-SBsql conversion cannot consume retained SQL text"));
    return false;
  }
  if (!source.render_metadata_only || source.raw_sql_text_authoritative) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_POLICY_UNSUPPORTED",
        "Source-preserving SBLR-to-SBsql conversion requires non-authoritative render-only metadata"));
    return false;
  }
  if (source.policy_status != "non_authoritative_render_metadata") {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_POLICY_UNSUPPORTED",
        "Source-preserving SBLR-to-SBsql conversion requires non-authoritative render metadata"));
    return false;
  }
  return true;
}

bool RequireRenderFamily(const SblrOperationEnvelope& envelope,
                         std::string_view expected_family,
                         SblrToSbsqlResult* result) {
  if (OperandValue(envelope, "sbsql_render_family") != expected_family) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_RENDER_FAMILY_UNSUPPORTED",
        "SBLR-to-SBsql conversion does not support this render family"));
    return false;
  }
  return true;
}

bool RequireOperand(const SblrOperationEnvelope& envelope,
                    std::string_view name,
                    std::string_view message,
                    SblrToSbsqlResult* result) {
  if (!OperandValue(envelope, name).empty()) return true;
  result->ok = false;
  result->diagnostics.push_back(Diagnostic(
      "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED", std::string(message)));
  return false;
}

bool RequireSemanticOperand(const SblrOperationEnvelope& envelope,
                            std::string_view name,
                            std::string_view message,
                            SblrToSbsqlResult* result) {
  if (!OperandValue(envelope, name).empty()) return true;
  result->ok = false;
  result->diagnostics.push_back(
      Diagnostic("SB_SBLR_TO_SBSQL_OPERAND_REQUIRED", std::string(message)));
  return false;
}

bool RequireSymbolAuthorityMatch(const SblrOperationEnvelope& envelope,
                                 std::string_view symbol_kind,
                                 std::string_view operand_name,
                                 std::string_view message_prefix,
                                 SblrToSbsqlResult* result) {
  const auto authority_uuid = OperandValue(envelope, operand_name);
  if (authority_uuid.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
        std::string(message_prefix) + " requires UUID authority"));
    return false;
  }
  const auto* symbol = FindSymbol(envelope, symbol_kind);
  if (symbol == nullptr || symbol->resolved_uuid.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OBJECT_REQUIRED",
        std::string(message_prefix) +
            " cannot use render hints without UUID authority"));
    return false;
  }
  if (symbol->resolved_uuid != authority_uuid) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
        std::string(message_prefix) +
            " source artifact UUID does not match the envelope authority operand"));
    return false;
  }
  return true;
}

bool RequireSymbolAuthorityMatchByStableKey(
    const SblrOperationEnvelope& envelope,
    std::string_view symbol_kind,
    std::string_view stable_key_operand_name,
    std::string_view authority_operand_name,
    std::string_view message_prefix,
    SblrToSbsqlResult* result) {
  const auto stable_key = OperandValue(envelope, stable_key_operand_name);
  if (stable_key.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_OPERAND_REQUIRED",
        std::string(message_prefix) + " requires a source symbol stable key"));
    return false;
  }
  const auto authority_uuid = OperandValue(envelope, authority_operand_name);
  if (authority_uuid.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
        std::string(message_prefix) + " requires UUID authority"));
    return false;
  }
  const auto* symbol = FindSymbolByStableKey(envelope, symbol_kind, stable_key);
  if (symbol == nullptr || symbol->resolved_uuid.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OBJECT_REQUIRED",
        std::string(message_prefix) +
            " cannot use render hints without UUID authority"));
    return false;
  }
  if (symbol->resolved_uuid != authority_uuid) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
        std::string(message_prefix) +
            " source artifact UUID does not match the envelope authority operand"));
    return false;
  }
  return true;
}

bool IsOperation(const SblrOperationEnvelope& envelope,
                 std::string_view operation_id,
                 std::string_view opcode) {
  return envelope.operation_id == operation_id && envelope.opcode == opcode;
}

bool IsAnyOperation(const SblrOperationEnvelope& envelope,
                    std::string_view operation_id_a,
                    std::string_view opcode_a,
                    std::string_view operation_id_b,
                    std::string_view opcode_b) {
  return IsOperation(envelope, operation_id_a, opcode_a) ||
         IsOperation(envelope, operation_id_b, opcode_b);
}

bool ValidateProceduralAuthorityOperands(const SblrOperationEnvelope& envelope,
                                         SblrToSbsqlResult* result) {
  if (!RequireRenderFamily(envelope, "source_preserving_procedural_bundle_v1",
                           result)) {
    return false;
  }
  if (OperandValue(envelope, "authority_descriptor_uuid").empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
        "SBLR-to-SBsql conversion requires descriptor authority operands"));
    return false;
  }
  const auto relation_uuid = OperandValue(envelope, "relation_object_uuid");
  if (relation_uuid.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
        "SBLR-to-SBsql conversion requires relation object UUID authority"));
    return false;
  }
  const auto* object_symbol = FindSymbol(envelope, "object_display_name");
  if (object_symbol == nullptr || object_symbol->resolved_uuid.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_OBJECT_REQUIRED",
        "SBLR-to-SBsql conversion cannot use object display names without UUID authority"));
    return false;
  }
  if (object_symbol->resolved_uuid != relation_uuid) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
        "SBLR-to-SBsql source artifact object UUID does not match the envelope authority operand"));
    return false;
  }
  if (OperandValue(envelope, "variable_type").empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic(
        "SB_SBLR_TO_SBSQL_OPERAND_REQUIRED",
        "SBLR-to-SBsql conversion requires operand semantics for variable type"));
    return false;
  }
  return true;
}

SblrToSbsqlResult RenderProceduralBundle(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  if (!ValidateProceduralAuthorityOperands(envelope, &result)) return result;

  const auto* variable = RequiredSymbol(envelope, "variable", &result);
  const auto* parameter = RequiredSymbol(envelope, "parameter", &result);
  const auto* cursor = RequiredSymbol(envelope, "cursor", &result);
  const auto* label = RequiredSymbol(envelope, "label", &result);
  const auto* handler = RequiredSymbol(envelope, "exception_handler", &result);
  const auto* relation_alias = RequiredSymbol(envelope, "relation_alias", &result);
  const auto* column_alias = RequiredSymbol(envelope, "column_alias", &result);
  const auto* object_display_name = RequiredSymbol(envelope, "object_display_name", &result);
  if (!result.diagnostics.empty()) return result;

  const std::string parameter_name = ParameterName(parameter->render_hint);
  if (!RequireIdentifier(variable->render_hint, "variable", &result) ||
      !RequireIdentifier(parameter_name, "parameter", &result) ||
      !RequireIdentifier(cursor->render_hint, "cursor", &result) ||
      !RequireIdentifier(label->render_hint, "label", &result) ||
      !RequireIdentifier(handler->render_hint, "exception_handler", &result) ||
      !RequireIdentifier(relation_alias->render_hint, "relation_alias", &result) ||
      !RequireIdentifier(column_alias->render_hint, "column_alias", &result) ||
      !RequireIdentifier(object_display_name->render_hint, "object_display_name", &result)) {
    return result;
  }

  std::ostringstream out;
  out << "DECLARE VARIABLE " << variable->render_hint << ' '
      << OperandValue(envelope, "variable_type") << ";\n";
  out << "PARAM LIST " << parameter_name << ";\n";
  out << "DECLARE " << cursor->render_hint << " CURSOR;\n";
  out << "PSQL LEAVE " << label->render_hint << ";\n";
  out << "EXCEPTION HANDLER WHEN " << handler->render_hint << ";\n";
  out << "SELECT :" << parameter_name << " AS " << column_alias->render_hint
      << " FROM " << object_display_name->render_hint
      << " AS " << relation_alias->render_hint << ";";

  result.ok = true;
  result.sbsql_text = out.str();
  return result;
}

SblrToSbsqlResult RenderDmlSingleRow(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  if (!RequireRenderFamily(envelope, "source_preserving_dml_single_row_v1",
                           &result) ||
      !RequireOperand(envelope, "authority_descriptor_uuid",
                      "SBLR-to-SBsql DML conversion requires descriptor authority",
                      &result) ||
      !RequireSymbolAuthorityMatch(envelope, "object_display_name",
                                   "target_object_uuid",
                                   "DML target object",
                                   &result)) {
    return result;
  }

  const auto* table = RequiredSymbol(envelope, "object_display_name", &result);
  if (!result.diagnostics.empty()) return result;
  if (!RequireIdentifier(table->render_hint, "object_display_name", &result)) {
    return result;
  }

  const auto require_value_column = [&]() -> const SblrSourceSymbolArtifact* {
    if (!RequireSymbolAuthorityMatchByStableKey(envelope, "column_alias",
                                                "value_column_symbol_key",
                                                "value_column_uuid",
                                                "DML value column",
                                                &result)) {
      return nullptr;
    }
    return RequiredSymbolByStableKey(
        envelope, "column_alias", OperandValue(envelope, "value_column_symbol_key"),
        &result);
  };
  const auto require_value_parameter = [&]() -> const SblrSourceSymbolArtifact* {
    if (!RequireSymbolAuthorityMatchByStableKey(envelope, "parameter",
                                                "value_parameter_symbol_key",
                                                "value_parameter_uuid",
                                                "DML value parameter",
                                                &result)) {
      return nullptr;
    }
    return RequiredSymbolByStableKey(
        envelope, "parameter", OperandValue(envelope, "value_parameter_symbol_key"),
        &result);
  };
  const auto require_predicate_column = [&]() -> const SblrSourceSymbolArtifact* {
    if (!RequireSymbolAuthorityMatchByStableKey(envelope, "column_alias",
                                                "predicate_column_symbol_key",
                                                "predicate_column_uuid",
                                                "DML predicate column",
                                                &result)) {
      return nullptr;
    }
    return RequiredSymbolByStableKey(
        envelope, "column_alias", OperandValue(envelope, "predicate_column_symbol_key"),
        &result);
  };
  const auto require_predicate_parameter = [&]() -> const SblrSourceSymbolArtifact* {
    if (!RequireSymbolAuthorityMatchByStableKey(envelope, "parameter",
                                                "predicate_parameter_symbol_key",
                                                "predicate_parameter_uuid",
                                                "DML predicate parameter",
                                                &result)) {
      return nullptr;
    }
    return RequiredSymbolByStableKey(
        envelope, "parameter", OperandValue(envelope, "predicate_parameter_symbol_key"),
        &result);
  };

  const bool insert = IsAnyOperation(envelope, "dml.insert_rows",
                                     "SBLR_DML_INSERT_ROWS",
                                     "engine.op.insert", "SBLR_INSERT");
  const bool select = IsOperation(envelope, "dml.select_rows",
                                  "SBLR_DML_SELECT_ROWS");
  const bool update = IsAnyOperation(envelope, "dml.update_rows",
                                     "SBLR_DML_UPDATE_ROWS",
                                     "engine.op.update", "SBLR_UPDATE");
  const bool delete_row = IsAnyOperation(envelope, "dml.delete_rows",
                                         "SBLR_DML_DELETE_ROWS",
                                         "engine.op.delete", "SBLR_DELETE");
  if (!insert && !select && !update && !delete_row) {
    return Refuse("SB_SBLR_TO_SBSQL_UNSUPPORTED_OPERATION",
                  "SBLR-to-SBsql DML route supports insert select update and delete only");
  }

  const auto* value_column = (insert || select || update) ? require_value_column() : nullptr;
  const auto* value_parameter = (insert || update) ? require_value_parameter() : nullptr;
  const auto* predicate_column = (select || update || delete_row) ? require_predicate_column() : nullptr;
  const auto* predicate_parameter = (select || update || delete_row) ? require_predicate_parameter() : nullptr;
  if (!result.diagnostics.empty()) return result;

  if (value_column != nullptr &&
      !RequireIdentifier(value_column->render_hint, "value_column", &result)) {
    return result;
  }
  if (predicate_column != nullptr &&
      !RequireIdentifier(predicate_column->render_hint, "predicate_column", &result)) {
    return result;
  }
  std::string value_parameter_name;
  if (value_parameter != nullptr) {
    value_parameter_name = ParameterName(value_parameter->render_hint);
    if (!RequireIdentifier(value_parameter_name, "value_parameter", &result)) {
      return result;
    }
  }
  std::string predicate_parameter_name;
  if (predicate_parameter != nullptr) {
    predicate_parameter_name = ParameterName(predicate_parameter->render_hint);
    if (!RequireIdentifier(predicate_parameter_name, "predicate_parameter", &result)) {
      return result;
    }
  }

  std::ostringstream out;
  if (insert) {
    out << "INSERT INTO " << table->render_hint << " ("
        << value_column->render_hint << ") VALUES (:"
        << value_parameter_name << ");";
  } else if (select) {
    out << "SELECT " << value_column->render_hint << " FROM "
        << table->render_hint << " WHERE "
        << predicate_column->render_hint << " = :"
        << predicate_parameter_name << ";";
  } else if (update) {
    out << "UPDATE " << table->render_hint << " SET "
        << value_column->render_hint << " = :" << value_parameter_name
        << " WHERE " << predicate_column->render_hint << " = :"
        << predicate_parameter_name << ";";
  } else {
    out << "DELETE FROM " << table->render_hint << " WHERE "
        << predicate_column->render_hint << " = :"
        << predicate_parameter_name << ";";
  }

  result.ok = true;
  result.sbsql_text = out.str();
  return result;
}

SblrToSbsqlResult RenderDdlCreateTable(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  if (!RequireRenderFamily(envelope, "source_preserving_ddl_create_table_v1",
                           &result) ||
      !RequireOperand(envelope, "authority_descriptor_uuid",
                      "SBLR-to-SBsql CREATE TABLE conversion requires descriptor authority",
                      &result) ||
      !RequireSymbolAuthorityMatchByStableKey(envelope, "object_display_name",
                                              "table_symbol_key",
                                              "target_object_uuid",
                                              "CREATE TABLE target object",
                                              &result) ||
      !RequireSymbolAuthorityMatchByStableKey(envelope, "column_alias",
                                              "column_symbol_key",
                                              "column_descriptor_uuid",
                                              "CREATE TABLE column",
                                              &result) ||
      !RequireSemanticOperand(envelope, "column_type",
                              "SBLR-to-SBsql CREATE TABLE conversion requires column type semantics",
                              &result)) {
    return result;
  }

  const auto* table = RequiredSymbolByStableKey(
      envelope, "object_display_name", OperandValue(envelope, "table_symbol_key"),
      &result);
  const auto* column = RequiredSymbolByStableKey(
      envelope, "column_alias", OperandValue(envelope, "column_symbol_key"),
      &result);
  if (!result.diagnostics.empty()) return result;
  if (!RequireIdentifier(table->render_hint, "table", &result) ||
      !RequireIdentifier(column->render_hint, "column", &result) ||
      !RequireIdentifier(OperandValue(envelope, "column_type"), "column_type", &result)) {
    return result;
  }

  result.ok = true;
  result.sbsql_text = std::string("CREATE TABLE ") + table->render_hint + " (" +
                      column->render_hint + " " +
                      std::string(OperandValue(envelope, "column_type")) + ");";
  return result;
}

SblrToSbsqlResult RenderDdlCreateIndex(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  if (!RequireRenderFamily(envelope, "source_preserving_ddl_create_index_v1",
                           &result) ||
      !RequireOperand(envelope, "authority_descriptor_uuid",
                      "SBLR-to-SBsql CREATE INDEX conversion requires descriptor authority",
                      &result) ||
      !RequireSymbolAuthorityMatchByStableKey(envelope, "object_display_name",
                                              "index_symbol_key",
                                              "index_object_uuid",
                                              "CREATE INDEX index object",
                                              &result) ||
      !RequireSymbolAuthorityMatchByStableKey(envelope, "object_display_name",
                                              "table_symbol_key",
                                              "relation_object_uuid",
                                              "CREATE INDEX relation object",
                                              &result) ||
      !RequireSymbolAuthorityMatchByStableKey(envelope, "column_alias",
                                              "column_symbol_key",
                                              "column_descriptor_uuid",
                                              "CREATE INDEX column",
                                              &result)) {
    return result;
  }

  const auto* index = RequiredSymbolByStableKey(
      envelope, "object_display_name", OperandValue(envelope, "index_symbol_key"),
      &result);
  const auto* table = RequiredSymbolByStableKey(
      envelope, "object_display_name", OperandValue(envelope, "table_symbol_key"),
      &result);
  const auto* column = RequiredSymbolByStableKey(
      envelope, "column_alias", OperandValue(envelope, "column_symbol_key"),
      &result);
  if (!result.diagnostics.empty()) return result;
  if (!RequireIdentifier(index->render_hint, "index", &result) ||
      !RequireIdentifier(table->render_hint, "table", &result) ||
      !RequireIdentifier(column->render_hint, "column", &result)) {
    return result;
  }

  result.ok = true;
  result.sbsql_text = std::string("CREATE INDEX ") + index->render_hint + " ON " +
                      table->render_hint + " (" + column->render_hint + ");";
  return result;
}

SblrToSbsqlResult RenderQueryProjection(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  if (!RequireRenderFamily(envelope, "source_preserving_query_projection_v1",
                           &result) ||
      !RequireOperand(envelope, "authority_descriptor_uuid",
                      "SBLR-to-SBsql query projection conversion requires descriptor authority",
                      &result) ||
      !RequireOperand(envelope, "projection_descriptor_uuid",
                      "SBLR-to-SBsql query projection conversion requires projection descriptor authority",
                      &result) ||
      !RequireSymbolAuthorityMatch(envelope, "parameter",
                                   "parameter_slot_uuid",
                                   "query projection parameter",
                                   &result) ||
      !RequireSymbolAuthorityMatch(envelope, "column_alias",
                                   "projection_alias_uuid",
                                   "query projection alias",
                                   &result) ||
      !RequireSemanticOperand(envelope, "projection_expr_kind",
                              "SBLR-to-SBsql query projection conversion requires expression semantics",
                              &result)) {
    return result;
  }
  if (OperandValue(envelope, "projection_expr_kind") != "parameter_reference") {
    return Refuse("SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
                  "SBLR-to-SBsql query projection route supports parameter_reference projections only");
  }

  const auto* parameter = RequiredSymbol(envelope, "parameter", &result);
  const auto* column_alias = RequiredSymbol(envelope, "column_alias", &result);
  if (!result.diagnostics.empty()) return result;

  const std::string parameter_name = ParameterName(parameter->render_hint);
  if (!RequireIdentifier(parameter_name, "parameter", &result) ||
      !RequireIdentifier(column_alias->render_hint, "column_alias", &result)) {
    return result;
  }

  result.ok = true;
  result.sbsql_text = "SELECT :" + parameter_name + " AS " +
                      column_alias->render_hint + ";";
  return result;
}

SblrToSbsqlResult RenderCatalogGetDescriptor(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  if (!RequireRenderFamily(envelope, "source_preserving_catalog_descriptor_v1",
                           &result) ||
      !RequireOperand(envelope, "authority_descriptor_uuid",
                      "SBLR-to-SBsql catalog descriptor conversion requires descriptor authority",
                      &result) ||
      !RequireSymbolAuthorityMatch(envelope, "object_display_name",
                                   "target_object_uuid",
                                   "catalog descriptor target object",
                                   &result) ||
      !RequireSemanticOperand(envelope, "target_object_kind",
                              "SBLR-to-SBsql catalog descriptor conversion requires target object kind",
                              &result)) {
    return result;
  }
  if (OperandValue(envelope, "target_object_kind") != "TABLE") {
    return Refuse("SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
                  "SBLR-to-SBsql catalog descriptor route supports TABLE descriptors only");
  }

  const auto* object_display_name =
      RequiredSymbol(envelope, "object_display_name", &result);
  if (!result.diagnostics.empty()) return result;
  if (!RequireIdentifier(object_display_name->render_hint, "object_display_name",
                         &result)) {
    return result;
  }

  result.ok = true;
  result.sbsql_text = "SHOW CREATE TABLE " +
                      object_display_name->render_hint + ";";
  return result;
}

SblrToSbsqlResult RenderTransactionSimpleControl(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  const bool exact_begin =
      IsOperation(envelope, "engine.op.txn_begin", "SBLR_TXN_BEGIN");
  const bool exact_commit =
      IsOperation(envelope, "engine.op.txn_commit", "SBLR_TXN_COMMIT");
  const bool exact_rollback =
      IsOperation(envelope, "engine.op.txn_rollback", "SBLR_TXN_ROLLBACK");
  if (!exact_begin && !exact_commit && !exact_rollback &&
      !RequireRenderFamily(envelope,
                           "source_preserving_transaction_control_v1",
                           &result)) {
    return result;
  }

  if (exact_begin) {
    const auto* operand = FindOperand(envelope, "options");
    SblrTransactionBeginOptionsV1 options;
    std::string detail;
    if (operand == nullptr ||
        !DecodeSblrTransactionBeginOptionsV1(
            operand->value_body.data(), operand->value_body.size(), &options,
            &detail) ||
        options.read_mode != 1 || options.authority_scope != 1) {
      return Refuse("SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
                    "SBLR-to-SBsql transaction begin rendering supports the exact local read-write profile only");
    }
    result.ok = true;
    result.sbsql_text = "BEGIN TRANSACTION;";
    return result;
  }

  if (IsAnyOperation(envelope, "transaction.begin", "SBLR_TRANSACTION_BEGIN",
                     "transaction.txn_begin", "SBLR_TXN_BEGIN")) {
    if (!RequireOperand(envelope, "session_context_uuid",
                        "SBLR-to-SBsql transaction begin conversion requires session context authority",
                        &result)) {
      return result;
    }
    result.ok = true;
    result.sbsql_text = "BEGIN TRANSACTION;";
    return result;
  }

  if (IsOperation(envelope, "transaction.set_characteristics",
                  "SBLR_TRANSACTION_SET_CHARACTERISTICS")) {
    if (!RequireOperand(envelope, "session_context_uuid",
                        "SBLR-to-SBsql transaction characteristics conversion requires session context authority",
                        &result) ||
        !RequireSemanticOperand(envelope, "transaction_read_mode",
                                "SBLR-to-SBsql transaction characteristics conversion requires read mode semantics",
                                &result)) {
      return result;
    }
    const auto mode = OperandValue(envelope, "transaction_read_mode");
    if (mode == "read_write") {
      result.ok = true;
      result.sbsql_text = "SET TRANSACTION READ WRITE;";
      return result;
    }
    if (mode == "read_only") {
      result.ok = true;
      result.sbsql_text = "SET TRANSACTION READ ONLY;";
      return result;
    }
    return Refuse("SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
                  "SBLR-to-SBsql transaction route supports read_write and read_only modes only");
  }

  if (exact_commit) {
    const auto* operand = FindOperand(envelope, "options");
    SblrTransactionCommitOptionsV1 options;
    std::string detail;
    if (operand == nullptr ||
        !DecodeSblrTransactionCommitOptionsV1(
            operand->value_body.data(), operand->value_body.size(), &options,
            &detail) ||
        options.commit_mode != 1 || options.authority_scope != 1) {
      return Refuse("SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
                    "SBLR-to-SBsql transaction commit rendering supports the exact local durable-normal profile only");
    }
    result.ok = true;
    result.sbsql_text = "COMMIT;";
    return result;
  }

  if (IsAnyOperation(envelope, "transaction.commit", "SBLR_TRANSACTION_COMMIT",
                     "transaction.txn_commit", "SBLR_TXN_COMMIT")) {
    if (!RequireOperand(envelope, "transaction_context_uuid",
                        "SBLR-to-SBsql transaction commit conversion requires transaction context authority",
                        &result)) {
      return result;
    }
    result.ok = true;
    result.sbsql_text = "COMMIT;";
    return result;
  }

  if (exact_rollback) {
    const auto* operand = FindOperand(envelope, "options");
    SblrTransactionRollbackOptionsV1 options;
    std::string detail;
    if (operand == nullptr ||
        !DecodeSblrTransactionRollbackOptionsV1(
            operand->value_body.data(), operand->value_body.size(), &options,
            &detail) ||
        options.rollback_mode != 1 || options.authority_scope != 1) {
      return Refuse("SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
                    "SBLR-to-SBsql transaction rollback rendering supports the exact local full-rollback profile only");
    }
    result.ok = true;
    result.sbsql_text = "ROLLBACK;";
    return result;
  }

  if (IsAnyOperation(envelope, "transaction.rollback", "SBLR_TRANSACTION_ROLLBACK",
                     "transaction.txn_rollback", "SBLR_TXN_ROLLBACK")) {
    if (!RequireOperand(envelope, "transaction_context_uuid",
                        "SBLR-to-SBsql transaction rollback conversion requires transaction context authority",
                        &result)) {
      return result;
    }
    result.ok = true;
    result.sbsql_text = "ROLLBACK;";
    return result;
  }

  return Refuse("SB_SBLR_TO_SBSQL_UNSUPPORTED_OPERATION",
                "SBLR-to-SBsql transaction route supports begin set-characteristics commit rollback and savepoint operations only");
}

SblrToSbsqlResult RenderTransactionSavepoint(const SblrOperationEnvelope& envelope) {
  SblrToSbsqlResult result;
  const bool exact_create = IsOperation(
      envelope, "engine.op.txn_savepoint", "SBLR_TXN_SAVEPOINT");
  const bool exact_release = IsOperation(
      envelope, "engine.op.txn_release_savepoint",
      "SBLR_TXN_RELEASE_SAVEPOINT");
  const bool exact_rollback_to = IsOperation(
      envelope, "engine.op.txn_rollback_to_savepoint",
      "SBLR_TXN_ROLLBACK_TO_SAVEPOINT");
  const bool exact_savepoint =
      exact_create || exact_release || exact_rollback_to;
  if (!exact_savepoint &&
      (!RequireRenderFamily(
           envelope, "source_preserving_transaction_control_v1", &result) ||
       !RequireOperand(
           envelope, "transaction_context_uuid",
           "SBLR-to-SBsql transaction conversion requires transaction context authority",
           &result) ||
       !RequireSymbolAuthorityMatch(envelope, "label",
                                    "savepoint_authority_uuid",
                                    "transaction savepoint", &result))) {
    return result;
  }

  const auto* savepoint = RequiredSymbol(envelope, "label", &result);
  if (!result.diagnostics.empty()) return result;
  if (exact_savepoint) {
    const auto authority = DecodeExactSavepointRenderAuthority(envelope);
    if (!authority.has_value()) {
      return Refuse(
          "SB_SBLR_TO_SBSQL_OPERAND_UNSUPPORTED",
          "SBLR-to-SBsql savepoint rendering requires the exact typed engine authority carrier");
    }
    if (savepoint->resolved_uuid != FormatUuid(authority->savepoint_uuid)) {
      return Refuse(
          "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
          "transaction savepoint source artifact UUID does not match the typed savepoint authority");
    }
  }
  if (!RequireRenderedIdentifier(savepoint->render_hint, "savepoint",
                                 &result)) {
    return result;
  }

  std::string prefix;
  if (exact_create ||
      IsAnyOperation(envelope, "transaction.create_savepoint",
                     "SBLR_TRANSACTION_CREATE_SAVEPOINT",
                     "transaction.savepoint.create",
                     "SBLR_TXN_SAVEPOINT")) {
    prefix = "SAVEPOINT ";
  } else if (exact_release ||
             IsAnyOperation(envelope, "transaction.release_savepoint",
                            "SBLR_TRANSACTION_RELEASE_SAVEPOINT",
                            "transaction.savepoint.release",
                            "SBLR_TXN_RELEASE_SAVEPOINT")) {
    prefix = "RELEASE SAVEPOINT ";
  } else if (exact_rollback_to ||
             IsAnyOperation(envelope, "transaction.rollback_to_savepoint",
                            "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT",
                            "transaction.savepoint.rollback_to",
                            "SBLR_TXN_ROLLBACK_TO_SAVEPOINT")) {
    prefix = "ROLLBACK TO SAVEPOINT ";
  } else {
    return Refuse("SB_SBLR_TO_SBSQL_UNSUPPORTED_OPERATION",
                  "SBLR-to-SBsql transaction route supports savepoint operations only");
  }

  result.ok = true;
  result.sbsql_text = prefix + savepoint->render_hint + ";";
  return result;
}

SblrToSbsqlResult RefuseKnownOperationWithoutRenderContract(
    const SblrOperationEnvelope& envelope) {
  const auto* entry = LookupSblrOperation(envelope.operation_id);
  if (entry == nullptr) {
    return Refuse("SB_SBLR_TO_SBSQL_UNSUPPORTED_OPERATION",
                  "SBLR-to-SBsql conversion does not support this operation family");
  }
  if (entry->support == SblrOpcodeSupport::cluster_refusal ||
      entry->requires_cluster_authority ||
      entry->category == SblrOpcodeCategory::cluster) {
    return Refuse("SB_SBLR_TO_SBSQL_NON_CORE_OPERATION_REFUSED",
                  "SBLR-to-SBsql source-preserving conversion refuses non-core cluster operation routes");
  }
  if (entry->category == SblrOpcodeCategory::extensibility) {
    return Refuse("SB_SBLR_TO_SBSQL_OPTIONAL_PROVIDER_OPERATION_REFUSED",
                  "SBLR-to-SBsql source-preserving conversion refuses optional-provider operation routes");
  }
  return Refuse("SB_SBLR_TO_SBSQL_NO_SOURCE_PRESERVING_RENDER_CONTRACT",
                "Known SBLR operation has no source-preserving SBsql render contract; raw SQL text cannot be used as authority");
}

}  // namespace

SblrToSbsqlResult RenderSblrEnvelopeToSbsql(const SblrOperationEnvelope& envelope,
                                            const SblrToSbsqlOptions& options) {
  if (!options.source_preserving) {
    return Refuse("SB_SBLR_TO_SBSQL_POLICY_REFUSED",
                  "SBLR-to-SBsql conversion requires source-preserving policy");
  }

  SblrToSbsqlResult result;
  auto operation = envelope;
  operation.source_artifact_map = {};
  const auto validation = ValidateSblrEnvelope(operation);
  if (!validation.ok) {
    CopyValidationDiagnostics(validation, &result);
    return result;
  }

  if (!ValidateSourcePolicy(envelope, &result)) return result;

  if (IsOperation(envelope, "general.procedural_operation",
                  "SBLR_GENERAL_PROCEDURAL_OPERATION")) {
    return RenderProceduralBundle(envelope);
  }
  if (IsAnyOperation(envelope, "dml.insert_rows", "SBLR_DML_INSERT_ROWS",
                     "engine.op.insert", "SBLR_INSERT") ||
      IsOperation(envelope, "dml.select_rows", "SBLR_DML_SELECT_ROWS") ||
      IsAnyOperation(envelope, "dml.update_rows", "SBLR_DML_UPDATE_ROWS",
                     "engine.op.update", "SBLR_UPDATE") ||
      IsAnyOperation(envelope, "dml.delete_rows", "SBLR_DML_DELETE_ROWS",
                     "engine.op.delete", "SBLR_DELETE")) {
    return RenderDmlSingleRow(envelope);
  }
  if (IsOperation(envelope, "ddl.create_table", "SBLR_DDL_CREATE_TABLE")) {
    return RenderDdlCreateTable(envelope);
  }
  if (IsOperation(envelope, "ddl.create_index", "SBLR_DDL_CREATE_INDEX")) {
    return RenderDdlCreateIndex(envelope);
  }
  if (IsOperation(envelope, "query.evaluate_projection",
                  "SBLR_QUERY_EVALUATE_PROJECTION")) {
    return RenderQueryProjection(envelope);
  }
  if (IsOperation(envelope, "catalog.get_descriptor",
                  "SBLR_CATALOG_GET_DESCRIPTOR")) {
    return RenderCatalogGetDescriptor(envelope);
  }
  if (IsOperation(envelope, "engine.op.txn_begin", "SBLR_TXN_BEGIN") ||
      IsAnyOperation(envelope, "transaction.begin", "SBLR_TRANSACTION_BEGIN",
                     "transaction.txn_begin", "SBLR_TXN_BEGIN") ||
      IsOperation(envelope, "transaction.set_characteristics",
                  "SBLR_TRANSACTION_SET_CHARACTERISTICS") ||
      IsOperation(envelope, "engine.op.txn_commit", "SBLR_TXN_COMMIT") ||
      IsAnyOperation(envelope, "transaction.commit", "SBLR_TRANSACTION_COMMIT",
                     "transaction.txn_commit", "SBLR_TXN_COMMIT") ||
      IsOperation(envelope, "engine.op.txn_rollback", "SBLR_TXN_ROLLBACK") ||
      IsAnyOperation(envelope, "transaction.rollback", "SBLR_TRANSACTION_ROLLBACK",
                     "transaction.txn_rollback", "SBLR_TXN_ROLLBACK")) {
    return RenderTransactionSimpleControl(envelope);
  }
  if (IsOperation(envelope, "transaction.create_savepoint",
                  "SBLR_TRANSACTION_CREATE_SAVEPOINT") ||
      IsOperation(envelope, "transaction.release_savepoint",
                  "SBLR_TRANSACTION_RELEASE_SAVEPOINT") ||
      IsOperation(envelope, "transaction.rollback_to_savepoint",
                  "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT") ||
      IsOperation(envelope, "transaction.savepoint.create",
                  "SBLR_TXN_SAVEPOINT") ||
      IsOperation(envelope, "transaction.savepoint.release",
                  "SBLR_TXN_RELEASE_SAVEPOINT") ||
      IsOperation(envelope, "transaction.savepoint.rollback_to",
                  "SBLR_TXN_ROLLBACK_TO_SAVEPOINT") ||
      IsOperation(envelope, "engine.op.txn_savepoint",
                  "SBLR_TXN_SAVEPOINT") ||
      IsOperation(envelope, "engine.op.txn_release_savepoint",
                  "SBLR_TXN_RELEASE_SAVEPOINT") ||
      IsOperation(envelope, "engine.op.txn_rollback_to_savepoint",
                  "SBLR_TXN_ROLLBACK_TO_SAVEPOINT")) {
    return RenderTransactionSavepoint(envelope);
  }

  return RefuseKnownOperationWithoutRenderContract(envelope);
}

namespace {

SblrToSbsqlResult RenderBoundSourceArtifact(
    const scratchbird::engine::SblrCanonicalContainer& container,
    const std::vector<std::uint8_t>& artifact_bytes,
    bool external_reference,
    const SblrSourceArtifactUuidV1& expected_sblr_envelope_uuid,
    const SblrSourceArtifactUuidV1& expected_artifact_uuid,
    std::uint16_t expected_redaction_class,
    const SblrToSbsqlOptions& options) {
  const auto payload_kind = scratchbird::engine::SblrReadU16(
      container.canonical_anchor.data() + 100);
  const std::string operation_bytes(
      reinterpret_cast<const char*>(container.operation_payload.data()),
      container.operation_payload.size());
  SblrOperationEnvelope operation;
  if (payload_kind == 2) {
    auto decoded_operation = DecodeSblrEnvelope(operation_bytes);
    if (!decoded_operation.ok) {
      SblrToSbsqlResult result;
      CopyValidationDiagnostics(
          SblrEnvelopeValidationResult{
              .ok = false,
              .diagnostics = std::move(decoded_operation.diagnostics)},
          &result);
      return result;
    }
    operation = std::move(decoded_operation.envelope);
  } else if (payload_kind == 1) {
    auto decoded_stream = DecodeSblrOpcodeStream(operation_bytes);
    if (!decoded_stream.ok) {
      return Refuse(decoded_stream.diagnostic_id.empty()
                        ? "SBLR.OPERAND_INVALID"
                        : decoded_stream.diagnostic_id,
                    decoded_stream.detail.empty()
                        ? "canonical opcode stream decoding failed"
                        : decoded_stream.detail);
    }
    if (decoded_stream.stream.operations.size() != 3) {
      return Refuse(
          "SBLR.SOURCE_ARTIFACT.INVALID",
          "source_artifact.opcode_stream_node_profile_unsupported");
    }
    operation = std::move(decoded_stream.stream.operations[1]);
  } else {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.operation_payload_kind_unsupported");
  }

  const auto decoded_artifact = DecodeSblrSourceArtifactMapV1(
      artifact_bytes.data(), artifact_bytes.size());
  if (decoded_artifact.status != SblrSourceArtifactDecodeStatusV1::ok) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact." + decoded_artifact.detail);
  }
  if (external_reference &&
      (decoded_artifact.artifact.artifact_uuid != expected_artifact_uuid ||
       static_cast<std::uint16_t>(decoded_artifact.artifact.redaction_class) !=
           expected_redaction_class)) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.semantic_reference_mismatch");
  }

  SblrSourceArtifactValidationContextV1 validation_context;
  validation_context.operation_validated_without_artifact = true;
  validation_context.source_preserving_requested = true;
  if (external_reference) {
    validation_context.expected_sblr_envelope_uuid =
        expected_sblr_envelope_uuid;
  } else {
    std::copy_n(container.canonical_anchor.data() + 116, 16,
                validation_context.expected_container_request_uuid.begin());
  }
  std::copy_n(container.canonical_anchor.data() + 16, 16,
              validation_context.expected_dialect_family_uuid.begin());
  std::copy_n(container.canonical_anchor.data() + 32, 16,
              validation_context.expected_parser_package_uuid.begin());
  SblrSourceArtifactUuidV1 operation_parser_uuid{};
  if (!ParseUuid(operation.parser_package_uuid, &operation_parser_uuid) ||
      operation_parser_uuid !=
          validation_context.expected_parser_package_uuid) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.binding");
  }

  validation_context.admitted_node_ids.push_back(1);
  for (const auto& operand : operation.operands) {
    validation_context.admitted_node_ids.push_back(
        static_cast<std::uint64_t>(operand.ordinal) + 1U);
    if (!IsObjectAuthorityOperand(operand.name)) continue;
    SblrSourceArtifactUuidV1 object_uuid{};
    if (ParseUuid(OperandValue(operation, operand.name), &object_uuid) &&
        std::find(validation_context.admitted_object_uuids.begin(),
                  validation_context.admitted_object_uuids.end(),
                  object_uuid) ==
            validation_context.admitted_object_uuids.end()) {
      validation_context.admitted_object_uuids.push_back(object_uuid);
    }
  }
  std::sort(validation_context.admitted_node_ids.begin(),
            validation_context.admitted_node_ids.end());
  validation_context.admitted_node_ids.erase(
      std::unique(validation_context.admitted_node_ids.begin(),
                  validation_context.admitted_node_ids.end()),
      validation_context.admitted_node_ids.end());

  std::string artifact_detail;
  if (!ValidateSblrSourceArtifactMapV1(decoded_artifact.artifact,
                                       validation_context,
                                       &artifact_detail)) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact." + artifact_detail);
  }
  operation.source_artifact_map =
      ToLegacySourceArtifact(operation, decoded_artifact.artifact);
  return RenderSblrEnvelopeToSbsql(operation, options);
}

}  // namespace

SblrToSbsqlResult RenderSblrContainerToSbsql(
    const std::uint8_t* data,
    std::size_t size,
    const SblrToSbsqlOptions& options) {
  if (!options.source_preserving) {
    return Refuse("SB_SBLR_TO_SBSQL_POLICY_REFUSED",
                  "SBLR-to-SBsql conversion requires source-preserving policy");
  }
  const auto decoded_container =
      scratchbird::engine::DecodeSblrContainerBytes(data, size);
  if (decoded_container.status != scratchbird::engine::SblrCodecStatus::ok) {
    return Refuse(
        decoded_container.diagnostic_code.empty()
            ? "SBLR.ENVELOPE.INVALID"
            : std::string(decoded_container.diagnostic_code),
        decoded_container.message_key.empty()
            ? "canonical SBLR container decoding failed"
            : std::string(decoded_container.message_key));
  }
  const auto& container = decoded_container.container;
  if (container.source_map.empty()) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.absent");
  }
  if (container.lowering_metadata.size() >= 4 &&
      std::equal(container.lowering_metadata.begin(),
                 container.lowering_metadata.begin() + 4, "SAM1")) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.channel_duplicate_or_misplaced");
  }
  return RenderBoundSourceArtifact(container, container.source_map, false,
                                   {}, {}, 0, options);
}

SblrToSbsqlResult RenderSblrExternalSourceArtifactToSbsql(
    const std::uint8_t* container_data,
    std::size_t container_size,
    const std::uint8_t* execution_envelope_data,
    std::size_t execution_envelope_size,
    const std::uint8_t* artifact_data,
    std::size_t artifact_size,
    const SblrToSbsqlOptions& options) {
  if (!options.source_preserving) {
    return Refuse("SB_SBLR_TO_SBSQL_POLICY_REFUSED",
                  "SBLR-to-SBsql conversion requires source-preserving policy");
  }
  if (artifact_data == nullptr || artifact_size == 0) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.absent");
  }
  const auto decoded_container = scratchbird::engine::DecodeSblrContainerBytes(
      container_data, container_size);
  if (decoded_container.status != scratchbird::engine::SblrCodecStatus::ok) {
    return Refuse(
        decoded_container.diagnostic_code.empty()
            ? "SBLR.ENVELOPE.INVALID"
            : std::string(decoded_container.diagnostic_code),
        decoded_container.message_key.empty()
            ? "canonical SBLR container decoding failed"
            : std::string(decoded_container.message_key));
  }
  const auto& container = decoded_container.container;
  if (!container.source_map.empty()) {
    return Refuse("SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY",
                  "source_artifact.duplicate_channel");
  }
  if (container.lowering_metadata.size() >= 4 &&
      std::equal(container.lowering_metadata.begin(),
                 container.lowering_metadata.begin() + 4, "SAM1")) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.channel_duplicate_or_misplaced");
  }
  const auto decoded_ingress =
      scratchbird::engine::DecodeSblrExecutionEnvelopeV1Bytes(
          execution_envelope_data, execution_envelope_size);
  scratchbird::engine::SblrExecutionEnvelopeSemanticView ingress;
  if (decoded_ingress.status != scratchbird::engine::SblrCodecStatus::ok ||
      !scratchbird::engine::SblrValidateExecutionEnvelopeFields(
          decoded_ingress.envelope, &ingress) ||
      !ingress.source_artifact_present ||
      ingress.source_artifact_ref_kind != 4) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.external_reference_invalid");
  }
  const auto* operation_data =
      ingress.payload_kind == scratchbird::engine::SblrPayloadKind::opcode_stream
          ? ingress.opcode_inline_data
          : ingress.operation_inline_data;
  const auto operation_size =
      ingress.payload_kind == scratchbird::engine::SblrPayloadKind::opcode_stream
          ? ingress.opcode_inline_size
          : ingress.operation_inline_size;
  const auto anchor_kind = static_cast<scratchbird::engine::SblrPayloadKind>(
      scratchbird::engine::SblrReadU16(container.canonical_anchor.data() + 100));
  if (operation_data == nullptr || anchor_kind != ingress.payload_kind ||
      operation_size != container.operation_payload.size() ||
      !std::equal(container.operation_payload.begin(),
                  container.operation_payload.end(), operation_data)) {
    return Refuse("SBLR.ENVELOPE.CHECKSUM_MISMATCH",
                  "source_artifact.operation_payload_binding_mismatch");
  }
  const auto artifact_crc =
      scratchbird::engine::SblrCrc32c(artifact_data, artifact_size);
  bool checksum_matches = false;
  if (ingress.source_artifact_checksum_kind == 1) {
    checksum_matches =
        ingress.source_artifact_checksum_crc32c == artifact_crc;
  } else if (ingress.source_artifact_checksum_kind == 2) {
    checksum_matches =
        ingress.source_artifact_checksum_sha256 ==
        HashSblrSourceArtifactBytesV1(artifact_data, artifact_size);
  }
  if (ingress.source_artifact_declared_size != artifact_size ||
      ingress.source_artifact_crc32c != artifact_crc || !checksum_matches) {
    return Refuse("SBLR.SOURCE_ARTIFACT.INVALID",
                  "source_artifact.reference_or_checksum_mismatch");
  }
  SblrSourceArtifactUuidV1 sblr_envelope_uuid{};
  std::copy_n(decoded_ingress.envelope.fields[0].data(),
              sblr_envelope_uuid.size(), sblr_envelope_uuid.begin());
  std::vector<std::uint8_t> artifact_bytes(
      artifact_data, artifact_data + artifact_size);
  return RenderBoundSourceArtifact(
      container, artifact_bytes, true, sblr_envelope_uuid,
      ingress.source_artifact_uuid, ingress.source_artifact_redaction_class,
      options);
}

}  // namespace scratchbird::engine::sblr
