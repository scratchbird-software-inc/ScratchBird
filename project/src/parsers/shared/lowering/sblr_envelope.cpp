// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_envelope.hpp"

#include <sstream>

namespace scratchbird::parser::lowering {
namespace {

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

LoweringDiagnostic Error(std::string code, std::string message) {
  return LoweringDiagnostic{std::move(code), std::move(message)};
}

}  // namespace

bool LoweringResult::ok() const {
  return std::holds_alternative<LogicalEnvelope>(value);
}

LoweringResult LowerBoundShowIdentity(
    const scratchbird::parser::bound_ast::BoundShowIdentity& bound) {
  if (bound.header.sblr_operation_key.empty()) {
    return LoweringResult{Error("SBLRLOW_OPERATION_KEY_MISSING",
                                "bound AST is missing SBLR operation key")};
  }
  if (bound.header.result_shape.empty()) {
    return LoweringResult{Error("SBLRLOW_RESULT_SHAPE_MISSING",
                                "bound AST is missing result shape")};
  }
  if (bound.header.diagnostic_shape.empty()) {
    return LoweringResult{Error("SBLRLOW_DIAGNOSTIC_SHAPE_MISSING",
                                "bound AST is missing diagnostic shape")};
  }

  LogicalEnvelope envelope;
  envelope.operation_key = bound.header.sblr_operation_key;
  envelope.database_uuid = bound.header.database_uuid;
  envelope.principal_uuid = bound.header.principal_uuid;
  envelope.registry_snapshot_uuid = bound.header.registry_snapshot_uuid;
  envelope.result_shape = bound.header.result_shape;
  envelope.diagnostic_shape = bound.header.diagnostic_shape;
  envelope.trace_key = bound.header.trace_key;

  if (bound.header.sblr_operation_key == "op.show.database") {
    envelope.operands.push_back({"uuid_ref", "database_uuid", bound.header.database_uuid});
  }

  return LoweringResult{std::move(envelope)};
}

LoweringResult LowerBoundStatementFamilyEvidence(
    const scratchbird::parser::bound_ast::BoundStatementFamilyEvidence& bound,
    const SblrRouteDescriptor& route) {
  if (bound.header.database_uuid.empty() || bound.header.principal_uuid.empty()) {
    return LoweringResult{Error("SBLRLOW_IDENTITY_CONTEXT_MISSING",
                                "bound AST is missing database or principal identity")};
  }
  if (route.canonical_operation_family.empty() || route.route_operation_family.empty() ||
      route.operation_id.empty() || route.sblr_opcode.empty() || route.result_shape.empty() ||
      route.diagnostic_shape.empty() || route.payload_class.empty()) {
    return LoweringResult{Error("SBLRLOW_ROUTE_DESCRIPTOR_INCOMPLETE",
                                "parser-supplied SBLR route descriptor is incomplete")};
  }
  if (route.contains_raw_sql_text) {
    return LoweringResult{Error("SBLRLOW_RAW_SQL_PAYLOAD_FORBIDDEN",
                                "parser-supplied SBLR route cannot contain raw SQL text")};
  }
  if (!bound.header.sblr_operation_key.empty() &&
      bound.header.sblr_operation_key != route.operation_id) {
    return LoweringResult{Error("SBLRLOW_OPERATION_KEY_MISMATCH",
                                "bound AST operation key does not match parser-supplied SBLR route")};
  }
  if (!bound.header.result_shape.empty() && bound.header.result_shape != route.result_shape) {
    return LoweringResult{Error("SBLRLOW_RESULT_SHAPE_MISMATCH",
                                "bound AST result shape does not match parser-supplied SBLR route")};
  }

  LogicalEnvelope envelope;
  envelope.envelope_kind = "SBLRExecutionEnvelope.v3";
  envelope.operation_family = route.route_operation_family;
  envelope.canonical_operation_family = route.canonical_operation_family;
  envelope.operation_key = route.operation_id;
  envelope.sblr_opcode = route.sblr_opcode;
  envelope.operation_version = 3;
  envelope.database_uuid = bound.header.database_uuid;
  envelope.principal_uuid = bound.header.principal_uuid;
  envelope.registry_snapshot_uuid = bound.header.registry_snapshot_uuid;
  envelope.result_shape = route.result_shape;
  envelope.diagnostic_shape = route.diagnostic_shape;
  envelope.payload_class = route.payload_class;
  envelope.trace_key = bound.header.trace_key;
  envelope.requires_public_abi_dispatch = route.requires_public_abi_dispatch;
  envelope.contains_raw_sql_text = false;
  envelope.operands.push_back({"descriptor_profile", "descriptor_profile", bound.descriptor_profile});
  envelope.operands.push_back({"operation_family", "canonical_operation_family", route.canonical_operation_family});
  envelope.operands.push_back({"operation_family", "route_operation_family", route.route_operation_family});
  return LoweringResult{std::move(envelope)};
}

std::string SerializeToJson(const LogicalEnvelope& envelope) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"envelope_format_version\": " << envelope.envelope_format_version << ",\n";
  out << "  \"envelope_kind\": \"" << JsonEscape(envelope.envelope_kind) << "\",\n";
  out << "  \"operation_family\": \"" << JsonEscape(envelope.operation_family) << "\",\n";
  out << "  \"canonical_operation_family\": \"" << JsonEscape(envelope.canonical_operation_family) << "\",\n";
  out << "  \"operation_key\": \"" << JsonEscape(envelope.operation_key) << "\",\n";
  out << "  \"sblr_opcode\": \"" << JsonEscape(envelope.sblr_opcode) << "\",\n";
  out << "  \"operation_version\": " << envelope.operation_version << ",\n";
  out << "  \"database_uuid\": \"" << JsonEscape(envelope.database_uuid) << "\",\n";
  out << "  \"principal_uuid\": \"" << JsonEscape(envelope.principal_uuid) << "\",\n";
  out << "  \"registry_snapshot_uuid\": \"" << JsonEscape(envelope.registry_snapshot_uuid) << "\",\n";
  out << "  \"result_shape\": \"" << JsonEscape(envelope.result_shape) << "\",\n";
  out << "  \"diagnostic_shape\": \"" << JsonEscape(envelope.diagnostic_shape) << "\",\n";
  out << "  \"payload_class\": \"" << JsonEscape(envelope.payload_class) << "\",\n";
  out << "  \"trace_key\": \"" << JsonEscape(envelope.trace_key) << "\",\n";
  out << "  \"requires_public_abi_dispatch\": " << (envelope.requires_public_abi_dispatch ? "true" : "false") << ",\n";
  out << "  \"contains_raw_sql_text\": " << (envelope.contains_raw_sql_text ? "true" : "false") << ",\n";
  out << "  \"operands\": [\n";
  for (std::size_t i = 0; i < envelope.operands.size(); ++i) {
    const auto& operand = envelope.operands[i];
    out << "    {\"type\": \"" << JsonEscape(operand.type) << "\", "
        << "\"name\": \"" << JsonEscape(operand.name) << "\", "
        << "\"value\": \"" << JsonEscape(operand.value) << "\"}";
    if (i + 1 != envelope.operands.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

std::string SerializeDiagnosticToJson(const LoweringDiagnostic& diagnostic) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"diagnostic_code\": \"" << JsonEscape(diagnostic.code) << "\",\n";
  out << "  \"phase\": \"lowering\",\n";
  out << "  \"message\": \"" << JsonEscape(diagnostic.message) << "\"\n";
  out << "}\n";
  return out.str();
}

std::string SerializeLoweringResultToJson(const LoweringResult& result) {
  if (const auto* envelope = std::get_if<LogicalEnvelope>(&result.value)) {
    return SerializeToJson(*envelope);
  }
  return SerializeDiagnosticToJson(std::get<LoweringDiagnostic>(result.value));
}

}  // namespace scratchbird::parser::lowering
