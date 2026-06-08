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

std::string SerializeToJson(const LogicalEnvelope& envelope) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"envelope_format_version\": " << envelope.envelope_format_version << ",\n";
  out << "  \"envelope_kind\": \"" << JsonEscape(envelope.envelope_kind) << "\",\n";
  out << "  \"operation_key\": \"" << JsonEscape(envelope.operation_key) << "\",\n";
  out << "  \"operation_version\": " << envelope.operation_version << ",\n";
  out << "  \"database_uuid\": \"" << JsonEscape(envelope.database_uuid) << "\",\n";
  out << "  \"principal_uuid\": \"" << JsonEscape(envelope.principal_uuid) << "\",\n";
  out << "  \"registry_snapshot_uuid\": \"" << JsonEscape(envelope.registry_snapshot_uuid) << "\",\n";
  out << "  \"result_shape\": \"" << JsonEscape(envelope.result_shape) << "\",\n";
  out << "  \"diagnostic_shape\": \"" << JsonEscape(envelope.diagnostic_shape) << "\",\n";
  out << "  \"trace_key\": \"" << JsonEscape(envelope.trace_key) << "\",\n";
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
