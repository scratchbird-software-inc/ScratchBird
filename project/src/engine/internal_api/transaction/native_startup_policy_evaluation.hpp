// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_transaction_policy_selection.hpp"
#include "native_startup_policy_types.hpp"
#include "native_startup_lifetime.hpp"
#include "engine/internal_api/security/native_runtime_authorization_projection.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeStartupPolicyEvaluationError {
  none,invalid_request,source_mismatch,invalid_policy,workspace_required,unsupported_scope,unsupported_policy
};
enum class NativeStartupPolicyPoint {transaction_start};
enum class NativeStartupPolicyTrust {server_internal};
enum class NativeStartupPolicyOperation {startup};
enum class NativeStartupPolicyDisclosure {none};
struct NativeStartupPolicyCacheRequirements {
  bool parser=false,sblr=false,plan=false,session=false,management=false,udr=false;
  // Preserve these additional descriptor obligations independently of the
  // six decision cache surfaces; their bit positions are not interchangeable.
  bool metric=false,route=false;
};
// Request/decision data only. The host supplies its actual private attachment,
// authentication, frozen source and issuer; these UUIDs cannot create authority.
struct NativeStartupPolicyRequest {
  core::platform::Uuid evaluation_uuid,database_uuid,transaction_uuid,session_uuid,policy_snapshot_uuid;
  core::platform::Uuid cluster_uuid,connection_uuid,statement_uuid;
  core::platform::u64 transaction_number=0,policy_generation_required=0;
  std::span<const core::platform::Uuid> acting_subject_uuids,target_object_uuids;
  NativeStartupPolicyPoint evaluation_point=NativeStartupPolicyPoint::transaction_start;
  NativeStartupPolicyTrust source_trust_class=NativeStartupPolicyTrust::server_internal;
  NativeStartupPolicyOperation operation_class=NativeStartupPolicyOperation::startup;
  NativeStartupPolicyDisclosure requested_disclosure_level=NativeStartupPolicyDisclosure::none;
  std::string_view requested_action="startup.transaction.admit";
};
struct NativeStartupPolicyEvaluation {
  NativeStartupPolicyEvaluationError error=NativeStartupPolicyEvaluationError::invalid_request;
  NativeStartupPolicyOutcome outcome=NativeStartupPolicyOutcome::deny;
  core::platform::Uuid decision_uuid,evaluation_uuid,decisive_policy_uuid;
  core::platform::u64 effective_generation=0;
  NativeStartupPolicyDisclosure disclosure_result=NativeStartupPolicyDisclosure::none;
  NativeStartupPolicyCacheRequirements cache_revalidation_required;
  std::span<const NativeStartupPolicyBinding> effective_policies;
  const char* reason="invalid_request";
  // Registered mga.policy_decision must precede visible branch outcome.
  // These obligations are not receipts that audit/evidence have been written.
  bool audit_required=true,evidence_required=true,retry_allowed=false;
  bool evaluated()const noexcept{return error==NativeStartupPolicyEvaluationError::none;}
};
enum class NativeStartupPolicyReadError {
  none,not_ready,busy,cancelled,source_mismatch,policy_required,authorization_refused,
  lifetime_refused,identity_failure,resource_exhausted,evaluation_failure,crypto_failure
};
// A value-owned report. It carries no source pointers or executable rights.
struct NativeStartupPolicyObservation {
  core::platform::Uuid evaluation_uuid,decision_uuid,database_uuid,transaction_uuid,session_uuid,policy_snapshot_uuid;
  core::platform::Uuid decisive_policy_uuid;
  core::platform::u64 transaction_number=0,effective_generation=0;
  std::size_t subject_count=0,target_count=0,policy_count=0;
  NativeStartupPolicyOutcome outcome=NativeStartupPolicyOutcome::deny;
  const char* reason="not_evaluated";
  NativeStartupPolicyCacheRequirements cache_revalidation_required;
  bool audit_required=true,evidence_required=true;
};
struct NativeStartupPolicyResult {
  NativeStartupPolicyReadError error=NativeStartupPolicyReadError::not_ready;
  NativeStartupPolicyEvaluationError evaluation_error=NativeStartupPolicyEvaluationError::none;
  std::optional<NativeStartupPolicyObservation> observation;
  NativeStartupLifetimeError lifetime_error=NativeStartupLifetimeError::none;
  bool evaluated()const noexcept{return error==NativeStartupPolicyReadError::none&&observation.has_value();}
};

