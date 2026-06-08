// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "expression_index_extractor.hpp"

// SB-EXPRESSION-INDEX-EXTRACTOR-ANCHOR

#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool IsIdentStart(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
}

bool IsIdentBody(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
         ch == '$' || ch == '.';
}

bool IsNumberish(char ch) {
  return std::isdigit(static_cast<unsigned char>(ch));
}

enum class CanonTokenKind {
  identifier,
  number,
  string_literal,
  punctuation,
  operator_symbol
};

struct CanonToken {
  CanonTokenKind kind = CanonTokenKind::identifier;
  std::string text;
};

bool SpaceNeeded(const CanonToken& previous, const CanonToken& current) {
  if (current.text == ")" || current.text == "," ||
      current.kind == CanonTokenKind::operator_symbol) {
    return false;
  }
  if (previous.text == "(" || previous.text == "," ||
      previous.kind == CanonTokenKind::operator_symbol) {
    return false;
  }
  if (current.text == "(") {
    return false;
  }
  return (previous.kind == CanonTokenKind::identifier ||
          previous.kind == CanonTokenKind::number ||
          previous.kind == CanonTokenKind::string_literal) &&
         (current.kind == CanonTokenKind::identifier ||
          current.kind == CanonTokenKind::number ||
          current.kind == CanonTokenKind::string_literal);
}

std::string JoinCanonicalTokens(const std::vector<CanonToken>& tokens) {
  std::string out;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0 && SpaceNeeded(tokens[i - 1], tokens[i])) {
      out.push_back(' ');
    }
    out += tokens[i].text;
  }
  return out;
}

std::uint64_t Fnv1a64(std::string_view input) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : input) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = digits[value & 0x0f];
    value >>= 4;
  }
  return out;
}

std::string EscapeCanonical(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '|' || ch == '(' || ch == ')' || ch == ',') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  return out;
}

std::string UuidText(const TypedUuid& uuid) {
  if (!uuid.valid()) {
    return {};
  }
  return scratchbird::core::uuid::UuidToString(uuid.value);
}

std::string DescriptorSignature(const ExpressionIndexValueDescriptor& descriptor) {
  std::ostringstream out;
  out << "type=" << LowerAscii(descriptor.canonical_type_name)
      << ";encoded=" << descriptor.encoded_descriptor
      << ";type_uuid=" << UuidText(descriptor.type_descriptor_uuid)
      << ";collation_uuid=" << UuidText(descriptor.collation_uuid)
      << ";case_folded=" << (descriptor.case_folded ? "true" : "false");
  return out.str();
}

std::string NodeCanonical(const ExpressionIndexExpressionNode& node) {
  std::ostringstream out;
  switch (node.kind) {
    case ExpressionIndexNodeKind::literal:
      out << "literal(type=" << LowerAscii(node.literal.descriptor.canonical_type_name)
          << ",null=" << (node.literal.is_null ? "true" : "false")
          << ",value=" << EscapeCanonical(node.literal.encoded_value) << ")";
      break;
    case ExpressionIndexNodeKind::column_ref:
      out << "column(";
      if (!node.identifier.empty()) {
        out << "name=" << LowerAscii(node.identifier);
      }
      if (node.column_ordinal_valid) {
        if (!node.identifier.empty()) out << ",";
        out << "ordinal=" << node.column_ordinal;
      }
      out << ")";
      break;
    case ExpressionIndexNodeKind::operator_call:
      out << "operator(" << LowerAscii(node.identifier);
      for (const auto& argument : node.arguments) {
        out << "," << NodeCanonical(argument);
      }
      out << ")";
      break;
    case ExpressionIndexNodeKind::function_call:
      out << "function(" << LowerAscii(node.identifier);
      for (const auto& argument : node.arguments) {
        out << "," << NodeCanonical(argument);
      }
      out << ")";
      break;
  }
  return out.str();
}

ExpressionIndexNonAuthorityEvidence NonAuthorityEvidence() {
  return {};
}

void AddNonAuthorityEvidence(std::vector<std::string>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  evidence->push_back("expression_index_visibility_authority=false");
  evidence->push_back("expression_index_authorization_authority=false");
  evidence->push_back("expression_index_transaction_finality_authority=false");
  evidence->push_back("expression_index_cleanup_authority=false");
  evidence->push_back("expression_index_recovery_authority=false");
  evidence->push_back("expression_index_parser_or_donor_finality_authority=false");
}

