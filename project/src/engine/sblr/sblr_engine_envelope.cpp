// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

SblrEnvelopeDiagnostic Diagnostic(std::string code, std::string message, bool error = true) {
  return SblrEnvelopeDiagnostic{std::move(code), std::move(message), error};
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

std::string Trim(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r')) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

bool ParseBool(std::string_view value) {
  return value == "true" || value == "1" || value == "yes";
}

bool IsAllowedSourceArtifactPolicy(std::string_view value) {
  return value == "absent" || value == "non_authoritative_render_metadata" ||
         value == "redacted_render_metadata";
}

bool IsAllowedSourceSymbolKind(std::string_view value) {
  constexpr std::array<std::string_view, 13> kKinds = {
      "variable",
      "parameter",
      "cursor",
      "label",
      "block_name",
      "routine",
      "routine_argument",
      "exception_handler",
      "cte",
      "relation_alias",
      "column_alias",
      "object_display_name",
      "generated_temp"};
  for (const auto kind : kKinds) {
    if (value == kind) return true;
  }
  return false;
}

bool IsAllowedOperationRenderHintKind(std::string_view value) {
  constexpr std::array<std::string_view, 6> kKinds = {
      "operation", "clause", "result_shape", "format", "ordering", "diagnostic"};
  for (const auto kind : kKinds) {
    if (value == kind) return true;
  }
  return false;
}

std::string EscapeOperandField(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch; break;
    }
  }
  return out;
}

std::string UnescapeOperandField(std::string_view value) {
  std::string out;
  bool escaped = false;
  for (const char ch : value) {
    if (!escaped) {
      if (ch == '\\') {
        escaped = true;
      } else {
        out += ch;
      }
      continue;
    }
    switch (ch) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case '\\': out += '\\'; break;
      default:
        out += '\\';
        out += ch;
        break;
    }
    escaped = false;
  }
  if (escaped) out += '\\';
  return out;
}

void AssignOperand(SblrOperationEnvelope* envelope, std::string_view value) {
  const auto first = value.find('\t');
  if (first == std::string_view::npos) return;
  const auto second = value.find('\t', first + 1);
  if (second == std::string_view::npos) return;
  SblrOperand operand;
  operand.type = UnescapeOperandField(value.substr(0, first));
  operand.name = UnescapeOperandField(value.substr(first + 1, second - first - 1));
  operand.value = UnescapeOperandField(value.substr(second + 1));
  envelope->operands.push_back(std::move(operand));
}

void AssignSourceSymbol(SblrOperationEnvelope* envelope, std::string_view value) {
  std::array<std::string_view, 8> fields{};
  std::size_t field_index = 0;
  std::size_t start = 0;
  while (field_index + 1 < fields.size()) {
    const auto tab = value.find('\t', start);
    if (tab == std::string_view::npos) break;
    fields[field_index++] = value.substr(start, tab - start);
    start = tab + 1;
  }
  fields[field_index++] = value.substr(start);

  SblrSourceSymbolArtifact artifact;
  if (field_index == fields.size()) {
    artifact.symbol_kind = UnescapeOperandField(fields[0]);
    artifact.stable_key = UnescapeOperandField(fields[1]);
    artifact.resolved_uuid = UnescapeOperandField(fields[2]);
    artifact.render_hint = UnescapeOperandField(fields[3]);
    artifact.scope = UnescapeOperandField(fields[4]);
    artifact.source_hash = UnescapeOperandField(fields[5]);
    artifact.authoritative = ParseBool(UnescapeOperandField(fields[6]));
    artifact.contains_sql_text = ParseBool(UnescapeOperandField(fields[7]));
  }
  envelope->source_artifact_map.symbols.push_back(std::move(artifact));
}

