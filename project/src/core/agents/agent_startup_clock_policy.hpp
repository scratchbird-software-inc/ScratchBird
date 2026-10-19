// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../catalog/catalog_agent_bootstrap.hpp"
#include "../uuid/node_uuid_issuer.hpp"
#include <charconv>
#include <limits>

namespace scratchbird::core::agents {
enum class AgentStartupClockPolicyError {none,invalid_definition,missing_field,invalid_field};
struct AgentStartupClockPolicySelection {
  AgentStartupClockPolicyError error=AgentStartupClockPolicyError::invalid_definition;
  platform::Uuid database_uuid,policy_uuid,version_uuid;
  platform::u64 generation=0;
  uuid::StandaloneUuidV7Policy policy;
  std::string_view field;
  bool ok()const noexcept{return error==AgentStartupClockPolicyError::none;}
};
// Exact declaration decoding only. The caller must retain the actual catalog
// source and its policy owner; this value issues no clock, identity or snapshot.
inline AgentStartupClockPolicySelection SelectAgentStartupClockPolicy(
    const catalog::CatalogAgentBootstrapDefinition& definition,const platform::Uuid& version) noexcept {
  using E=AgentStartupClockPolicyError;using platform::u64;
  const auto fail=[](E error,std::string_view field={}){AgentStartupClockPolicySelection r;r.error=error;r.field=field;return r;};
  if(definition.kind!=catalog::AgentBootstrapDefinitionKind::policy||definition.agent_type!="agent_coordinator"||
      !uuid::IsEngineIdentityUuid(definition.database_uuid)||!uuid::IsEngineIdentityUuid(definition.definition_uuid)||
      !uuid::IsEngineIdentityUuid(version)||!definition.generation||definition.policy_uuid!=definition.definition_uuid)
    return fail(E::invalid_definition);
  constexpr std::string_view keys[]{"config.startup_clock_max_backward_ns","config.startup_clock_max_forward_jump_ns",
    "config.startup_clock_fail_on_rollback","config.startup_clock_fail_on_monotonic_regression",
    "config.startup_clock_fail_on_forward_jump","config.startup_uuid_max_regression_ms",
    "config.startup_uuid_same_ms_wait_timeout_ms"};
  std::array<std::string_view,7> values{};unsigned seen=0;
  auto body=definition.properties;
  while(!body.empty()){
    const auto end=body.find('\n');if(end==body.npos)return fail(E::invalid_field);
    const auto line=body.substr(0,end);const auto equal=line.find('=');
    if(!equal||equal==line.npos)return fail(E::invalid_field);
    const auto key=line.substr(0,equal);
    for(unsigned i=0;i<values.size();++i)if(key==keys[i]){
      if(seen&(1u<<i))return fail(E::invalid_field,key);
      seen|=1u<<i;values[i]=line.substr(equal+1);
    }
    body.remove_prefix(end+1);
  }
  for(unsigned i=0;i<values.size();++i)if(!(seen&(1u<<i)))return fail(E::missing_field,keys[i]);
  const auto number=[](std::string_view s,u64& out){
    if(s.empty()||s.front()<'0'||s.front()>'9')return false;
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),out);
    return parsed.ec==std::errc{}&&parsed.ptr==s.data()+s.size();
  };
  AgentStartupClockPolicySelection out;
  const std::array<u64*,4> numbers{&out.policy.clock.max_wall_clock_backward_nanoseconds,
    &out.policy.clock.max_wall_clock_forward_jump_nanoseconds,&out.policy.max_uuid_regression_ms,&out.policy.same_ms_wait_timeout_ms};
  constexpr unsigned indices[]{0,1,5,6};
  for(unsigned i=0;i<numbers.size();++i)if(!number(values[indices[i]],*numbers[i]))return fail(E::invalid_field,keys[indices[i]]);
  if(out.policy.max_uuid_regression_ms>0x0000ffffffffffffULL)return fail(E::invalid_field,keys[5]);
  if(!out.policy.same_ms_wait_timeout_ms||out.policy.same_ms_wait_timeout_ms>std::numeric_limits<u64>::max()/1000000)
    return fail(E::invalid_field,keys[6]);
  const std::array<bool*,3> flags{&out.policy.clock.fail_closed_on_wall_clock_rollback,
    &out.policy.clock.fail_closed_on_monotonic_regression,&out.policy.clock.fail_closed_on_wall_clock_forward_jump};
  for(unsigned i=0;i<flags.size();++i){const auto value=values[i+2];
    if(value!="true"&&value!="false")return fail(E::invalid_field,keys[i+2]);*flags[i]=value=="true";
  }
  out.database_uuid=definition.database_uuid;out.policy_uuid=definition.definition_uuid;out.version_uuid=version;
  out.generation=definition.generation;out.error=E::none;return out;
}
} // namespace scratchbird::core::agents
