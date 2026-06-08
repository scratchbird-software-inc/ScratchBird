// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rendering/rendering.hpp"

#include <sstream>

namespace scratchbird::parser::sbsql {
namespace {

std::string RenderSafe(std::string_view text) {
  return EscapeJson(text);
}

std::string RedactPublicResultPayload(std::string payload) {
  const std::string hidden_row_version =
      ",{\"ordinal\":1,\"name\":\"_sb_row_version\",\"alias\":\"_sb_row_version\","
      "\"object_uuid\":\"019e05df-f012-7000-8000-0000000000f1\","
      "\"type\":\"uint64\",\"canonical_type\":\"uint64\","
      "\"domain\":\"sb.system.row_version\","
      "\"precision\":20,\"scale\":0,\"length\":8,\"charset\":\"\","
      "\"collation\":\"\",\"nullable\":false,\"generated\":true,"
      "\"computed\":true,\"identity\":false,\"has_default\":false,"
      "\"hidden\":true,\"system\":true}";
  std::size_t pos = 0;
  while ((pos = payload.find(hidden_row_version, pos)) != std::string::npos) {
    payload.erase(pos, hidden_row_version.size());
  }
  return payload;
}

bool HasServerExecutionDiagnostic(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.component == "sbp_sbsql.sbps_client") return true;
  }
  return false;
}

} // namespace

std::string RenderMessageVectorSet(const MessageVectorSet& messages) {
  std::ostringstream out;
  if (messages.diagnostics.empty()) {
    out << "MESSAGE_VECTOR_SET []\n";
    return out.str();
  }
  for (const auto& diagnostic : messages.diagnostics) {
    out << "MESSAGE " << RenderSafe(diagnostic.severity) << ' '
        << RenderSafe(diagnostic.code) << ' ' << RenderSafe(diagnostic.message);
    if (!diagnostic.component.empty()) {
      out << " component=" << RenderSafe(diagnostic.component);
    }
    for (const auto& field : diagnostic.fields) {
      if (!IsPublicDiagnosticFieldAllowed(field.name, field.value)) continue;
      out << ' ' << RenderSafe(field.name) << '=' << RenderSafe(field.value);
    }
    out << "\n";
  }
  return out.str();
}

std::string RenderSblrEnvelope(const SblrEnvelope& envelope) {
  if (envelope.messages.has_errors()) return RenderMessageVectorSet(envelope.messages);
  std::ostringstream out;
  out << "SBLR " << envelope.operation_family << ' ' << envelope.statement_hash << ' ' << envelope.payload << "\n";
  return out.str();
}

std::string RenderPipelineResult(const PipelineResult& result) {
  std::ostringstream out;
  if ((!result.accepted || result.messages.has_errors()) &&
      !result.sblr_payload.empty() &&
      HasServerExecutionDiagnostic(result.messages)) {
    out << "PREPARED " << result.operation_family << ' ' << result.statement_hash << "\n";
    out << "SBLR " << result.sblr_payload << "\n";
    out << RenderMessageVectorSet(result.messages);
    return out.str();
  }
  if (!result.accepted || result.messages.has_errors()) return RenderMessageVectorSet(result.messages);
  out << "PREPARED " << result.operation_family << ' ' << result.statement_hash << "\n";
  out << "SBLR " << result.sblr_payload << "\n";
  if (!result.server_cursor_uuid.empty()) {
    out << "CURSOR " << result.server_cursor_uuid << ' ' << result.server_row_count << "\n";
  }
  if (!result.server_result_payload.empty()) {
    out << "RESULT " << result.server_operation_id << ' ' << result.server_row_count << ' '
        << RedactPublicResultPayload(result.server_result_payload);
    if (result.server_result_payload.empty() || result.server_result_payload.back() != '\n') out << '\n';
  }
  return out.str();
}

} // namespace scratchbird::parser::sbsql