void AssignOperationRenderHint(SblrOperationEnvelope* envelope, std::string_view value) {
  std::array<std::string_view, 5> fields{};
  std::size_t field_index = 0;
  std::size_t start = 0;
  while (field_index + 1 < fields.size()) {
    const auto tab = value.find('\t', start);
    if (tab == std::string_view::npos) break;
    fields[field_index++] = value.substr(start, tab - start);
    start = tab + 1;
  }
  fields[field_index++] = value.substr(start);

  SblrOperationRenderHint hint;
  if (field_index == fields.size()) {
    hint.hint_kind = UnescapeOperandField(fields[0]);
    hint.stable_key = UnescapeOperandField(fields[1]);
    hint.value = UnescapeOperandField(fields[2]);
    hint.authoritative = ParseBool(UnescapeOperandField(fields[3]));
    hint.contains_sql_text = ParseBool(UnescapeOperandField(fields[4]));
  }
  envelope->source_artifact_map.operation_render_hints.push_back(std::move(hint));
}

void AssignNamedTextOperand(SblrOperationEnvelope* envelope,
                            std::string name,
                            std::string_view value) {
  SblrOperand operand;
  operand.type = "text";
  operand.name = std::move(name);
  operand.value = Trim(value);
  envelope->operands.push_back(std::move(operand));
}

void AssignEnvelopeField(SblrOperationEnvelope* envelope, std::string_view key, std::string_view value) {
  const std::string trimmed = Trim(value);
  if (key == "operation_id") envelope->operation_id = trimmed;
  else if (key == "opcode") envelope->opcode = trimmed;
  else if (key == "result_shape") envelope->result_shape = trimmed;
  else if (key == "diagnostic_shape") envelope->diagnostic_shape = trimmed;
  else if (key == "parser_package_uuid") envelope->parser_package_uuid = trimmed;
  else if (key == "registry_snapshot_uuid") envelope->registry_snapshot_uuid = trimmed;
  else if (key == "trace_key") envelope->trace_key = trimmed;
  else if (key == "contains_sql_text") envelope->contains_sql_text = ParseBool(trimmed);
  else if (key == "parser_resolved_names_to_uuids") envelope->parser_resolved_names_to_uuids = ParseBool(trimmed);
  else if (key == "requires_security_context") envelope->requires_security_context = ParseBool(trimmed);
  else if (key == "requires_transaction_context") envelope->requires_transaction_context = ParseBool(trimmed);
  else if (key == "requires_cluster_authority") envelope->requires_cluster_authority = ParseBool(trimmed);
  else if (key == "source_artifact_policy_status") envelope->source_artifact_map.policy_status = trimmed;
  else if (key == "source_artifact_identity") envelope->source_artifact_map.source_identity = UnescapeOperandField(trimmed);
  else if (key == "source_artifact_hash") envelope->source_artifact_map.source_hash = UnescapeOperandField(trimmed);
  else if (key == "source_artifact_format") envelope->source_artifact_map.artifact_format = UnescapeOperandField(trimmed);
  else if (key == "source_artifact_render_metadata_only") envelope->source_artifact_map.render_metadata_only = ParseBool(trimmed);
  else if (key == "source_artifact_contains_sql_text") envelope->source_artifact_map.contains_sql_text = ParseBool(trimmed);
  else if (key == "source_artifact_raw_sql_text_authoritative") envelope->source_artifact_map.raw_sql_text_authoritative = ParseBool(trimmed);
  else if (key == "source_symbol") AssignSourceSymbol(envelope, value);
  else if (key == "source_operation_render_hint") AssignOperationRenderHint(envelope, value);
  else if (key == "operand") AssignOperand(envelope, value);
  else if (key == "savepoint_name") AssignNamedTextOperand(envelope, "savepoint_name", value);
}

bool HasSourceArtifactMetadata(const SblrSourceArtifactMap& source_artifact_map) {
  return source_artifact_map.policy_status != "absent" ||
         !source_artifact_map.source_identity.empty() ||
         !source_artifact_map.source_hash.empty() ||
         !source_artifact_map.symbols.empty() ||
         !source_artifact_map.operation_render_hints.empty() ||
         source_artifact_map.contains_sql_text ||
         source_artifact_map.raw_sql_text_authoritative ||
         !source_artifact_map.render_metadata_only;
}

