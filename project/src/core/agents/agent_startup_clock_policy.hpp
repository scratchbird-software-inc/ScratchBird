// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../catalog/catalog_native_agent_record.hpp"
#include "../uuid/node_uuid_issuer.hpp"
#include <limits>

namespace scratchbird::core::agents {
// missing_field stays for source compatibility: the typed config codec
// requires every clock field, so a selection never reports it (3c-3).
enum class AgentStartupClockPolicyError {none,invalid_definition,missing_field,invalid_field};
struct AgentStartupClockPolicySelection {
  AgentStartupClockPolicyError error=AgentStartupClockPolicyError::invalid_definition;
  platform::Uuid database_uuid,policy_uuid,version_uuid;
  platform::u64 generation=0;
  uuid::StandaloneUuidV7Policy policy;
  std::string_view field;
  bool ok()const noexcept{return error==AgentStartupClockPolicyError::none;}
};
// SB_CATALOG_NATIVE_AGENT_RECORDS (3c-3): the coordinator's startup clock
// limits from its typed policy record and its typed agent_coordinator_policy
// config record (no text). The caller must retain the actual catalog source
// and its policy owner; this value issues no clock, identity or snapshot.
inline AgentStartupClockPolicySelection SelectAgentStartupClockPolicy(
    const catalog::NativeAgentPolicyRecord& definition,const catalog::NativeAgentConfig_agent_coordinator_policy& config,
    const platform::Uuid& version) noexcept {
  using E=AgentStartupClockPolicyError;
  const auto fail=[](E error,std::string_view field={}){AgentStartupClockPolicySelection r;r.error=error;r.field=field;return r;};
  if(definition.agent_type!=catalog::CatalogAgentType::agent_coordinator||
      definition.policy_family!=catalog::CatalogAgentPolicyFamily::agent_coordinator_policy||
      !uuid::IsEngineIdentityUuid(definition.database_uuid)||!uuid::IsEngineIdentityUuid(definition.definition_uuid)||
      !uuid::IsEngineIdentityUuid(version)||!definition.generation||definition.policy_uuid!=definition.definition_uuid||
      config.definition_uuid!=definition.definition_uuid)
    return fail(E::invalid_definition);
  // The codec enforces the schema map's ranges; the selection keeps its own
  // limits, which the issuer depends on.
  if(config.startup_uuid_max_regression_ms>0x0000ffffffffffffULL)
    return fail(E::invalid_field,"config.startup_uuid_max_regression_ms");
  if(!config.startup_uuid_same_ms_wait_timeout_ms||
      config.startup_uuid_same_ms_wait_timeout_ms>std::numeric_limits<platform::u64>::max()/1000000)
    return fail(E::invalid_field,"config.startup_uuid_same_ms_wait_timeout_ms");
  AgentStartupClockPolicySelection out;
  out.policy.clock.max_wall_clock_backward_nanoseconds=config.startup_clock_max_backward_ns;
  out.policy.clock.max_wall_clock_forward_jump_nanoseconds=config.startup_clock_max_forward_jump_ns;
  out.policy.max_uuid_regression_ms=config.startup_uuid_max_regression_ms;
  out.policy.same_ms_wait_timeout_ms=config.startup_uuid_same_ms_wait_timeout_ms;
  out.policy.clock.fail_closed_on_wall_clock_rollback=config.startup_clock_fail_on_rollback;
  out.policy.clock.fail_closed_on_monotonic_regression=config.startup_clock_fail_on_monotonic_regression;
  out.policy.clock.fail_closed_on_wall_clock_forward_jump=config.startup_clock_fail_on_forward_jump;
  out.database_uuid=definition.database_uuid;out.policy_uuid=definition.definition_uuid;out.version_uuid=version;
  out.generation=definition.generation;out.error=E::none;return out;
}
} // namespace scratchbird::core::agents
