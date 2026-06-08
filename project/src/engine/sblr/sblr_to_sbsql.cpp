// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_to_sbsql.hpp"

#include "sblr_opcode_registry.hpp"

#include <cctype>
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
  return operand == nullptr ? std::string_view() : std::string_view(operand->value);
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
                                     "dml.insert", "SBLR_INSERT");
  const bool select = IsOperation(envelope, "dml.select_rows",
                                  "SBLR_DML_SELECT_ROWS");
  const bool update = IsAnyOperation(envelope, "dml.update_rows",
                                     "SBLR_DML_UPDATE_ROWS",
                                     "dml.update", "SBLR_UPDATE");
  const bool delete_row = IsAnyOperation(envelope, "dml.delete_rows",
                                         "SBLR_DML_DELETE_ROWS",
                                         "dml.delete", "SBLR_DELETE");
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
  if (!RequireRenderFamily(envelope, "source_preserving_transaction_control_v1",
                           &result)) {
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
  if (!RequireRenderFamily(envelope, "source_preserving_transaction_control_v1",
                           &result) ||
      !RequireOperand(envelope, "transaction_context_uuid",
                      "SBLR-to-SBsql transaction conversion requires transaction context authority",
                      &result) ||
      !RequireSymbolAuthorityMatch(envelope, "label",
                                   "savepoint_authority_uuid",
                                   "transaction savepoint",
                                   &result)) {
    return result;
  }

  const auto* savepoint = RequiredSymbol(envelope, "label", &result);
  if (!result.diagnostics.empty()) return result;
  if (!RequireIdentifier(savepoint->render_hint, "savepoint", &result)) {
    return result;
  }

  std::string prefix;
  if (IsAnyOperation(envelope, "transaction.create_savepoint",
                     "SBLR_TRANSACTION_CREATE_SAVEPOINT",
                     "transaction.savepoint.create",
                     "SBLR_TXN_SAVEPOINT")) {
    prefix = "SAVEPOINT ";
  } else if (IsAnyOperation(envelope, "transaction.release_savepoint",
                            "SBLR_TRANSACTION_RELEASE_SAVEPOINT",
                            "transaction.savepoint.release",
                            "SBLR_TXN_RELEASE_SAVEPOINT")) {
    prefix = "RELEASE SAVEPOINT ";
  } else if (IsAnyOperation(envelope, "transaction.rollback_to_savepoint",
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
  const auto validation = ValidateSblrEnvelope(envelope);
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
                     "dml.insert", "SBLR_INSERT") ||
      IsOperation(envelope, "dml.select_rows", "SBLR_DML_SELECT_ROWS") ||
      IsAnyOperation(envelope, "dml.update_rows", "SBLR_DML_UPDATE_ROWS",
                     "dml.update", "SBLR_UPDATE") ||
      IsAnyOperation(envelope, "dml.delete_rows", "SBLR_DML_DELETE_ROWS",
                     "dml.delete", "SBLR_DELETE")) {
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
  if (IsAnyOperation(envelope, "transaction.begin", "SBLR_TRANSACTION_BEGIN",
                     "transaction.txn_begin", "SBLR_TXN_BEGIN") ||
      IsOperation(envelope, "transaction.set_characteristics",
                  "SBLR_TRANSACTION_SET_CHARACTERISTICS") ||
      IsAnyOperation(envelope, "transaction.commit", "SBLR_TRANSACTION_COMMIT",
                     "transaction.txn_commit", "SBLR_TXN_COMMIT") ||
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
                  "SBLR_TXN_ROLLBACK_TO_SAVEPOINT")) {
    return RenderTransactionSavepoint(envelope);
  }

  return RefuseKnownOperationWithoutRenderContract(envelope);
}

}  // namespace scratchbird::engine::sblr
