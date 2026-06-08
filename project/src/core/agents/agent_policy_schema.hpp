// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_AGENT_RUNTIME_SUBSTRATE_REFACTOR
// SEARCH_KEY: ARHC_TYPED_POLICY_SCHEMAS_DEFAULTS
// Typed policy schema substrate for agent runtime policy defaults and
// validation. Policy config values are engine-owned policy evidence; they are
// not parser, client, donor, transaction, visibility, recovery, or security
// authority.

#include "agent_runtime.hpp"

#include <optional>

namespace scratchbird::core::agents {

enum class AgentPolicyFieldType {
  boolean,
  unsigned_integer,
  decimal,
  duration_seconds,
  duration_microseconds,
  byte_count,
  percent,
  token,
  token_list
};

enum class AgentPolicyFieldSensitivity {
  public_evidence,
  operational,
  sensitive
};

struct AgentPolicyFieldSchema {
  std::string name;
  AgentPolicyFieldType type = AgentPolicyFieldType::token;
  std::string units;
  std::string default_value;
  std::optional<double> minimum;
  std::optional<double> maximum;
  AgentPolicyFieldSensitivity sensitivity =
      AgentPolicyFieldSensitivity::operational;
  bool required = true;
};

std::string AgentPolicyFieldTypeName(AgentPolicyFieldType type);
std::string AgentPolicyFieldSensitivityName(
    AgentPolicyFieldSensitivity sensitivity);
std::vector<std::string> AgentPolicySchemaFamilies();
std::vector<AgentPolicyFieldSchema> AgentPolicySchemaForFamily(
    const std::string& policy_family);
std::vector<std::string> RequiredAgentPolicySchemaFieldsForFamily(
    const std::string& policy_family);
std::map<std::string, std::string> DefaultAgentPolicyConfigFieldsForFamily(
    const std::string& policy_family);
std::optional<AgentPolicyFieldSchema> FindAgentPolicyFieldSchema(
    const std::string& policy_family,
    const std::string& field_name);
AgentRuntimeStatus ValidateAgentPolicyConfigAgainstSchema(
    const AgentPolicy& policy);

}  // namespace scratchbird::core::agents