// Complete applicable descriptor scan of the original captured source. This
// does not acquire locks, activate policies, persist audit, or admit a BEGIN.
// Caller retains capture, authorization and disjoint governed output backing.
inline NativeStartupPolicyEvaluation EvaluateNativeStartupPolicies(
    const storage::database::NativeCatalogCapture& capture,
    const NativeTransactionPolicyDeclarations& selected,
    const NativeRuntimeAuthorizationProjection& authorization,
    const NativeStartupPolicyRequest& request,const core::platform::Uuid& decision_uuid,
    std::span<NativeStartupPolicyBinding> output) noexcept {
  namespace c=core::catalog;using E=NativeStartupPolicyEvaluationError;using O=NativeStartupPolicyOutcome;
  using core::platform::Uuid;using core::platform::u64;
  const auto fail=[](E error,const char* reason){NativeStartupPolicyEvaluation r;r.error=error;r.reason=reason;return r;};
  if(!capture.ok()||!capture.inventory()||!selected.ok()||!authorization.present())return fail(E::source_mismatch,"source_required");
  const std::array ids{request.evaluation_uuid,request.database_uuid,request.transaction_uuid,request.session_uuid,request.policy_snapshot_uuid,decision_uuid};
  for(std::size_t n=0;n<ids.size();++n){if(!core::uuid::IsEngineIdentityUuid(ids[n]))return fail(E::invalid_request,"invalid_identity");
    for(std::size_t j=0;j<n;++j)if(ids[n]==ids[j])return fail(E::invalid_request,"crossed_identity");}
  if(request.transaction_number<=1||!request.policy_generation_required)return fail(E::invalid_request,"invalid_generation");
  if(!request.cluster_uuid.is_nil()||!request.connection_uuid.is_nil()||!request.statement_uuid.is_nil()||
      request.evaluation_point!=NativeStartupPolicyPoint::transaction_start||request.source_trust_class!=NativeStartupPolicyTrust::server_internal||
      request.operation_class!=NativeStartupPolicyOperation::startup||request.requested_disclosure_level!=NativeStartupPolicyDisclosure::none||
      request.requested_action!="startup.transaction.admit")return fail(E::invalid_request,"unsupported_request");
  const auto& source=*capture.selection();
  if(source.header.database_uuid!=request.database_uuid||selected.descriptor.database_uuid!=request.database_uuid||
      authorization.binding().database_uuid!=request.database_uuid||
      selected.selection->checkpoint_sha256!=source.checkpoint_sha256||
      selected.selection->selection_generation!=source.selection_generation)return fail(E::source_mismatch,"source_binding");
  if(!storage::disk::detail::DecodedRange(output).valid||reinterpret_cast<std::uintptr_t>(output.data())%alignof(NativeStartupPolicyBinding)||
      !capture.DisjointWorkspace(std::as_bytes(output))||
      !storage::disk::detail::DisjointNativeDecodeRegions(output,std::span{&request,1},std::span{&selected,1},
          std::span{&capture,1},std::span{&authorization,1},authorization.subjects(),authorization.memberships(),authorization.grants(),
          request.acting_subject_uuids,request.target_object_uuids,std::span{request.requested_action.data(),request.requested_action.size()},std::span{&decision_uuid,1}))
    return fail(E::workspace_required,"invalid_workspace");
  for(auto values:{request.acting_subject_uuids,request.target_object_uuids}){
    if(!storage::disk::detail::DecodedRange(values).valid||reinterpret_cast<std::uintptr_t>(values.data())%alignof(Uuid))
      return fail(E::invalid_request,"invalid_identity_set");
    for(std::size_t i=0;i<values.size();++i)if(!core::uuid::IsEngineIdentityUuid(values[i])||(i&&!(values[i-1]<values[i])))
      return fail(E::invalid_request,"unordered_identity_set");
  }
  if(request.acting_subject_uuids.size()!=authorization.subjects().size()||
      request.target_object_uuids.size()!=capture.filespaces().size()+1||
      !std::binary_search(request.target_object_uuids.begin(),request.target_object_uuids.end(),request.database_uuid))
    return fail(E::source_mismatch,"incomplete_request_scope");
  for(const auto& actor:authorization.subjects())
    if(!std::binary_search(request.acting_subject_uuids.begin(),request.acting_subject_uuids.end(),actor.subject_uuid))
      return fail(E::source_mismatch,"subject_binding");
  for(const auto& file:capture.filespaces())
    if(!std::binary_search(request.target_object_uuids.begin(),request.target_object_uuids.end(),file.bootstrap.filespace_uuid))
      return fail(E::source_mismatch,"target_binding");
  NativeStartupPolicyEvaluation result;result.evaluation_uuid=request.evaluation_uuid;result.decision_uuid=decision_uuid;
  const bool generation_changed=request.policy_generation_required!=selected.descriptor.generation;
  const auto subject=[&](const Uuid& id,NativeRuntimeAuthorizationSubjectKind kind){
    return std::any_of(authorization.subjects().begin(),authorization.subjects().end(),
      [&](const auto& actor){return actor.subject_uuid==id&&actor.kind==kind;});};
  const auto target=[&](const Uuid& id){return id==request.database_uuid||std::any_of(capture.filespaces().begin(),capture.filespaces().end(),
      [&](const auto& file){return file.bootstrap.filespace_uuid==id;});};
  const auto known=[&](const c::CatalogPolicyDescriptor& d,const Uuid& version){
    const auto same=[&](const auto& expected,const Uuid& expected_version){return d.policy_uuid==expected.policy_uuid&&
        version==expected_version&&d.body_uuid==expected.body_uuid&&d.body_generation==expected.body_generation&&d.generation==expected.generation;};
    if(same(selected.descriptor,selected.descriptor_version_uuid))return true;
    return std::any_of(selected.locks.begin(),selected.locks.end(),[&](const auto& lock){return same(lock.descriptor,lock.descriptor_version_uuid);});
  };
  // inventory() returns its descriptor by value; retain that descriptor while
  // its spans continue to borrow the capture's governed immutable backing.
  const auto inventory=capture.inventory()->inventory;
  const auto committed_origin=[&](const c::CatalogPolicyDescriptor& d){
    const auto at=std::lower_bound(inventory.entries.begin(),inventory.entries.end(),d.origin_local_transaction_id,
        [](const auto& entry,u64 n){return entry.identity.local_id.value<n;});
    return at!=inventory.entries.end()&&at->identity.local_id.value==d.origin_local_transaction_id&&
        at->identity.transaction_uuid.value==d.origin_transaction_uuid.value&&
        at->identity.scope==transaction::mga::TransactionScope::local_node&&transaction::mga::HasCommittedInventoryOutcome(*at);
  };
  std::size_t count=0;bool transaction_seen=false;std::array<bool,3> locks_seen{};
  // Validate/count before writing output. A second scan copies only the same
  // immutable source; no allocation or callback occurs in either pass.
  for(unsigned pass=0;pass<2;++pass){count=0;
    for(const auto& captured:capture.rows()){const auto& row=captured.row;const auto& metadata=row.metadata;
      if(metadata.record.header.deleted||metadata.object_subtype!="policy_descriptor")continue;
      if(captured.role!=5||row.provisional||row.name_payload||!core::uuid::IsEngineIdentityUuid(row.version_uuid)||
          !c::IsCatalogPolicyDescriptorPayload(metadata.record.payload)||
          !c::CatalogPolicyDescriptorMatchesDatabase(metadata,request.database_uuid))return fail(E::invalid_policy,"invalid_descriptor");
      const auto decoded=c::DecodeCatalogPolicyDescriptor(metadata.record.payload);
      if(!decoded.ok())return fail(E::invalid_policy,"invalid_descriptor");const auto& d=*decoded.record;
      if(!(d.enforcement_points&(u64{1}<<7))||row.effective_lifecycle!=c::CatalogObjectLifecycle::active||
          row.effective_status!=c::CatalogObjectStatus::active||
          (d.effective_transaction_number&&*d.effective_transaction_number>request.transaction_number)||
          (d.superseding_transaction_number&&*d.superseding_transaction_number<=request.transaction_number))continue;
      bool applies=false;
      switch(d.scope){
        case c::CatalogPolicyScope::database:applies=d.scope_uuid==request.database_uuid;break;
        case c::CatalogPolicyScope::user:applies=subject(d.scope_uuid,NativeRuntimeAuthorizationSubjectKind::principal);break;
        case c::CatalogPolicyScope::role:applies=subject(d.scope_uuid,NativeRuntimeAuthorizationSubjectKind::role);break;
        case c::CatalogPolicyScope::group:applies=subject(d.scope_uuid,NativeRuntimeAuthorizationSubjectKind::group);break;
        // This private internal-service authorization projection has no
        // external mappings. An internal principal/role/group UUID cannot be
        // reinterpreted as one. External ingress needs its own typed source.
        case c::CatalogPolicyScope::external_authorization:continue;
        case c::CatalogPolicyScope::object:case c::CatalogPolicyScope::filespace:applies=target(d.scope_uuid);break;
        case c::CatalogPolicyScope::transaction:applies=d.scope_uuid==request.transaction_uuid;break;
        // There is no statement/connection/row/UDR/cluster target in this
        // narrowly owned first-startup transaction admission request.
        default:continue;
      }
      if(!applies)continue;
      if(d.cluster_uuid)return fail(E::unsupported_scope,"cluster_policy_requires_authority");
      if(!committed_origin(d))return fail(E::invalid_policy,"uncommitted_policy_origin");
      if(count==output.size())return fail(E::workspace_required,"policy_capacity");
      if(d.policy_uuid==selected.descriptor.policy_uuid)transaction_seen=true;
      for(std::size_t i=0;i<selected.locks.size();++i)if(d.policy_uuid==selected.locks[i].descriptor.policy_uuid)locks_seen[i]=true;
      // An additional denial must not disappear behind a selected allow.
      // An additional allow body needs its actual resolver before it can be
      // treated as permission; a default label alone is not that resolver.
      if(pass)output[count]={d.policy_uuid,row.version_uuid,d.body_uuid,d.generation,d.body_generation,
          d.cache_invalidation_mask,d.default_decision,d.precedence_rank,d.scope,known(d,row.version_uuid)};
      ++count;
    }
    if(!transaction_seen||std::ranges::any_of(locks_seen,[](bool seen){return !seen;}))return fail(E::source_mismatch,"selected_policy_missing");
  }
  auto policies=output.first(count);
  const auto specificity=[](c::CatalogPolicyScope scope){
    // A transaction target is narrower than its objects/subjects, which are
    // narrower than the entire database. UUID order only breaks equal scopes.
    return scope==c::CatalogPolicyScope::transaction?3:
        scope==c::CatalogPolicyScope::object||scope==c::CatalogPolicyScope::filespace?2:
        scope==c::CatalogPolicyScope::database?0:1;
  };
  std::sort(policies.begin(),policies.end(),[&](const auto& a,const auto& b){
    if(a.precedence_rank!=b.precedence_rank)return a.precedence_rank<b.precedence_rank;
    if(specificity(a.scope)!=specificity(b.scope))return specificity(a.scope)>specificity(b.scope);
    return a.policy_uuid<b.policy_uuid;
  });
  result.effective_policies=policies;result.error=E::none;result.outcome=O::allow;result.reason="startup_policy_allow";
  for(const auto& policy:policies){result.effective_generation=std::max(result.effective_generation,policy.generation);
    const auto mask=policy.cache_invalidation_mask;auto& caches=result.cache_revalidation_required;
    caches.parser|=(mask&1)!=0;caches.sblr|=(mask&2)!=0;caches.plan|=(mask&4)!=0;caches.session|=(mask&8)!=0;
    caches.udr|=(mask&16)!=0;caches.management|=(mask&32)!=0;caches.metric|=(mask&64)!=0;caches.route|=(mask&128)!=0;
    if(policy.decision!=c::CatalogPolicyDefault::allow&&result.outcome==O::allow){
      result.decisive_policy_uuid=policy.policy_uuid;result.reason="applicable_policy_refusal";
      result.outcome=policy.decision==c::CatalogPolicyDefault::defer?O::defer:
          policy.decision==c::CatalogPolicyDefault::require_human_review?O::require_human_review:O::deny;
    }
    if(policy.decision==c::CatalogPolicyDefault::allow&&!policy.body_resolved&&result.outcome==O::allow){
      result.outcome=O::deny;result.reason="unresolved_applicable_body";result.decisive_policy_uuid=policy.policy_uuid;
    }
  }
  if(result.outcome==O::allow&&generation_changed){result.outcome=O::require_revalidation;result.reason="policy_generation_changed";}
  return result;
}
} // namespace scratchbird::engine::internal_api