ExpressionIndexCanonicalizationResult RefuseCanonicalization(std::string code,
                                                             std::string key,
                                                             std::string detail) {
  ExpressionIndexCanonicalizationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeExpressionIndexExtractorDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

ExpressionIndexBindResult RefuseBind(std::string code,
                                      std::string key,
                                      std::string detail) {
  ExpressionIndexBindResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeExpressionIndexExtractorDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

ExpressionIndexBatchExtractionResult RefuseBatch(std::string code,
                                                 std::string key,
                                                 std::string detail) {
  ExpressionIndexBatchExtractionResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeExpressionIndexExtractorDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.non_authority = NonAuthorityEvidence();
  AddNonAuthorityEvidence(&result.evidence);
  return result;
}

ExpressionIndexKeyEnvelopeValidationResult RefuseEnvelope(std::string code,
                                                          std::string key,
                                                          std::string detail) {
  ExpressionIndexKeyEnvelopeValidationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeExpressionIndexExtractorDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool SameOperationKind(ExpressionIndexOperationKind left,
                       ExpressionIndexOperationKind right) {
  return left == right;
}

const ExpressionIndexOperationProof* FindProof(
    const std::vector<ExpressionIndexOperationProof>& proofs,
    ExpressionIndexOperationKind kind,
    std::string_view operation_id) {
  const std::string normalized = LowerAscii(std::string(operation_id));
  for (const auto& proof : proofs) {
    if (SameOperationKind(proof.kind, kind) &&
        LowerAscii(proof.operation_id) == normalized) {
      return &proof;
    }
  }
  return nullptr;
}

bool ProofAccepted(const ExpressionIndexOperationProof& proof,
                   std::string* refusal_code,
                   std::string* refusal_key,
                   std::string* refusal_detail) {
  const std::string operation_id = LowerAscii(proof.operation_id);
  if (!proof.proof_present || proof.proof_digest.empty()) {
    *refusal_code = "SB-INDEX-EXPR-DETERMINISM-PROOF-MISSING";
    *refusal_key = "index.expression.determinism_proof_missing";
    *refusal_detail = operation_id;
    return false;
  }
  if (!proof.deterministic || proof.depends_on_time_or_random ||
      proof.depends_on_transaction_state || proof.depends_on_visibility_state ||
      proof.depends_on_authorization_state) {
    *refusal_code = "SB-INDEX-EXPR-NONDETERMINISTIC-OPERATION";
    *refusal_key = "index.expression.nondeterministic_operation";
    *refusal_detail = operation_id;
    return false;
  }
  if (proof.reads_external_resource && !proof.resource_epoch_bound) {
    *refusal_code = "SB-INDEX-EXPR-RESOURCE-PROOF-MISSING";
    *refusal_key = "index.expression.resource_proof_missing";
    *refusal_detail = operation_id;
    return false;
  }
  return true;
}

bool ExtractorSupportsOperation(ExpressionIndexOperationKind kind,
                                std::string_view operation_id) {
  const std::string op = LowerAscii(std::string(operation_id));
  if (kind == ExpressionIndexOperationKind::operator_call) {
    return op == "op_unary_minus" || op == "op_add" || op == "op_mul" ||
           op == "op_div" || op == "op_concat" || op == "op_eq";
  }
  return op == "fn.lower_ascii" || op == "fn.upper_ascii" ||
         op == "fn.length_text" || op == "fn.abs_i64" ||
         op == "fn.coalesce_text";
}

bool ValidateDeterminism(
    const ExpressionIndexExpressionNode& node,
    const std::vector<ExpressionIndexOperationProof>& proofs,
    std::string* refusal_code,
    std::string* refusal_key,
    std::string* refusal_detail) {
  if (node.kind == ExpressionIndexNodeKind::operator_call ||
      node.kind == ExpressionIndexNodeKind::function_call) {
    const auto kind = node.kind == ExpressionIndexNodeKind::operator_call
                          ? ExpressionIndexOperationKind::operator_call
                          : ExpressionIndexOperationKind::function_call;
    if (!ExtractorSupportsOperation(kind, node.identifier)) {
      *refusal_code = kind == ExpressionIndexOperationKind::operator_call
                          ? "SB-INDEX-EXPR-OPERATOR-UNSUPPORTED"
                          : "SB-INDEX-EXPR-FUNCTION-UNSUPPORTED";
      *refusal_key = kind == ExpressionIndexOperationKind::operator_call
                         ? "index.expression.operator_unsupported"
                         : "index.expression.function_unsupported";
      *refusal_detail = LowerAscii(node.identifier);
      return false;
    }
    const auto* proof = FindProof(proofs, kind, node.identifier);
    if (proof == nullptr) {
      *refusal_code = "SB-INDEX-EXPR-DETERMINISM-PROOF-MISSING";
      *refusal_key = "index.expression.determinism_proof_missing";
      *refusal_detail = LowerAscii(node.identifier);
      return false;
    }
    if (!ProofAccepted(*proof, refusal_code, refusal_key, refusal_detail)) {
      return false;
    }
  }
  for (const auto& argument : node.arguments) {
    if (!ValidateDeterminism(argument, proofs, refusal_code, refusal_key, refusal_detail)) {
      return false;
    }
  }
  return true;
}

std::string CacheKey(const std::string& expression_digest,
                     const ExpressionIndexEpochs& epochs,
                     const std::string& result_signature) {
  std::ostringstream out;
  out << expression_digest
      << "|resource_epoch=" << epochs.resource_epoch
      << "|collation_epoch=" << epochs.collation_epoch
      << "|function_resource_epoch=" << epochs.function_resource_epoch
      << "|result=" << result_signature;
  return out.str();
}

bool SameExpressionAndResult(const ExpressionIndexExtractorDescriptor& descriptor,
                             const std::string& expression_digest,
                             const std::string& result_signature) {
  return descriptor.expression_digest == expression_digest &&
         descriptor.result_type_descriptor_signature == result_signature;
}

bool ParseI64(std::string_view text, std::int64_t* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  std::size_t index = 0;
  bool negative = false;
  if (text[index] == '-' || text[index] == '+') {
    negative = text[index] == '-';
    ++index;
  }
  if (index >= text.size()) {
    return false;
  }
  std::uint64_t value = 0;
  for (; index < text.size(); ++index) {
    const char ch = text[index];
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    value = value * 10u + static_cast<unsigned>(ch - '0');
  }
  if (negative) {
    if (value > static_cast<std::uint64_t>(INT64_MAX) + 1ull) {
      return false;
    }
    *out = value == static_cast<std::uint64_t>(INT64_MAX) + 1ull
               ? INT64_MIN
               : -static_cast<std::int64_t>(value);
    return true;
  }
  if (value > static_cast<std::uint64_t>(INT64_MAX)) {
    return false;
  }
  *out = static_cast<std::int64_t>(value);
  return true;
}

std::vector<byte> I64Payload(std::int64_t value) {
  const auto sortable = static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::vector<byte> Bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

bool IsIntegerType(std::string_view type_name) {
  const std::string type = LowerAscii(std::string(type_name));
  return type == "bigint" || type == "int64" || type == "integer";
}

bool IsBooleanType(std::string_view type_name) {
  const std::string type = LowerAscii(std::string(type_name));
  return type == "boolean" || type == "bool";
}

bool IsBinaryType(std::string_view type_name) {
  const std::string type = LowerAscii(std::string(type_name));
  return type == "binary" || type == "varbinary" || type == "bytes";
}

struct EvaluationResult {
  Status status;
  bool ok_value = false;
  ExpressionIndexTypedValue value;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && ok_value; }
};

EvaluationResult EvaluationOk(ExpressionIndexTypedValue value) {
  EvaluationResult result;
  result.status = OkStatus();
  result.ok_value = true;
  result.value = std::move(value);
  return result;
}

EvaluationResult EvaluationNull(const ExpressionIndexValueDescriptor& descriptor) {
  ExpressionIndexTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  return EvaluationOk(std::move(value));
}

EvaluationResult EvaluationError(std::string code,
                                 std::string key,
                                 std::string detail) {
  EvaluationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeExpressionIndexExtractorDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

const ExpressionIndexColumnValue* FindColumn(const ExpressionIndexRowInput& row,
                                             const ExpressionIndexExpressionNode& node) {
  if (!node.identifier.empty()) {
    const std::string requested = LowerAscii(node.identifier);
    for (const auto& column : row.columns) {
      if (LowerAscii(column.name) == requested) {
        return &column;
      }
    }
  }
  if (node.column_ordinal_valid) {
    for (const auto& column : row.columns) {
      if (column.ordinal_valid && column.ordinal == node.column_ordinal) {
        return &column;
      }
    }
  }
  return nullptr;
}

ExpressionIndexValueDescriptor DescriptorForType(std::string type_name) {
  ExpressionIndexValueDescriptor descriptor;
  descriptor.canonical_type_name = std::move(type_name);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

ExpressionIndexTypedValue TextValue(std::string value) {
  ExpressionIndexTypedValue out;
  out.descriptor = DescriptorForType("text");
  out.encoded_value = std::move(value);
  return out;
}

ExpressionIndexTypedValue I64Value(std::int64_t value) {
  ExpressionIndexTypedValue out;
  out.descriptor = DescriptorForType("bigint");
  out.encoded_value = std::to_string(value);
  return out;
}

ExpressionIndexTypedValue BoolValue(bool value) {
  ExpressionIndexTypedValue out;
  out.descriptor = DescriptorForType("boolean");
  out.encoded_value = value ? "true" : "false";
  return out;
}

bool TruthValue(const ExpressionIndexTypedValue& value, bool* out) {
  if (out == nullptr || value.is_null) {
    return false;
  }
  const std::string lowered = LowerAscii(value.encoded_value);
  if (lowered == "true" || lowered == "1") {
    *out = true;
    return true;
  }
  if (lowered == "false" || lowered == "0") {
    *out = false;
    return true;
  }
  return false;
}

EvaluationResult EvaluateNode(const ExpressionIndexExpressionNode& node,
                              const ExpressionIndexRowInput& row);

std::vector<EvaluationResult> EvaluateArguments(
    const ExpressionIndexExpressionNode& node,
    const ExpressionIndexRowInput& row) {
  std::vector<EvaluationResult> arguments;
  arguments.reserve(node.arguments.size());
  for (const auto& argument : node.arguments) {
    arguments.push_back(EvaluateNode(argument, row));
    if (!arguments.back().ok()) {
      return arguments;
    }
  }
  return arguments;
}

bool AnyNull(const std::vector<EvaluationResult>& arguments) {
  for (const auto& argument : arguments) {
    if (argument.ok() && argument.value.is_null) {
      return true;
    }
  }
  return false;
}

EvaluationResult PropagateArgumentFailure(const std::vector<EvaluationResult>& arguments) {
  for (const auto& argument : arguments) {
    if (!argument.ok()) {
      return argument;
    }
  }
  return EvaluationError("SB-INDEX-EXPR-EVALUATION-FAILED",
                         "index.expression.evaluation_failed",
                         "argument_failure");
}

EvaluationResult EvaluateOperator(const ExpressionIndexExpressionNode& node,
                                  const ExpressionIndexRowInput& row) {
  const std::string op = LowerAscii(node.identifier);
  const auto arguments = EvaluateArguments(node, row);
  for (const auto& argument : arguments) {
    if (!argument.ok()) {
      return PropagateArgumentFailure(arguments);
    }
  }

  auto arity_error = [&] {
    return EvaluationError("SB-INDEX-EXPR-ARITY-MISMATCH",
                           "index.expression.arity_mismatch",
                           op);
  };

  if (op == "op_unary_minus") {
    if (arguments.size() != 1) return arity_error();
    if (AnyNull(arguments)) return EvaluationNull(arguments.front().value.descriptor);
    std::int64_t value = 0;
    if (!ParseI64(arguments[0].value.encoded_value, &value)) {
      return EvaluationError("SB-INDEX-EXPR-NUMERIC-PARSE-ERROR",
                             "index.expression.numeric_parse_error",
                             op);
    }
    return EvaluationOk(I64Value(-value));
  }

  if (op == "op_add" || op == "op_mul" || op == "op_div") {
    if (arguments.size() != 2) return arity_error();
    if (AnyNull(arguments)) return EvaluationNull(arguments.front().value.descriptor);
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ParseI64(arguments[0].value.encoded_value, &left) ||
        !ParseI64(arguments[1].value.encoded_value, &right)) {
      return EvaluationError("SB-INDEX-EXPR-NUMERIC-PARSE-ERROR",
                             "index.expression.numeric_parse_error",
                             op);
    }
    if (op == "op_add") return EvaluationOk(I64Value(left + right));
    if (op == "op_mul") return EvaluationOk(I64Value(left * right));
    if (right == 0) {
      return EvaluationError("SB-INDEX-EXPR-DIVIDE-BY-ZERO",
                             "index.expression.divide_by_zero",
                             op);
    }
    return EvaluationOk(I64Value(left / right));
  }

  if (op == "op_concat") {
    if (arguments.size() != 2) return arity_error();
    if (AnyNull(arguments)) return EvaluationNull(DescriptorForType("text"));
    return EvaluationOk(TextValue(arguments[0].value.encoded_value +
                                  arguments[1].value.encoded_value));
  }

  if (op == "op_eq") {
    if (arguments.size() != 2) return arity_error();
    if (AnyNull(arguments)) return EvaluationNull(DescriptorForType("boolean"));
    return EvaluationOk(BoolValue(arguments[0].value.encoded_value ==
                                  arguments[1].value.encoded_value));
  }

  return EvaluationError("SB-INDEX-EXPR-OPERATOR-UNSUPPORTED",
                         "index.expression.operator_unsupported",
                         op);
}

std::string UpperAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return value;
}

EvaluationResult EvaluateFunction(const ExpressionIndexExpressionNode& node,
                                  const ExpressionIndexRowInput& row) {
  const std::string fn = LowerAscii(node.identifier);
  const auto arguments = EvaluateArguments(node, row);
  for (const auto& argument : arguments) {
    if (!argument.ok()) {
      return PropagateArgumentFailure(arguments);
    }
  }

  auto arity_error = [&] {
    return EvaluationError("SB-INDEX-EXPR-ARITY-MISMATCH",
                           "index.expression.arity_mismatch",
                           fn);
  };

  if (fn == "fn.lower_ascii" || fn == "fn.upper_ascii" ||
      fn == "fn.length_text" || fn == "fn.abs_i64") {
    if (arguments.size() != 1) return arity_error();
    if (AnyNull(arguments)) return EvaluationNull(arguments.front().value.descriptor);
    if (fn == "fn.lower_ascii") {
      return EvaluationOk(TextValue(LowerAscii(arguments[0].value.encoded_value)));
    }
    if (fn == "fn.upper_ascii") {
      return EvaluationOk(TextValue(UpperAscii(arguments[0].value.encoded_value)));
    }
    if (fn == "fn.length_text") {
      return EvaluationOk(I64Value(static_cast<std::int64_t>(
          arguments[0].value.encoded_value.size())));
    }
    std::int64_t value = 0;
    if (!ParseI64(arguments[0].value.encoded_value, &value)) {
      return EvaluationError("SB-INDEX-EXPR-NUMERIC-PARSE-ERROR",
                             "index.expression.numeric_parse_error",
                             fn);
    }
    return EvaluationOk(I64Value(value < 0 ? -value : value));
  }

  if (fn == "fn.coalesce_text") {
    if (arguments.empty()) return arity_error();
    for (const auto& argument : arguments) {
      if (!argument.value.is_null) return EvaluationOk(argument.value);
    }
    return EvaluationNull(DescriptorForType("text"));
  }

  return EvaluationError("SB-INDEX-EXPR-FUNCTION-UNSUPPORTED",
                         "index.expression.function_unsupported",
                         fn);
}

EvaluationResult EvaluateNode(const ExpressionIndexExpressionNode& node,
                              const ExpressionIndexRowInput& row) {
  switch (node.kind) {
    case ExpressionIndexNodeKind::literal:
      return EvaluationOk(node.literal);
    case ExpressionIndexNodeKind::column_ref: {
      const auto* column = FindColumn(row, node);
      if (column == nullptr) {
        return EvaluationError("SB-INDEX-EXPR-COLUMN-MISSING",
                               "index.expression.column_missing",
                               node.identifier.empty() ? std::to_string(node.column_ordinal)
                                                       : LowerAscii(node.identifier));
      }
      EvaluationResult result = EvaluationOk(column->value);
      result.evidence.push_back("expression_index_column_ref=" +
                                (node.identifier.empty()
                                     ? std::to_string(node.column_ordinal)
                                     : LowerAscii(node.identifier)));
      return result;
    }
    case ExpressionIndexNodeKind::operator_call:
      return EvaluateOperator(node, row);
    case ExpressionIndexNodeKind::function_call:
      return EvaluateFunction(node, row);
  }
  return EvaluationError("SB-INDEX-EXPR-EVALUATION-FAILED",
                         "index.expression.evaluation_failed",
                         "unknown_node_kind");
}

ExpressionIndexExtractedRow RowError(const ExpressionIndexRowInput& row,
                                     const DiagnosticRecord& diagnostic) {
  ExpressionIndexExtractedRow extracted;
  extracted.row_id = row.row_id;
  extracted.expression_error = true;
  extracted.diagnostic = diagnostic;
  extracted.evidence.push_back("expression_index_key_extracted=false");
  return extracted;
}

IndexKeyEncodingComponent ComponentForValue(
    const ExpressionIndexTypedValue& value,
    const ExpressionIndexExtractorDescriptor& descriptor) {
  IndexKeyEncodingComponent component;
  component.kind = IndexKeyComponentKind::expression;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor.result_descriptor.type_descriptor_uuid;
  component.collation_uuid = descriptor.result_descriptor.collation_uuid;
  component.sort_direction = descriptor.sort_direction;
  component.null_placement = descriptor.null_placement;
  component.is_null = value.is_null;
  component.case_folded = descriptor.result_descriptor.case_folded;
  if (value.is_null) {
    return component;
  }

  const std::string type = LowerAscii(descriptor.result_descriptor.canonical_type_name);
  if (IsIntegerType(type)) {
    std::int64_t parsed = 0;
    if (ParseI64(value.encoded_value, &parsed)) {
      component.payload = I64Payload(parsed);
    } else {
      component.payload = Bytes(value.encoded_value);
      component.lossy = true;
    }
  } else if (IsBooleanType(type)) {
    bool truth = false;
    component.payload = TruthValue(value, &truth)
                            ? std::vector<byte>{static_cast<byte>(truth ? 1 : 0)}
                            : Bytes(value.encoded_value);
    component.lossy = component.payload.size() != 1;
  } else if (IsBinaryType(type)) {
    component.payload = !value.binary_value.empty()
                            ? value.binary_value
                            : Bytes(value.encoded_value);
  } else {
    component.payload = Bytes(value.encoded_value);
  }
  return component;
}

}  // namespace

std::string CanonicalizeExpressionIndexTextEnvelope(std::string_view expression_text) {
  std::vector<CanonToken> tokens;
  for (std::size_t index = 0; index < expression_text.size();) {
    const char ch = expression_text[index];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++index;
      continue;
    }
    if (IsIdentStart(ch)) {
      std::size_t end = index + 1;
      while (end < expression_text.size() && IsIdentBody(expression_text[end])) {
        ++end;
      }
      tokens.push_back({CanonTokenKind::identifier,
                        LowerAscii(std::string(expression_text.substr(index, end - index)))});
      index = end;
      continue;
    }
    if (IsNumberish(ch)) {
      std::size_t end = index + 1;
      while (end < expression_text.size() &&
             (IsNumberish(expression_text[end]) || expression_text[end] == '.')) {
        ++end;
      }
      tokens.push_back({CanonTokenKind::number,
                        std::string(expression_text.substr(index, end - index))});
      index = end;
      continue;
    }
    if (ch == '\'') {
      std::string literal;
      literal.push_back(ch);
      ++index;
      while (index < expression_text.size()) {
        literal.push_back(expression_text[index]);
        if (expression_text[index] == '\'') {
          if (index + 1 < expression_text.size() && expression_text[index + 1] == '\'') {
            literal.push_back(expression_text[index + 1]);
            index += 2;
            continue;
          }
          ++index;
          break;
        }
        ++index;
      }
      tokens.push_back({CanonTokenKind::string_literal, std::move(literal)});
      continue;
    }
    std::string symbol(1, ch);
    if (index + 1 < expression_text.size()) {
      const std::string two = std::string(expression_text.substr(index, 2));
      if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
        symbol = two == "!=" ? "<>" : two;
        index += 2;
        tokens.push_back({CanonTokenKind::operator_symbol, symbol});
        continue;
      }
    }
    ++index;
    const bool punctuation = symbol == "(" || symbol == ")" || symbol == ",";
    tokens.push_back({punctuation ? CanonTokenKind::punctuation
                                  : CanonTokenKind::operator_symbol,
                      symbol});
  }
  return JoinCanonicalTokens(tokens);
}

