// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hosted_database_runtime.hpp"
#include "core/agents/agent_engine_lifecycle.hpp"
#include "core/agents/agent_policy_schema.hpp"
#include "core/agents/agent_startup_clock_policy.hpp"

namespace scratchbird::server {
namespace {
namespace a=core::agents;
namespace c=core::catalog;
using E=NativeAgentPolicyError;
using K=NativeStartupAgentKind;
using T=c::CatalogAgentType;
using Definition=NativeStartupAgentDefinition;

// SB_CATALOG_NATIVE_AGENT_RECORDS (3c-3): every check below compares
// constants decoded from the typed records; no catalog text is parsed. The
// canonical registry (code, not catalog data) still spells its descriptors as
// coding-time text; each is mapped to its constant once, here.
void Text(std::array<char,128>& target,std::string_view value) noexcept {
  target={};std::copy_n(value.begin(),std::min(value.size(),target.size()-1),target.begin());
}
struct Descriptor {
  const a::AgentTypeDescriptor* source=nullptr;
  T type=T::none;
  c::NativeAgentPolicyFamilyList families;  // RequiredPolicyFamiliesForAgent
  bool families_ok=true;
};
Descriptor Describe(const a::AgentTypeDescriptor& d) {
  Descriptor out;out.source=&d;
  if(const auto type=c::CatalogAgentTypeForTag(d.type_id))out.type=*type;
  for(const auto& family:a::RequiredPolicyFamiliesForAgent(d)){
    const auto code=c::CatalogAgentPolicyFamilyForTag(family);
    if(!code||!out.families.push(*code))out.families_ok=false;
  }
  return out;
}
bool Contains(const c::NativeAgentPolicyFamilyList& list,c::CatalogAgentPolicyFamily family) {
  return std::ranges::find(list.values(),family)!=list.values().end();
}
bool IsLifecycle(T type){return type==T::agent_coordinator||type==T::startup_agent||type==T::shutdown_agent;}

// a::ValidateAgentPolicy, re-expressed on constants (same order and codes).
// Its config-schema step (ValidateAgentPolicyConfigAgainstSchema) is enforced
// by the typed config codec at selection: every family field required and in
// the schema map's range, no unknown field, enumerations listed.
std::string_view ValidPolicy(const c::NativeAgentPolicyRecord& p,const Descriptor& d) {
  using AP=c::CatalogAgentActivationProfile;
  if(!p.enabled&&(p.activation!=AP::disabled||p.action_mode!=c::CatalogAgentActionMode::disabled))
    return "SB_AGENT_POLICY.DISABLED_STATE_INCONSISTENT";
  if(!d.families_ok||!Contains(d.families,p.policy_family))return "SB_AGENT_POLICY.WRONG_FAMILY";
  const auto scope=c::CatalogAgentScopeForPath(d.source->scope);
  if(!scope||*scope!=p.scope)return "SB_AGENT_POLICY.SCOPE_INCOMPATIBLE";
  if(!p.policy_generation)return "SB_AGENT_POLICY.GENERATION_REQUIRED";
  if(p.activation==AP::live_action&&!p.allow_live_action)return "SB_AGENT_POLICY.LIVE_ACTION_NOT_ALLOWED";
  if(p.activation==AP::live_action&&d.source->authority==a::AgentAuthorityClass::observe_only)
    return "SB_AGENT_POLICY.OBSERVE_AGENT_CANNOT_LIVE_ACT";
  if(!p.run_interval_microseconds||!p.lease_microseconds||!p.max_runtime_microseconds||!p.max_restart_attempts||
      !p.initial_backoff_microseconds||!p.max_backoff_microseconds)return "SB_AGENT_POLICY.INVALID_TIME_BUDGET";
  if(p.max_backoff_microseconds<p.initial_backoff_microseconds)return "SB_AGENT_POLICY.INVALID_BACKOFF_BUDGET";
  return {};
}
// The lifecycle agents' literal invariants, on the typed config records.
bool Lifecycle(const Definition& d,const c::NativeAgentPolicyRecord& p,
    std::span<const NativeStartupAgentDefinition> entries,std::string_view& bad) {
  if(!IsLifecycle(d.agent_type))return true;
  if(!p.enabled||p.activation!=c::CatalogAgentActivationProfile::live_action||
      p.action_mode!=c::CatalogAgentActionMode::request_action||!p.allow_live_action){bad="activation";return false;}
  // Additional approval/dry-run requirements remain valid policy. Only their
  // owning runtime gates can satisfy them; configuration validation cannot.
  const auto require=[&](bool value,std::string_view key){if(!value)bad=key;return value;};
  if(d.agent_type==T::agent_coordinator){
    const auto config=d.Config<c::NativeAgentConfig_agent_coordinator_policy>();
    if(!config.ok()){bad="config";return false;}
    const auto& r=*config.record;
    if(!require(r.startup_launch_enabled,"startup_launch_enabled")||!require(r.go_no_go_required,"go_no_go_required")||
        !require(r.worker_launch_forbidden,"worker_launch_forbidden")||!require(r.coordinator_stops_last,"coordinator_stops_last")||
        !require(r.startup_agent==T::startup_agent,"startup_agent")||!require(r.shutdown_agent==T::shutdown_agent,"shutdown_agent"))
      return false;
    using L=c::CatalogAgentRuntimeLayer;
    const auto layers=r.managed_layers.values();
    if(layers.size()!=3||std::ranges::find(layers,L::l1_observation)==layers.end()||
        std::ranges::find(layers,L::l2_recorder)==layers.end()||std::ranges::find(layers,L::l3_dispatcher)==layers.end()){
      bad="managed_layers";return false;
    }
  }else if(d.agent_type==T::startup_agent){
    const auto config=d.Config<c::NativeAgentConfig_agent_startup_policy>();
    if(!config.ok()){bad="config";return false;}
    const auto& r=*config.record;
    if(!require(r.engine_verification_required,"engine_verification_required")||
        !require(r.committed_policy_required,"committed_policy_required")||
        !require(r.child_readiness_required,"child_readiness_required")||
        !require(r.configured_components_only,"configured_components_only")||
        !require(r.launch_trigger==c::CatalogAgentLaunchTrigger::coordinator_startup,"launch_trigger"))return false;
    if(!r.dependency_order.count){bad="dependency_order";return false;}
    for(const auto type:r.dependency_order.values()){
      const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){
        return entry.kind==K::registration&&entry.agent_type==type;});
      if(found==entries.end()||found->state!=c::CatalogAgentRegistrationState::registered||IsLifecycle(type)){
        bad="dependency_order";return false;
      }
    }
  }else{
    const auto config=d.Config<c::NativeAgentConfig_agent_shutdown_policy>();
    if(!config.ok()){bad="config";return false;}
    const auto& r=*config.record;
    if(!require(r.withdraw_admission_first,"withdraw_admission_first")||
        !require(r.reverse_dependency_drain,"reverse_dependency_drain")||
        !require(r.drain_observations_before_recorders,"drain_observations_before_recorders")||
        !require(r.engine_final_receipt_required,"engine_final_receipt_required")||
        !require(r.cooperative_cancellation_only,"cooperative_cancellation_only")||
        !require(r.launch_trigger==c::CatalogAgentLaunchTrigger::coordinator_shutdown,"launch_trigger"))return false;
  }
  return true;
}
} // namespace

