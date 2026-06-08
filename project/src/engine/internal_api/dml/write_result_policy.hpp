// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class EngineWriteResultPolicy {
  full_payload,
  return_none,
  ids_only,
  summary_only,
  changed_fields,
};

struct EngineWriteResultPolicyResolution {
  bool ok = true;
  bool explicitly_supplied = false;
  EngineWriteResultPolicy policy = EngineWriteResultPolicy::full_payload;
  std::string policy_name = "full_payload";
  std::string source_key;
  EngineApiDiagnostic diagnostic;
};

EngineWriteResultPolicyResolution ResolveWriteResultPolicy(
    const EngineApiRequest& request,
    const std::string& operation_id);

EngineWriteResultPolicyResolution ResolveWriteResultPolicyOptions(
    const std::vector<std::string>& option_envelopes,
    const std::string& operation_id);

std::vector<std::string> StripWriteResultPolicyOptions(
    const std::vector<std::string>& option_envelopes);

bool IsWriteResultPolicyOption(const std::string& option);

bool WriteResultPolicySuppressesPayloadRows(
    const EngineWriteResultPolicyResolution& policy);

void AddWriteResultPolicyRefusalEvidence(
    const EngineWriteResultPolicyResolution& policy,
    EngineApiResult* result);

void ApplyWriteResultPolicy(const EngineWriteResultPolicyResolution& policy,
                            EngineApiResult* result);

}  // namespace scratchbird::engine::internal_api