std::string ExpressionIndexDigestForCanonicalEnvelope(std::string_view canonical_envelope) {
  return "sbexpr64:" + Hex64(Fnv1a64(canonical_envelope));
}

ExpressionIndexCanonicalizationResult CanonicalizeExpressionIndexDefinition(
    const ExpressionIndexDefinition& definition) {
  ExpressionIndexCanonicalizationResult result;
  result.status = OkStatus();
  result.ok_value = true;
  result.canonical_text =
      CanonicalizeExpressionIndexTextEnvelope(definition.expression_text);
  const std::string canonical_envelope =
      CanonicalizeExpressionIndexTextEnvelope(definition.expression_envelope);
  if (!canonical_envelope.empty()) {
    result.canonical_envelope = canonical_envelope;
  } else {
    result.canonical_envelope = result.canonical_text;
  }
  if (definition.root_present) {
    if (!result.canonical_envelope.empty()) {
      result.canonical_envelope += "|";
    }
    result.canonical_envelope += "root=" + NodeCanonical(definition.root);
  }
  if (result.canonical_text.empty() && result.canonical_envelope.empty()) {
    return RefuseCanonicalization("SB-INDEX-EXPR-CANONICALIZATION-EMPTY",
                                  "index.expression.canonicalization_empty",
                                  {});
  }
  result.expression_digest =
      ExpressionIndexDigestForCanonicalEnvelope(result.canonical_envelope);
  return result;
}