NativeAgentPolicyValidation NativeHostedDatabaseRuntime::ValidateAgentPolicies(
    const NativeLocalServiceAgentConfigurationResult& source) noexcept {
  NativeAgentPolicyValidation out;
  const auto fail=[&](E error,core::platform::Uuid id={},std::string_view field={},std::string_view code={}){
    out.error=error;out.failing_object_uuid=id;out.policy_count=out.registration_count=0;
    Text(out.field,field);Text(out.diagnostic_code,code);return out;
  };
  if(!source.ok())return out;
  try{
    const auto registry_status=a::ValidateCanonicalAgentRegistry();
    if(!registry_status.ok)return fail(E::registry_invalid,{}, {},registry_status.diagnostic_code);
    auto registry=a::CanonicalAgentRegistry();auto lifecycle=a::RuntimeLifecycleAgentDescriptors();
    registry.insert(registry.end(),lifecycle.begin(),lifecycle.end());
    std::vector<Descriptor> described;described.reserve(registry.size());
    for(const auto& item:registry)described.push_back(Describe(item));
    const auto entries=source.entries();
    constexpr auto capacity=NativeLocalServiceAgentConfigurationResult::maximum_definitions;
    std::array<std::array<bool,capacity>,capacity> edges{};
    for(std::size_t n=0;n<entries.size();++n){
      const auto& d=entries[n];const auto id=d.definition_uuid;out.failing_object_uuid=id;
      const auto descriptor=std::find_if(described.begin(),described.end(),[&](const auto& item){return item.type==d.agent_type;});
      if(d.agent_type==T::none||descriptor==described.end())return fail(E::unknown_agent,id);
      if(d.kind==K::registration){
        if(!descriptor->families_ok||d.policy_families.count!=descriptor->families.count)
          return fail(E::missing_policy_family,id,"policy_families");
        for(const auto family:descriptor->families.values())if(!Contains(d.policy_families,family))
          return fail(E::missing_policy_family,id,"policy_families");
        ++out.registration_count;continue;
      }
      // Selection decoded every record; a failure here is a changed source.
      const auto decoded=d.Policy();
      if(!decoded.ok())return fail(E::invalid_property,id,"policy");
      const auto& policy=*decoded.record;
      if(policy.policy_generation!=d.generation)return fail(E::generation_mismatch,id,"policy_generation");
      const auto& source_descriptor=*descriptor->source;
      const auto layer=c::CatalogAgentRuntimeLayerForTag(a::AgentRuntimeLayerName(source_descriptor.layer));
      const auto deployment=c::CatalogAgentDeploymentForTag(a::AgentDeploymentName(source_descriptor.deployment));
      const auto authority=c::CatalogAgentAuthorityClassForTag(a::AgentAuthorityClassName(source_descriptor.authority));
      const bool lifecycle_policy=!a::RuntimeLifecycleAgentPolicyFamily(source_descriptor.type_id).empty();
      if(!layer||policy.layer!=*layer||!deployment||policy.deployment!=*deployment||!authority||
          policy.authority!=*authority||policy.policy_class!=(lifecycle_policy?c::CatalogAgentPolicyClass::runtime_lifecycle_agent:
              c::CatalogAgentPolicyClass::operational_agent))
        return fail(E::descriptor_mismatch,id);
      if(const auto refused=ValidPolicy(policy,*descriptor);!refused.empty())return fail(E::schema_refused,id,{},refused);
      if(d.agent_type==T::agent_coordinator){
        const auto config=d.Config<c::NativeAgentConfig_agent_coordinator_policy>();
        if(!config.ok())return fail(E::schema_refused,id,"config");
        const auto clock=a::SelectAgentStartupClockPolicy(policy,*config.record,d.version_uuid);
        if(!clock.ok())return fail(E::schema_refused,id,clock.field);
      }
      // This host currently admits a standalone local service only. Stored
      // cluster declarations stay installed but require separate enrollment.
      if(source_descriptor.cluster_only&&policy.enabled)return fail(E::cluster_admission_required,id);
      std::string_view field;
      if(!Lifecycle(d,policy,entries,field))return fail(E::lifecycle_invariant,id,field);
      const auto required=policy.required_metric_families.values();
      for(const auto& dependency:source_descriptor.metric_dependencies){
        if(!dependency.required)continue;
        const auto metric=c::CatalogMetricFamilyForTag(dependency.metric_family);
        if(!metric||std::ranges::find(required,*metric)==required.end())
          return fail(E::metric_contract_mismatch,id,"required_metric_families");
      }
      for(const auto metric:required)
        if(std::none_of(source_descriptor.metric_dependencies.begin(),source_descriptor.metric_dependencies.end(),
            [&](const auto& dependency){return c::CatalogMetricFamilyForTag(dependency.metric_family)==metric;}))
          return fail(E::metric_contract_mismatch,id,"required_metric_families");
      for(const auto dependency:policy.policy_dependencies.values()){
        std::size_t target=entries.size();
        for(std::size_t j=0;j<entries.size();++j){
          const auto& candidate=entries[j];
          if(candidate.kind!=K::policy||candidate.policy_family!=dependency)continue;
          if(target!=entries.size())return fail(E::dependency_missing,id,"policy_dependencies");
          target=j;
        }
        if(target==entries.size()||(policy.enabled&&!entries[target].enabled))
          return fail(E::dependency_missing,id,"policy_dependencies");
        edges[n][target]=true;
      }
      ++out.policy_count;
    }
    std::array<unsigned char,capacity> color{};
    const auto visit=[&](auto&& self,std::size_t n)->bool{
      if(color[n]==1){out.failing_object_uuid=entries[n].definition_uuid;return false;}
      if(color[n]==2)return true;color[n]=1;
      for(std::size_t j=0;j<entries.size();++j)if(edges[n][j]&&!self(self,j))return false;
      color[n]=2;return true;
    };
    for(std::size_t n=0;n<entries.size();++n)if(!visit(visit,n))
      return fail(E::dependency_cycle,out.failing_object_uuid,"policy_dependencies");
    out.error=E::none;out.failing_object_uuid={};return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted,out.failing_object_uuid);}
}
} // namespace scratchbird::server