void ValidateSourceArtifactMap(const SblrSourceArtifactMap& source_artifact_map,
                               SblrEnvelopeValidationResult* result) {
  const bool has_metadata = HasSourceArtifactMetadata(source_artifact_map);
  if (!has_metadata) return;

  if (!IsAllowedSourceArtifactPolicy(source_artifact_map.policy_status)) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_ARTIFACT_POLICY_INVALID",
                                             "SBLR source artifact policy status is invalid"));
  }
  if (source_artifact_map.policy_status == "absent") {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_ARTIFACT_POLICY_REQUIRED",
                                             "SBLR source artifacts require an explicit non-authoritative policy"));
  }
  if (source_artifact_map.source_identity.empty() &&
      source_artifact_map.source_hash.empty()) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_ARTIFACT_IDENTITY_REQUIRED",
                                             "SBLR source artifacts require a source identity or hash"));
  }
  if (source_artifact_map.contains_sql_text) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN",
                                             "SBLR source artifacts must not carry raw SQL text"));
  }
  if (source_artifact_map.raw_sql_text_authoritative ||
      !source_artifact_map.render_metadata_only) {
    result->ok = false;
    result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_ARTIFACT_AUTHORITY_FORBIDDEN",
                                             "SBLR source artifacts are render metadata only and cannot be authority"));
  }

  for (const auto& symbol : source_artifact_map.symbols) {
    if (!IsAllowedSourceSymbolKind(symbol.symbol_kind) ||
        symbol.stable_key.empty() ||
        symbol.render_hint.empty()) {
      result->ok = false;
      result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_SYMBOL_ARTIFACT_INVALID",
                                               "SBLR source symbol artifact metadata is invalid"));
    }
    if (symbol.authoritative) {
      result->ok = false;
      result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_SYMBOL_AUTHORITY_FORBIDDEN",
                                               "SBLR source symbol artifacts cannot be authority"));
    }
    if (symbol.contains_sql_text) {
      result->ok = false;
      result->diagnostics.push_back(Diagnostic("SB_SBLR_SOURCE_SYMBOL_SQL_TEXT_FORBIDDEN",
                                               "SBLR source symbol artifacts must not carry raw SQL text"));
    }
  }

  for (const auto& hint : source_artifact_map.operation_render_hints) {
    if (!IsAllowedOperationRenderHintKind(hint.hint_kind) ||
        hint.stable_key.empty() ||
        hint.value.empty()) {
      result->ok = false;
      result->diagnostics.push_back(Diagnostic("SB_SBLR_OPERATION_RENDER_HINT_INVALID",
                                               "SBLR operation render hint metadata is invalid"));
    }
    if (hint.authoritative) {
      result->ok = false;
      result->diagnostics.push_back(Diagnostic("SB_SBLR_OPERATION_RENDER_HINT_AUTHORITY_FORBIDDEN",
                                               "SBLR operation render hints cannot be authority"));
    }
    if (hint.contains_sql_text) {
      result->ok = false;
      result->diagnostics.push_back(Diagnostic("SB_SBLR_OPERATION_RENDER_HINT_SQL_TEXT_FORBIDDEN",
                                               "SBLR operation render hints must not carry raw SQL text"));
    }
  }
}

}  // namespace

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key) {
  SblrOperationEnvelope envelope;
  envelope.operation_id = std::move(operation_id);
  envelope.opcode = std::move(opcode);
  envelope.trace_key = std::move(trace_key);
  envelope.result_shape = "engine.api.result.v1";
  envelope.diagnostic_shape = "engine.diagnostic.v1";
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

SblrEnvelopeValidationResult ValidateSblrEnvelope(const SblrOperationEnvelope& envelope) {
  SblrEnvelopeValidationResult result;
  result.ok = true;

  if (envelope.envelope_major != kEngineSblrEnvelopeMajor) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_ENVELOPE_MAJOR_UNSUPPORTED",
                                            "SBLR envelope major version is unsupported"));
  }
  if (envelope.operation_id.empty()) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_OPERATION_ID_REQUIRED",
                                            "SBLR envelope operation_id is required"));
  }
  if (envelope.opcode.empty()) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_OPCODE_REQUIRED",
                                            "SBLR envelope opcode is required"));
  }
  if (envelope.contains_sql_text) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_SQL_TEXT_FORBIDDEN",
                                            "engine SBLR envelope must not contain SQL text"));
  }
  if (!envelope.parser_resolved_names_to_uuids) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_NAMES_NOT_RESOLVED_TO_UUIDS",
                                            "parser lowering must resolve names to UUIDs before engine dispatch"));
  }
  if (envelope.result_shape.empty()) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_RESULT_SHAPE_REQUIRED",
                                            "SBLR envelope result shape is required"));
  }
  if (envelope.diagnostic_shape.empty()) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic("SB_SBLR_DIAGNOSTIC_SHAPE_REQUIRED",
                                            "SBLR envelope diagnostic shape is required"));
  }
  ValidateSourceArtifactMap(envelope.source_artifact_map, &result);

  return result;
}

SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded) {
  SblrDecodeResult result;
  result.ok = true;
  result.envelope = {};

  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line = encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos) {
      AssignEnvelopeField(&result.envelope, Trim(line.substr(0, equals)), line.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }

  const auto validation = ValidateSblrEnvelope(result.envelope);
  result.ok = validation.ok;
  result.diagnostics = validation.diagnostics;
  return result;
}

std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope) {
  std::ostringstream out;
  out << "operation_id=" << envelope.operation_id << "\n";
  out << "opcode=" << envelope.opcode << "\n";
  out << "result_shape=" << envelope.result_shape << "\n";
  out << "diagnostic_shape=" << envelope.diagnostic_shape << "\n";
  out << "parser_package_uuid=" << envelope.parser_package_uuid << "\n";
  out << "registry_snapshot_uuid=" << envelope.registry_snapshot_uuid << "\n";
  out << "trace_key=" << envelope.trace_key << "\n";
  out << "contains_sql_text=" << (envelope.contains_sql_text ? "true" : "false") << "\n";
  out << "parser_resolved_names_to_uuids=" << (envelope.parser_resolved_names_to_uuids ? "true" : "false") << "\n";
  out << "requires_security_context=" << (envelope.requires_security_context ? "true" : "false") << "\n";
  out << "requires_transaction_context=" << (envelope.requires_transaction_context ? "true" : "false") << "\n";
  out << "requires_cluster_authority=" << (envelope.requires_cluster_authority ? "true" : "false") << "\n";
  out << "source_artifact_policy_status=" << envelope.source_artifact_map.policy_status << "\n";
  out << "source_artifact_identity=" << EscapeOperandField(envelope.source_artifact_map.source_identity) << "\n";
  out << "source_artifact_hash=" << EscapeOperandField(envelope.source_artifact_map.source_hash) << "\n";
  out << "source_artifact_format=" << EscapeOperandField(envelope.source_artifact_map.artifact_format) << "\n";
  out << "source_artifact_render_metadata_only=" << (envelope.source_artifact_map.render_metadata_only ? "true" : "false") << "\n";
  out << "source_artifact_contains_sql_text=" << (envelope.source_artifact_map.contains_sql_text ? "true" : "false") << "\n";
  out << "source_artifact_raw_sql_text_authoritative=" << (envelope.source_artifact_map.raw_sql_text_authoritative ? "true" : "false") << "\n";
  for (const auto& symbol : envelope.source_artifact_map.symbols) {
    out << "source_symbol=" << EscapeOperandField(symbol.symbol_kind) << "\t"
        << EscapeOperandField(symbol.stable_key) << "\t"
        << EscapeOperandField(symbol.resolved_uuid) << "\t"
        << EscapeOperandField(symbol.render_hint) << "\t"
        << EscapeOperandField(symbol.scope) << "\t"
        << EscapeOperandField(symbol.source_hash) << "\t"
        << (symbol.authoritative ? "true" : "false") << "\t"
        << (symbol.contains_sql_text ? "true" : "false") << "\n";
  }
  for (const auto& hint : envelope.source_artifact_map.operation_render_hints) {
    out << "source_operation_render_hint=" << EscapeOperandField(hint.hint_kind) << "\t"
        << EscapeOperandField(hint.stable_key) << "\t"
        << EscapeOperandField(hint.value) << "\t"
        << (hint.authoritative ? "true" : "false") << "\t"
        << (hint.contains_sql_text ? "true" : "false") << "\n";
  }
  for (const auto& operand : envelope.operands) {
    out << "operand=" << EscapeOperandField(operand.type) << "\t"
        << EscapeOperandField(operand.name) << "\t"
        << EscapeOperandField(operand.value) << "\n";
  }
  return out.str();
}