ExpressionIndexBindResult BindExpressionIndexExtractorDescriptor(
    ExpressionIndexDescriptorCache* cache,
    const ExpressionIndexBindRequest& request) {
  if (!request.definition.root_present) {
    return RefuseBind("SB-INDEX-EXPR-ROOT-MISSING",
                      "index.expression.root_missing",
                      {});
  }
  if (!request.result_descriptor.type_descriptor_uuid.valid()) {
    return RefuseBind("SB-INDEX-EXPR-RESULT-DESCRIPTOR-MISSING",
                      "index.expression.result_descriptor_missing",
                      request.result_descriptor.canonical_type_name);
  }
  if (!request.epochs.resource_epoch_valid ||
      !request.epochs.collation_epoch_valid ||
      !request.epochs.function_resource_epoch_valid) {
    return RefuseBind("SB-INDEX-EXPR-EPOCH-INVALID",
                      "index.expression.epoch_invalid",
                      "resource_or_collation_or_function_epoch_invalid");
  }
  std::string refusal_code;
  std::string refusal_key;
  std::string refusal_detail;
  if (!ValidateDeterminism(request.definition.root,
                           request.operation_proofs,
                           &refusal_code,
                           &refusal_key,
                           &refusal_detail)) {
    return RefuseBind(std::move(refusal_code),
                      std::move(refusal_key),
                      std::move(refusal_detail));
  }

  const auto canonical = CanonicalizeExpressionIndexDefinition(request.definition);
  if (!canonical.ok()) {
    ExpressionIndexBindResult result;
    result.status = canonical.status;
    result.diagnostic = canonical.diagnostic;
    return result;
  }

  ExpressionIndexExtractorDescriptor descriptor;
  descriptor.expression_digest = canonical.expression_digest;
  descriptor.canonical_expression_text = canonical.canonical_text;
  descriptor.canonical_expression_envelope = canonical.canonical_envelope;
  descriptor.result_type_descriptor_signature =
      DescriptorSignature(request.result_descriptor);
  descriptor.cache_key = CacheKey(descriptor.expression_digest,
                                  request.epochs,
                                  descriptor.result_type_descriptor_signature);
  descriptor.definition = request.definition;
  descriptor.result_descriptor = request.result_descriptor;
  descriptor.epochs = request.epochs;
  descriptor.semantic_profile = request.semantic_profile;
  if (descriptor.semantic_profile.profile_id.empty()) {
    descriptor.semantic_profile.profile_id = "expression_index_sbko";
  }
  descriptor.semantic_profile.bytewise_stable = true;
  descriptor.sort_direction = request.sort_direction;
  descriptor.null_placement = request.null_placement;
  descriptor.non_authority = NonAuthorityEvidence();
  descriptor.evidence.push_back("expression_digest=" + descriptor.expression_digest);
  descriptor.evidence.push_back("descriptor_cache_key=" + descriptor.cache_key);
  descriptor.evidence.push_back("expression_deterministic=true");
  descriptor.evidence.push_back("resource_epoch=" +
                                std::to_string(request.epochs.resource_epoch));
  descriptor.evidence.push_back("collation_epoch=" +
                                std::to_string(request.epochs.collation_epoch));
  descriptor.evidence.push_back("function_resource_epoch=" +
                                std::to_string(request.epochs.function_resource_epoch));
  AddNonAuthorityEvidence(&descriptor.evidence);

  ExpressionIndexBindResult result;
  result.status = OkStatus();
  result.admitted = true;
  result.descriptor = descriptor;

  if (cache == nullptr) {
    result.cache_miss = true;
    result.evidence.push_back("descriptor_cache=not_supplied");
    return result;
  }

  const auto before = cache->descriptors.size();
  cache->descriptors.erase(
      std::remove_if(cache->descriptors.begin(),
                     cache->descriptors.end(),
                     [&](const ExpressionIndexExtractorDescriptor& cached) {
                       return SameExpressionAndResult(
                                  cached,
                                  descriptor.expression_digest,
                                  descriptor.result_type_descriptor_signature) &&
                              cached.cache_key != descriptor.cache_key;
                     }),
      cache->descriptors.end());
  result.invalidated_entries = before - cache->descriptors.size();
  cache->invalidations += result.invalidated_entries;

  for (const auto& cached : cache->descriptors) {
    if (cached.cache_key == descriptor.cache_key) {
      ++cache->hits;
      result.cache_hit = true;
      result.descriptor = cached;
      result.evidence.push_back("descriptor_cache=hit");
      result.evidence.push_back("descriptor_cache_hit=true");
      if (result.invalidated_entries != 0) {
        result.evidence.push_back("descriptor_cache_invalidated_entries=" +
                                  std::to_string(result.invalidated_entries));
      }
      return result;
    }
  }

  ++cache->misses;
  result.cache_miss = true;
  cache->descriptors.push_back(descriptor);
  result.evidence.push_back("descriptor_cache=miss");
  result.evidence.push_back("descriptor_cache_miss=true");
  if (result.invalidated_entries != 0) {
    result.evidence.push_back("descriptor_cache_invalidated_entries=" +
                              std::to_string(result.invalidated_entries));
  }
  return result;
}

