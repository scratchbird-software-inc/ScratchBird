// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_quarantine.hpp"

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

IndexQuarantineDecision Refuse(std::string code, std::string key, std::string detail, bool rebuild) {
  IndexQuarantineDecision decision;
  decision.status = ErrorStatus();
  decision.allow_use = false;
  decision.quarantine_required = true;
  decision.rebuild_required = rebuild;
  decision.diagnostic = MakeIndexQuarantineDiagnostic(decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}
}  // namespace

IndexQuarantineDecision ClassifyIndexPageAuthority(const IndexPageAuthorityInput& input) {
  if (!input.page_type_supported) {
    return Refuse("SB-INDEX-QUARANTINE-UNSUPPORTED-PAGE-TYPE",
                  "index.quarantine.unsupported_page_type", {}, true);
  }
  if (!input.checksum_valid) {
    return Refuse("SB-INDEX-QUARANTINE-CHECKSUM-INVALID",
                  "index.quarantine.checksum_invalid", {}, true);
  }
  if (!input.expected_index_uuid.valid() || !SameTypedUuid(input.expected_index_uuid, input.observed_index_uuid)) {
    return Refuse("SB-INDEX-QUARANTINE-INDEX-UUID-MISMATCH",
                  "index.quarantine.index_uuid_mismatch", {}, true);
  }
  if (input.expected_family == IndexFamily::unknown || input.expected_family != input.observed_family) {
    return Refuse("SB-INDEX-QUARANTINE-FAMILY-MISMATCH",
                  "index.quarantine.family_mismatch", {}, true);
  }
  if (input.expected_resource_epoch != input.observed_resource_epoch) {
    return Refuse("SB-INDEX-QUARANTINE-RESOURCE-EPOCH-STALE",
                  "index.quarantine.resource_epoch_stale", {}, false);
  }
  IndexQuarantineDecision decision;
  decision.status = OkStatus();
  decision.allow_use = true;
  return decision;
}

DiagnosticRecord MakeIndexQuarantineDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.quarantine");
}

}  // namespace scratchbird::core::index