std::string SerializeSblrEnvelopeToJson(const SblrOperationEnvelope& envelope) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"envelope_major\": " << envelope.envelope_major << ",\n";
  out << "  \"envelope_minor\": " << envelope.envelope_minor << ",\n";
  out << "  \"operation_id\": \"" << JsonEscape(envelope.operation_id) << "\",\n";
  out << "  \"opcode\": \"" << JsonEscape(envelope.opcode) << "\",\n";
  out << "  \"contains_sql_text\": " << (envelope.contains_sql_text ? "true" : "false") << ",\n";
  out << "  \"parser_resolved_names_to_uuids\": " << (envelope.parser_resolved_names_to_uuids ? "true" : "false") << ",\n";
  out << "  \"requires_cluster_authority\": " << (envelope.requires_cluster_authority ? "true" : "false") << ",\n";
  out << "  \"trace_key\": \"" << JsonEscape(envelope.trace_key) << "\",\n";
  out << "  \"source_artifact_map\": {\n";
  out << "    \"policy_status\": \"" << JsonEscape(envelope.source_artifact_map.policy_status) << "\",\n";
  out << "    \"source_identity\": \"" << JsonEscape(envelope.source_artifact_map.source_identity) << "\",\n";
  out << "    \"source_hash\": \"" << JsonEscape(envelope.source_artifact_map.source_hash) << "\",\n";
  out << "    \"artifact_format\": \"" << JsonEscape(envelope.source_artifact_map.artifact_format) << "\",\n";
  out << "    \"render_metadata_only\": " << (envelope.source_artifact_map.render_metadata_only ? "true" : "false") << ",\n";
  out << "    \"source_artifact_contains_sql_text\": " << (envelope.source_artifact_map.contains_sql_text ? "true" : "false") << ",\n";
  out << "    \"raw_sql_text_authoritative\": " << (envelope.source_artifact_map.raw_sql_text_authoritative ? "true" : "false") << ",\n";
  out << "    \"symbols\": [\n";
  for (std::size_t i = 0; i < envelope.source_artifact_map.symbols.size(); ++i) {
    const auto& symbol = envelope.source_artifact_map.symbols[i];
    out << "      {\"symbol_kind\": \"" << JsonEscape(symbol.symbol_kind)
        << "\", \"stable_key\": \"" << JsonEscape(symbol.stable_key)
        << "\", \"resolved_uuid\": \"" << JsonEscape(symbol.resolved_uuid)
        << "\", \"render_hint\": \"" << JsonEscape(symbol.render_hint)
        << "\", \"scope\": \"" << JsonEscape(symbol.scope)
        << "\", \"source_hash\": \"" << JsonEscape(symbol.source_hash)
        << "\", \"authoritative\": " << (symbol.authoritative ? "true" : "false")
        << ", \"symbol_contains_sql_text\": " << (symbol.contains_sql_text ? "true" : "false") << "}";
    if (i + 1 != envelope.source_artifact_map.symbols.size()) out << ",";
    out << "\n";
  }
  out << "    ],\n";
  out << "    \"operation_render_hints\": [\n";
  for (std::size_t i = 0; i < envelope.source_artifact_map.operation_render_hints.size(); ++i) {
    const auto& hint = envelope.source_artifact_map.operation_render_hints[i];
    out << "      {\"hint_kind\": \"" << JsonEscape(hint.hint_kind)
        << "\", \"stable_key\": \"" << JsonEscape(hint.stable_key)
        << "\", \"value\": \"" << JsonEscape(hint.value)
        << "\", \"authoritative\": " << (hint.authoritative ? "true" : "false")
        << ", \"hint_contains_sql_text\": " << (hint.contains_sql_text ? "true" : "false") << "}";
    if (i + 1 != envelope.source_artifact_map.operation_render_hints.size()) out << ",";
    out << "\n";
  }
  out << "    ]\n";
  out << "  }\n";
  out << "}\n";
  return out.str();
}

std::string SerializeSblrValidationToJson(const SblrEnvelopeValidationResult& result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const auto& diagnostic = result.diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code) << "\", \"message\": \""
        << JsonEscape(diagnostic.message) << "\", \"error\": " << (diagnostic.error ? "true" : "false") << "}";
    if (i + 1 != result.diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::engine::sblr
