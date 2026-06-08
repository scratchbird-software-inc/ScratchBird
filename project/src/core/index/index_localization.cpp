// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_localization.hpp"

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

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}
}  // namespace

IndexLocalizedProjectionResult ResolveIndexLocalizedProjection(const IndexLocalizedProjectionRequest& request) {
  IndexLocalizedProjectionResult result;
  result.index_uuid = request.index_uuid;
  if (!request.index_uuid.valid() || request.requested_language_tag.empty()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexLocalizationDiagnostic(result.status,
                                                        "SB-INDEX-LOCALIZATION-INVALID-REQUEST",
                                                        "index.localization.invalid_request");
    return result;
  }

  auto apply_text = [&](const IndexLocalizedText& text) {
    if (!SameTypedUuid(text.index_uuid, request.index_uuid)) {
      return;
    }
    if (text.text_class == IndexLocalizedTextClass::name) {
      if (!result.name.empty() && result.name != text.value) {
        result.ambiguity_detected = true;
      }
      result.name = text.value;
      result.language_tag = text.language_tag;
    } else if (text.text_class == IndexLocalizedTextClass::comment) {
      result.comment = text.value;
    }
  };

  for (const auto& text : request.texts) {
    if (text.language_tag == request.requested_language_tag) {
      apply_text(text);
    }
  }
  if (result.name.empty()) {
    result.used_default_language = true;
    for (const auto& text : request.texts) {
      if (text.language_tag == request.default_language_tag || text.is_default) {
        apply_text(text);
      }
    }
  }
  if (result.name.empty()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexLocalizationDiagnostic(result.status,
                                                        "SB-INDEX-LOCALIZATION-NAME-MISSING",
                                                        "index.localization.name_missing");
    return result;
  }
  if (result.ambiguity_detected) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexLocalizationDiagnostic(result.status,
                                                        "SB-INDEX-LOCALIZATION-AMBIGUOUS",
                                                        "index.localization.ambiguous",
                                                        result.name);
    return result;
  }
  result.status = OkStatus();
  return result;
}

DiagnosticRecord MakeIndexLocalizationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.localization");
}

}  // namespace scratchbird::core::index
