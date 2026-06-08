// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_schema.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::AgentTypeDescriptor RequireAgent(const std::string& type_id) {
  const auto descriptor = agents::FindAgentType(type_id);
  Require(descriptor.has_value(), "missing descriptor " + type_id);
  return *descriptor;
}

void RequireDiagnostic(const agents::AgentRuntimeStatus& status,
                       const std::string& diagnostic_code,
                       const std::string& context) {
  Require(!status.ok, context + " unexpectedly succeeded");
  Require(status.diagnostic_code == diagnostic_code,
          context + " diagnostic mismatch: " + status.diagnostic_code);
}

void TestEveryRequiredPolicyFamilyHasTypedSchema() {
  std::set<std::string> observed_families;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    for (const auto& family : agents::RequiredPolicyFamiliesForAgent(descriptor)) {
      observed_families.insert(family);
      const auto schema = agents::AgentPolicySchemaForFamily(family);
      Require(!schema.empty(), "missing typed schema for " + family);

      std::set<std::string> field_names;
      bool saw_required = false;
      for (const auto& field : schema) {
        Require(!field.name.empty(), "empty schema field for " + family);
        Require(field_names.insert(field.name).second,
                "duplicate schema field " + family + ":" + field.name);
        Require(!field.units.empty(), "missing schema units for " + family + ":" + field.name);
        Require(!agents::AgentPolicyFieldTypeName(field.type).empty(),
                "missing schema type name for " + family + ":" + field.name);
        if (field.required) {
          saw_required = true;
          Require(!field.default_value.empty(),
                  "missing required default for " + family + ":" + field.name);
        }
      }
      Require(saw_required, "policy family has no required typed fields: " + family);

      const auto required = agents::RequiredPolicyConfigFieldsForFamily(family);
      Require(!required.empty(), "legacy required field bridge returned no fields for " + family);
      for (const auto& field : required) {
        Require(field_names.find(field) != field_names.end(),
                "required field absent from typed schema " + family + ":" + field);
      }
    }
  }
  Require(!observed_families.empty(), "no policy families observed");
}

void TestBaselinePoliciesValidateThroughSchemas() {
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    for (const auto& family : agents::RequiredPolicyFamiliesForAgent(descriptor)) {
      const auto policy = agents::BaselinePolicyForAgentFamily(descriptor, family, 9);
      const auto status = agents::ValidateAgentPolicy(policy, descriptor);
      Require(status.ok, "baseline policy rejected by schema " + descriptor.type_id +
                         ":" + family + ":" + status.diagnostic_code);

      const auto defaults = agents::DefaultAgentPolicyConfigFieldsForFamily(family);
      Require(policy.config_fields == defaults,
              "baseline config did not come from typed defaults for " + family);
    }
  }
}

void TestMalformedPolicyValuesFailClosed() {
  const auto& page = RequireAgent("page_allocation_manager");
  auto policy = agents::BaselinePolicyForAgentFamily(page, "page_preallocation_policy", 9);

  auto bad_bool = policy;
  bad_bool.config_fields["preallocation_allowed"] = "1";
  RequireDiagnostic(agents::ValidateAgentPolicy(bad_bool, page),
                    "SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                    "bad bool");

  auto bad_duration = policy;
  bad_duration.config_fields["forecast_window_seconds"] = "soon";
  RequireDiagnostic(agents::ValidateAgentPolicy(bad_duration, page),
                    "SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                    "bad duration");

  auto bad_range = policy;
  bad_range.config_fields["history_confidence_threshold"] = "1.5";
  RequireDiagnostic(agents::ValidateAgentPolicy(bad_range, page),
                    "SB_AGENT_POLICY_SCHEMA.RANGE_VIOLATION",
                    "bad decimal range");

  const auto& filespace = RequireAgent("filespace_capacity_manager");
  auto percent = agents::BaselinePolicyForAgentFamily(filespace, "filespace_capacity_policy", 9);
  percent.config_fields["minimum_free_percent"] = "101";
  RequireDiagnostic(agents::ValidateAgentPolicy(percent, filespace),
                    "SB_AGENT_POLICY_SCHEMA.RANGE_VIOLATION",
                    "bad percent range");
}

void TestUnknownAndHeuristicFieldsFailClosed() {
  const auto& page = RequireAgent("page_allocation_manager");
  auto policy = agents::BaselinePolicyForAgentFamily(page, "page_preallocation_policy", 9);
  policy.config_fields["client_required_allowed_fence_redaction"] = "true";
  RequireDiagnostic(agents::ValidateAgentPolicy(policy, page),
                    "SB_AGENT_POLICY_SCHEMA.UNKNOWN_FIELD",
                    "substring heuristic field");

  Require(!agents::FindAgentPolicyFieldSchema("page_preallocation_policy",
                                              "client_required_allowed_fence_redaction")
               .has_value(),
          "substring-only field was accepted as schema");
}

void TestOptionalResourceBudgetOverridesRemainTyped() {
  const auto& page = RequireAgent("page_allocation_manager");
  auto policy = agents::BaselinePolicyForAgentFamily(page, "page_preallocation_policy", 9);
  policy.config_fields["protect_foreground_work"] = "false";
  policy.config_fields["max_memory_bytes"] = "22";
  policy.config_fields["watchdog_timeout_microseconds"] = "77";
  const auto status = agents::ValidateAgentPolicy(policy, page);
  Require(status.ok, "typed optional resource budget override rejected: " +
                         status.diagnostic_code);

  auto bad_optional = policy;
  bad_optional.config_fields["protect_foreground_work"] = "enabled";
  RequireDiagnostic(agents::ValidateAgentPolicy(bad_optional, page),
                    "SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                    "bad optional bool");
}

void TestUnknownPolicyFamilyHasNoSchemaAuthority() {
  Require(agents::AgentPolicySchemaForFamily("synthetic_required_allowed_policy").empty(),
          "unknown policy family unexpectedly has schema");
  Require(agents::DefaultAgentPolicyConfigFieldsForFamily(
              "synthetic_required_allowed_policy")
              .empty(),
          "unknown policy family produced defaults");
}

}  // namespace

int main() {
  TestEveryRequiredPolicyFamilyHasTypedSchema();
  TestBaselinePoliciesValidateThroughSchemas();
  TestMalformedPolicyValuesFailClosed();
  TestUnknownAndHeuristicFieldsFailClosed();
  TestOptionalResourceBudgetOverridesRemainTyped();
  TestUnknownPolicyFamilyHasNoSchemaAuthority();
  return EXIT_SUCCESS;
}
