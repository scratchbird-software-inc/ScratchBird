// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine_api_bridge.hpp"

#include "engine_builtin_operations.hpp"
#include "uuid.hpp"

#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::parser::lowering {
namespace {

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

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

EngineApiBridgeResult Error(std::string code, std::string message) {
  EngineApiBridgeResult result;
  result.accepted = false;
  result.diagnostic = {std::move(code), std::move(message)};
  return result;
}

api::EngineOperationCode OperationCodeForKey(const std::string& operation_key) {
  if (operation_key == "op.show.version") {
    return api::EngineOperationCode::show_version;
  }
  if (operation_key == "op.show.database") {
    return api::EngineOperationCode::show_database;
  }
  return api::EngineOperationCode::unknown;
}

api::BoundEngineOperation OperationDescriptorForKey(const std::string& operation_key) {
  if (operation_key == "op.show.version") {
    return api::MakeShowVersionOperationDescriptor();
  }
  if (operation_key == "op.show.database") {
    return api::MakeShowDatabaseOperationDescriptor();
  }

  api::BoundEngineOperation operation;
  operation.operation_code = api::EngineOperationCode::unknown;
  return operation;
}

std::vector<byte> PayloadForEnvelope(const LogicalEnvelope& envelope) {
  std::vector<byte> payload;
  const std::string stable_payload = envelope.operation_key + ":" + envelope.trace_key;
  payload.reserve(stable_payload.size());
  for (char value : stable_payload) {
    payload.push_back(static_cast<byte>(value));
  }
  return payload;
}

bool ParseTypedIdentity(UuidKind kind, const std::string& text, TypedUuid& out) {
  uuid::TypedUuidResult parsed = uuid::ParseTypedUuid(kind, text);
  if (!parsed.ok()) {
    return false;
  }
  out = parsed.value;
  return true;
}

std::string BridgeDiagnosticCode(const api::EngineDispatchRequestResult& result) {
  if (!result.diagnostic.diagnostic_code.empty()) {
    return result.diagnostic.diagnostic_code;
  }
  return "SB-PARSER-ENGINE-BRIDGE-DISPATCH-REJECTED";
}

std::string BridgeDiagnosticMessage(const api::EngineDispatchRequestResult& result) {
  if (!result.diagnostic.message_key.empty()) {
    return result.diagnostic.message_key;
  }
  return "engine dispatch request rejected";
}

}  // namespace

EngineApiBridgeResult BridgeLogicalEnvelopeToEngineRequest(const LogicalEnvelope& envelope,
                                                           const EngineApiBridgeContext& context) {
  if (OperationCodeForKey(envelope.operation_key) == api::EngineOperationCode::unknown) {
    return Error("SB-PARSER-ENGINE-BRIDGE-UNKNOWN-OPERATION",
                 "logical envelope operation is not mapped to an engine internal operation");
  }

  TypedUuid database_uuid;
  TypedUuid session_uuid;
  TypedUuid principal_uuid;
  if (!ParseTypedIdentity(UuidKind::database, envelope.database_uuid, database_uuid)) {
    return Error("SB-PARSER-ENGINE-BRIDGE-DATABASE-UUID-MUST-BE-V7",
                 "database UUID must be a UUIDv7 engine identity");
  }
  if (!ParseTypedIdentity(UuidKind::session, context.session_uuid, session_uuid)) {
    return Error("SB-PARSER-ENGINE-BRIDGE-SESSION-UUID-MUST-BE-V7",
                 "session UUID must be a UUIDv7 engine identity");
  }
  if (!ParseTypedIdentity(UuidKind::principal, envelope.principal_uuid, principal_uuid)) {
    return Error("SB-PARSER-ENGINE-BRIDGE-PRINCIPAL-UUID-MUST-BE-V7",
                 "principal UUID must be a UUIDv7 engine identity");
  }

  api::EngineContext engine_context;
  engine_context.database_uuid = database_uuid;
  engine_context.session_uuid = session_uuid;
  engine_context.principal_uuid = principal_uuid;
  engine_context.trace_id = envelope.trace_key;
  engine_context.cluster_authority_active = context.cluster_authority_active;
  engine_context.parser_is_trusted = false;

  api::SblrEnvelope engine_envelope;
  engine_envelope.envelope_major = api::kSblrEnvelopeMajor;
  engine_envelope.envelope_minor = api::kSblrEnvelopeMinor;
  engine_envelope.kind = api::SblrEnvelopeKind::native_sblr;
  engine_envelope.payload = PayloadForEnvelope(envelope);
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;

  api::EngineDispatchRequestResult request_result =
      api::MakeEngineDispatchRequest(std::move(engine_context),
                                     std::move(engine_envelope),
                                     OperationDescriptorForKey(envelope.operation_key));
  if (!request_result.ok()) {
    return Error(BridgeDiagnosticCode(request_result), BridgeDiagnosticMessage(request_result));
  }

  EngineApiBridgeResult result;
  result.accepted = true;
  result.request = std::move(request_result.request);
  return result;
}

std::string SerializeEngineApiBridgeResultToJson(const EngineApiBridgeResult& result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"accepted_by_engine_api\": " << (result.accepted ? "true" : "false");
  if (!result.accepted) {
    out << ",\n";
    out << "  \"diagnostic\": {\n";
    out << "    \"diagnostic_code\": \"" << JsonEscape(result.diagnostic.code) << "\",\n";
    out << "    \"phase\": \"parser_engine_bridge\",\n";
    out << "    \"message\": \"" << JsonEscape(result.diagnostic.message) << "\"\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
  }

  out << ",\n";
  out << "  \"operation_code\": \""
      << JsonEscape(api::EngineOperationCodeName(result.request.operation.operation_code)) << "\",\n";
  out << "  \"authority_class\": \""
      << JsonEscape(api::OperationAuthorityClassName(result.request.operation.authority_class)) << "\",\n";
  out << "  \"result_cardinality\": \""
      << JsonEscape(api::EngineResultCardinalityName(result.request.operation.result_shape.cardinality)) << "\",\n";
  out << "  \"result_column_count\": " << result.request.operation.result_shape.columns.size() << ",\n";
  out << "  \"sblr_envelope_kind\": \""
      << JsonEscape(api::SblrEnvelopeKindName(result.request.envelope.kind)) << "\",\n";
  out << "  \"payload_bytes\": " << result.request.envelope.payload.size() << ",\n";
  out << "  \"parser_is_trusted\": "
      << (result.request.context.parser_is_trusted ? "true" : "false") << "\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::parser::lowering
