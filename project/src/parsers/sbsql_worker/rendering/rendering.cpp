// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rendering/rendering.hpp"

#include <sstream>
#include <utility>

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

Diagnostic MakeRenderSelectionDiagnostic(const SblrRenderSelection& selection,
                                         std::string severity) {
  return MakeDiagnostic(
      selection.diagnostic_code,
      std::move(severity),
      selection.diagnostic_message,
      "sbp_sbsql.renderer",
      {{"render_decision", std::string(SblrRenderDecisionName(selection.decision))},
       {"renderer_lossiness", std::string(SblrRenderLossinessName(selection.lossiness))},
       {"selected_profile", selection.selected_language_profile.empty()
                                ? "not_available"
                                : selection.selected_language_profile},
       {"fallback_profile", selection.fallback_language_profile.empty()
                                ? "not_available"
                                : selection.fallback_language_profile},
       {"canonical_english_fallback",
        selection.used_canonical_english_fallback ? "true" : "false"},
       {"server_revalidation_required",
        selection.server_revalidation_required ? "true" : "false"}});
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

SblrEnvelopeRenderResult RenderSblrEnvelopeWithProfileSelection(
    const SblrEnvelope& envelope,
    const SblrRenderRequest& request) {
  SblrEnvelopeRenderResult result;
  result.selection = ClassifySblrRenderSelection(request);
  const bool renderable =
      result.selection.decision == SblrRenderDecision::kPreferredLanguage ||
      result.selection.decision == SblrRenderDecision::kCanonicalEnglishFallback;

  if (!renderable) {
    result.messages.diagnostics.push_back(
        MakeRenderSelectionDiagnostic(result.selection, "ERROR"));
    result.text = RenderMessageVectorSet(result.messages);
    return result;
  }

  if (envelope.messages.has_errors()) {
    result.messages = envelope.messages;
    result.text = RenderMessageVectorSet(result.messages);
    return result;
  }

  result.ok = true;
  result.messages.diagnostics.push_back(
      MakeRenderSelectionDiagnostic(result.selection, "INFO"));
  result.text = RenderSblrEnvelope(envelope);
  return result;
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