ExpressionIndexBatchExtractionResult ExtractExpressionIndexKeyBatch(
    const ExpressionIndexBatchExtractionRequest& request) {
  if (request.descriptor.expression_digest.empty() ||
      !request.descriptor.result_descriptor.type_descriptor_uuid.valid()) {
    return RefuseBatch("SB-INDEX-EXPR-DESCRIPTOR-INVALID",
                       "index.expression.descriptor_invalid",
                       {});
  }

  ExpressionIndexBatchExtractionResult result;
  result.status = OkStatus();
  result.extracted = true;
  result.row_count = request.rows.size();
  result.non_authority = NonAuthorityEvidence();
  result.evidence.push_back("expression_digest=" +
                            request.descriptor.expression_digest);
  result.evidence.push_back("expression_index_batch_extraction=true");
  AddNonAuthorityEvidence(&result.evidence);

  for (const auto& row : request.rows) {
    auto evaluated = EvaluateNode(request.descriptor.definition.root, row);
    if (!evaluated.ok()) {
      ++result.error_count;
      result.rows.push_back(RowError(row, evaluated.diagnostic));
      if (!request.continue_on_expression_error) {
        result.status = evaluated.status;
        result.extracted = false;
        result.diagnostic = evaluated.diagnostic;
        return result;
      }
      continue;
    }

    ExpressionIndexExtractedRow extracted;
    extracted.row_id = row.row_id;
    extracted.value = evaluated.value;
    extracted.expression_null = evaluated.value.is_null;
    extracted.evidence = std::move(evaluated.evidence);
    extracted.evidence.push_back("expression_index_key_extracted=true");
    if (extracted.expression_null) {
      ++result.null_count;
      extracted.evidence.push_back("expression_index_null_result=true");
    }

    const auto component = ComponentForValue(evaluated.value, request.descriptor);
    extracted.key = EncodeIndexKey({component}, request.descriptor.semantic_profile);
    if (!extracted.key.ok()) {
      ++result.error_count;
      extracted.expression_error = true;
      extracted.diagnostic = extracted.key.diagnostic;
      extracted.evidence.push_back("expression_index_key_encoding_failed=true");
      result.rows.push_back(std::move(extracted));
      if (!request.continue_on_expression_error) {
        result.status = extracted.key.status;
        result.extracted = false;
        result.diagnostic = extracted.key.diagnostic;
        return result;
      }
      continue;
    }

    ++result.key_count;
    extracted.extracted = true;
    extracted.evidence.insert(extracted.evidence.end(),
                              extracted.key.evidence.begin(),
                              extracted.key.evidence.end());
    extracted.evidence.push_back("expression_index_key_encoding=SBKO");
    AddNonAuthorityEvidence(&extracted.evidence);
    result.rows.push_back(std::move(extracted));
  }
  result.evidence.push_back("expression_index_key_count=" +
                            std::to_string(result.key_count));
  result.evidence.push_back("expression_index_error_count=" +
                            std::to_string(result.error_count));
  return result;
}

ExpressionIndexKeyEnvelopeValidationResult ValidateExpressionIndexExtractorKeyEnvelope(
    std::string_view encoded_key) {
  if (IsUnsafeLegacyIndexKeyEncoding(encoded_key)) {
    return RefuseEnvelope("SB-INDEX-EXPR-LEGACY-KEY-REFUSED",
                          "index.expression.legacy_key_refused",
                          "SBK1");
  }
  if (!IsOrderPreservingIndexKeyEncoding(encoded_key)) {
    return RefuseEnvelope("SB-INDEX-EXPR-KEY-ENVELOPE-REFUSED",
                          "index.expression.key_envelope_refused",
                          "expected_SBKO");
  }
  ExpressionIndexKeyEnvelopeValidationResult result;
  result.status = OkStatus();
  result.accepted = true;
  result.evidence.push_back("expression_index_key_encoding=SBKO");
  result.evidence.push_back("unsafe_legacy_key_encoding=false");
  return result;
}

DiagnosticRecord MakeExpressionIndexExtractorDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.expression_extractor");
}

}  // namespace scratchbird::core::index
