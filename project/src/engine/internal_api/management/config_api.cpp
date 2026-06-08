// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/config_api.hpp"

#include "behavior_support/api_behavior_store.hpp"

#include <cctype>
#include <string>

namespace scratchbird::engine::internal_api {
namespace {

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

bool ContainsSensitiveKey(const std::string& value) {
  const std::string lower = LowerAscii(value);
  return lower.find("password") != std::string::npos || lower.find("secret") != std::string::npos ||
         lower.find("token") != std::string::npos || lower.find("private_key") != std::string::npos ||
         lower.find("credential") != std::string::npos;
}

std::string RedactConfigPayload(const std::string& payload) {
  if (!ContainsSensitiveKey(payload)) { return payload; }
  return "<redacted:sensitive_config>";
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MANAGEMENT_CONFIG_API_BEHAVIOR
EngineInspectConfigResult EngineInspectConfig(const EngineInspectConfigRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineInspectConfigResult>(request.context, "management.inspect_config");
  for (const auto& record : VisibleApiBehaviorRecords(request.context, "config", request.context.local_transaction_id)) {
    AddApiBehaviorRow(&result, {{"config_uuid", record.object_uuid}, {"name", record.default_name}, {"payload", RedactConfigPayload(record.payload)}, {"state", record.state}});
  }
  AddApiBehaviorEvidence(&result, "config_inspect", std::to_string(result.result_shape.rows.size()));
  AddApiBehaviorEvidence(&result, "config_redaction", "key_based");
  return result;
}

EngineSetConfigResult EngineSetConfig(const EngineSetConfigRequest& request) {
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineSetConfigResult>(
        request.context,
        "management.set_config",
        MakeSecurityContextRequiredDiagnostic("management.set_config"));
  }
  return PersistedRecordResult<EngineSetConfigResult>(request, "management.set_config", "config", false, "set");
}

EngineResetConfigResult EngineResetConfig(const EngineResetConfigRequest& request) {
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineResetConfigResult>(
        request.context,
        "management.reset_config",
        MakeSecurityContextRequiredDiagnostic("management.reset_config"));
  }
  return PersistedRecordResult<EngineResetConfigResult>(request, "management.reset_config", "config", false, "reset", true);
}

}  // namespace scratchbird::engine::internal_api
