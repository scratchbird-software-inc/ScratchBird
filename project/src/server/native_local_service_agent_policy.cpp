// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hosted_database_runtime.hpp"
#include "core/agents/agent_engine_lifecycle.hpp"
#include "core/agents/agent_policy_schema.hpp"
#include <charconv>

namespace scratchbird::server {
namespace {
namespace a=core::agents;
using E=NativeAgentPolicyError;
using K=core::catalog::AgentBootstrapDefinitionKind;
using native_agent_configuration_detail::Property;
using Definition=core::catalog::CatalogAgentBootstrapDefinition;

void Text(std::array<char,128>& target,std::string_view value) noexcept {
  target={};std::copy_n(value.begin(),std::min(value.size(),target.size()-1),target.begin());
}
bool Unsigned(std::string_view value,std::uint64_t& out) noexcept {
  if(value.empty()||value.front()<'0'||value.front()>'9')return false;
  const auto parsed=std::from_chars(value.data(),value.data()+value.size(),out);
  return parsed.ec==std::errc{}&&parsed.ptr==value.data()+value.size();
}
bool Tokens(std::string_view value,std::vector<std::string>& out) {
  if(value.empty())return true;
  if(value.back()==',')return false;
  while(!value.empty()){
    const auto end=value.find(',');const auto token=value.substr(0,end);
    if(token.empty()||std::any_of(token.begin(),token.end(),[](unsigned char c){return c<=32||c==127;})||
        std::find(out.begin(),out.end(),token)!=out.end())return false;
    out.emplace_back(token);
    if(end==value.npos)break;value.remove_prefix(end+1);
  }
  return true;
}
bool DecodePolicy(const Definition& d,a::AgentPolicy& p,std::string_view& bad) {
  // Existing agent policy carriers store identity as 16 binary bytes. Never
  // synthesize an identity from the agent name, baseline or policy generation.
  p.policy_uuid.assign(reinterpret_cast<const char*>(d.definition_uuid.bytes.data()),16);
  constexpr std::array keys{
      "policy_name","policy_class","policy_family","policy_generation","agent_type","layer",
      "deployment","scope","authority","enabled","activation","action_mode","invalid_policy_behavior",
      "allow_live_action","require_manual_approval","require_dry_run_before_live","evidence_required",
      "explainability_required","run_interval_microseconds","jitter_microseconds","lease_microseconds",
      "cooldown_microseconds","max_runtime_microseconds","max_restart_attempts","initial_backoff_microseconds",
      "max_backoff_microseconds","max_history_query_rows","max_evidence_fanout","max_label_cardinality",
      "action_budget_per_window","required_metric_families","policy_dependencies","editable_seed_policy"};
  for(const std::string_view key:keys){
    bool present=false;auto body=d.properties;
    while(!body.empty()){
      const auto end=body.find('\n');const auto line=body.substr(0,end);
      if(line.substr(0,line.find('='))==key){present=true;break;}body.remove_prefix(end+1);
    }
    if(!present){bad=key;return false;}
  }
  auto body=d.properties;
  while(!body.empty()){
    const auto end=body.find('\n');const auto line=body.substr(0,end);const auto equal=line.find('=');
    const auto key=line.substr(0,equal),value=line.substr(equal+1);
    if(key.starts_with("config."))p.config_fields.emplace(std::string(key.substr(7)),std::string(value));
    else if(std::find(keys.begin(),keys.end(),key)==keys.end()){bad=key;return false;}
    body.remove_prefix(end+1);
  }
  for(const auto& [key,destination]:{
      std::pair{"policy_name",&p.policy_name},{"policy_family",&p.policy_family},{"scope",&p.scope},
      {"action_mode",&p.action_mode},{"invalid_policy_behavior",&p.invalid_policy_behavior}}){
    const auto value=Property(d.properties,key);
    if(value.empty()){bad=key;return false;}destination->assign(value);
  }
  for(const auto& [key,destination]:{
      std::pair{"enabled",&p.enabled},{"allow_live_action",&p.allow_live_action},
      {"require_manual_approval",&p.require_manual_approval},{"require_dry_run_before_live",&p.require_dry_run_before_live},
      {"evidence_required",&p.evidence_required},{"explainability_required",&p.explainability_required}}){
    const auto value=Property(d.properties,key);
    if(value!="0"&&value!="1"){bad=key;return false;}*destination=value=="1";
  }
  const auto editable=Property(d.properties,"editable_seed_policy");
  if(editable!="0"&&editable!="1"){bad="editable_seed_policy";return false;}
  for(const auto& [key,destination]:{
      std::pair{"policy_generation",&p.policy_generation},{"run_interval_microseconds",&p.run_interval_microseconds},
      {"jitter_microseconds",&p.jitter_microseconds},{"lease_microseconds",&p.lease_microseconds},
      {"cooldown_microseconds",&p.cooldown_microseconds},{"max_runtime_microseconds",&p.max_runtime_microseconds},
      {"max_restart_attempts",&p.max_restart_attempts},{"initial_backoff_microseconds",&p.initial_backoff_microseconds},
      {"max_backoff_microseconds",&p.max_backoff_microseconds},{"max_history_query_rows",&p.max_history_query_rows},
      {"max_evidence_fanout",&p.max_evidence_fanout},{"max_label_cardinality",&p.max_label_cardinality},
      {"action_budget_per_window",&p.action_budget_per_window}}){
    if(!Unsigned(Property(d.properties,key),*destination)){bad=key;return false;}
  }
  bool activation=false;
  for(const auto candidate:{a::AgentActivationProfile::disabled,a::AgentActivationProfile::observe_only,
      a::AgentActivationProfile::recommend_only,a::AgentActivationProfile::dry_run,a::AgentActivationProfile::live_action})
    if(Property(d.properties,"activation")==a::AgentActivationProfileName(candidate)){p.activation=candidate;activation=true;}
  if(!activation){bad="activation";return false;}
  const auto mode=std::string_view(p.action_mode);
  if(mode!="disabled"&&mode!="observe_only"&&mode!="recommend_only"&&mode!="request_action"&&
      mode!="direct_bounded"&&mode!="dry_run"&&mode!="live_action"&&mode!="operator_only"){
    bad="action_mode";return false;
  }
  if(!Tokens(Property(d.properties,"required_metric_families"),p.required_metric_families)){
    bad="required_metric_families";return false;
  }
  if(!Tokens(Property(d.properties,"policy_dependencies"),p.policy_dependencies)){
    bad="policy_dependencies";return false;
  }
  return true;
}
bool Lifecycle(const Definition& d,const a::AgentPolicy& p,
    std::span<const NativeStartupAgentDefinition> entries,std::string_view& bad) {
  if(a::RuntimeLifecycleAgentPolicyFamily(std::string(d.agent_type)).empty())return true;
  if(!p.enabled||p.activation!=a::AgentActivationProfile::live_action||p.action_mode!="request_action"||
      !p.allow_live_action){bad="activation";return false;}
  // Additional approval/dry-run requirements remain valid policy. Only their
  // owning runtime gates can satisfy them; configuration validation cannot.
  const auto expect=[&](std::string_view key,std::string_view value){
    const auto found=p.config_fields.find(std::string(key));
    if(found!=p.config_fields.end()&&found->second==value)return true;bad=key;return false;
  };
  if(d.agent_type=="agent_coordinator"){
    for(const auto key:{"startup_launch_enabled","go_no_go_required","worker_launch_forbidden","coordinator_stops_last"})
      if(!expect(key,"true"))return false;
    if(!expect("startup_agent","startup_agent")||!expect("shutdown_agent","shutdown_agent"))return false;
    std::vector<std::string> layers;
    if(!Tokens(p.config_fields.at("managed_layers"),layers)||layers.size()!=3||
        std::find(layers.begin(),layers.end(),"L1")==layers.end()||
        std::find(layers.begin(),layers.end(),"L2")==layers.end()||
        std::find(layers.begin(),layers.end(),"L3")==layers.end()){bad="managed_layers";return false;}
  }else if(d.agent_type=="startup_agent"){
    for(const auto key:{"engine_verification_required","committed_policy_required","child_readiness_required","configured_components_only"})
      if(!expect(key,"true"))return false;
    if(!expect("launch_trigger","coordinator_startup"))return false;
    std::vector<std::string> order;
    if(!Tokens(p.config_fields.at("dependency_order"),order)||order.empty()){bad="dependency_order";return false;}
    for(const auto& type:order){
      const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){
        return entry.definition.kind==K::registration&&entry.definition.agent_type==type;});
      if(found==entries.end()||Property(found->definition.properties,"state")!="registered"||
          type=="agent_coordinator"||type=="startup_agent"||type=="shutdown_agent"){
        bad="dependency_order";return false;
      }
    }
  }else{
    for(const auto key:{"withdraw_admission_first","reverse_dependency_drain","drain_observations_before_recorders",
        "engine_final_receipt_required","cooperative_cancellation_only"})if(!expect(key,"true"))return false;
    if(!expect("launch_trigger","coordinator_shutdown"))return false;
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
    const auto entries=source.entries();
    constexpr auto capacity=NativeLocalServiceAgentConfigurationResult::maximum_definitions;
    std::array<std::array<bool,capacity>,capacity> edges{};
    for(std::size_t n=0;n<entries.size();++n){
      const auto& d=entries[n].definition;const auto id=d.definition_uuid;out.failing_object_uuid=id;
      const auto descriptor=std::find_if(registry.begin(),registry.end(),[&](const auto& item){return item.type_id==d.agent_type;});
      if(descriptor==registry.end())return fail(E::unknown_agent,id);
      const auto families=a::RequiredPolicyFamiliesForAgent(*descriptor);
      if(d.kind==K::registration){
        std::vector<std::string> configured;
        if(!Tokens(Property(d.properties,"policy_families"),configured)||configured.size()!=families.size())
          return fail(E::missing_policy_family,id,"policy_families");
        for(const auto& family:families)if(std::find(configured.begin(),configured.end(),family)==configured.end())
          return fail(E::missing_policy_family,id,"policy_families");
        ++out.registration_count;continue;
      }
      a::AgentPolicy policy;std::string_view field;
      if(!DecodePolicy(d,policy,field))return fail(E::invalid_property,id,field);
      if(policy.policy_generation!=d.generation)return fail(E::generation_mismatch,id,"policy_generation");
      const bool lifecycle_policy=!a::RuntimeLifecycleAgentPolicyFamily(descriptor->type_id).empty();
      if(Property(d.properties,"layer")!=a::AgentRuntimeLayerName(descriptor->layer)||
          Property(d.properties,"deployment")!=a::AgentDeploymentName(descriptor->deployment)||
          Property(d.properties,"authority")!=a::AgentAuthorityClassName(descriptor->authority)||
          Property(d.properties,"policy_class")!=(lifecycle_policy?"runtime_lifecycle_agent":"operational_agent"))
        return fail(E::descriptor_mismatch,id);
      const auto valid=a::ValidateAgentPolicy(policy,*descriptor);
      if(!valid.ok)return fail(E::schema_refused,id,{},valid.diagnostic_code);
      if(d.agent_type=="agent_coordinator"){
        const auto clock=a::SelectAgentStartupClockPolicy(d,entries[n].version_uuid);
        if(!clock.ok())return fail(E::schema_refused,id,clock.field);
      }
      // This host currently admits a standalone local service only. Stored
      // cluster declarations stay installed but require separate enrollment.
      if(descriptor->cluster_only&&policy.enabled)return fail(E::cluster_admission_required,id);
      if(!Lifecycle(d,policy,entries,field))return fail(E::lifecycle_invariant,id,field);
      for(const auto& dependency:descriptor->metric_dependencies)
        if(dependency.required&&std::find(policy.required_metric_families.begin(),policy.required_metric_families.end(),
            dependency.metric_family)==policy.required_metric_families.end())return fail(E::metric_contract_mismatch,id,"required_metric_families");
      for(const auto& metric:policy.required_metric_families)
        if(std::none_of(descriptor->metric_dependencies.begin(),descriptor->metric_dependencies.end(),
            [&](const auto& dependency){return dependency.metric_family==metric;}))return fail(E::metric_contract_mismatch,id,"required_metric_families");
      for(const auto& dependency:policy.policy_dependencies){
        std::size_t target=entries.size();
        for(std::size_t j=0;j<entries.size();++j){
          const auto& candidate=entries[j].definition;
          if(candidate.kind!=K::policy||Property(candidate.properties,"policy_family")!=dependency)continue;
          if(target!=entries.size())return fail(E::dependency_missing,id,"policy_dependencies");
          target=j;
        }
        if(target==entries.size()||(policy.enabled&&Property(entries[target].definition.properties,"enabled")!="1"))
          return fail(E::dependency_missing,id,"policy_dependencies");
        edges[n][target]=true;
      }
      ++out.policy_count;
    }
    std::array<unsigned char,capacity> color{};
    const auto visit=[&](auto&& self,std::size_t n)->bool{
      if(color[n]==1){out.failing_object_uuid=entries[n].definition.definition_uuid;return false;}
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
