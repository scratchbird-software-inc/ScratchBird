// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "security/durable_authorization_projection.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include <memory>
#include <stdexcept>
#include <system_error>

namespace scratchbird::engine::internal_api {
enum class StorageActionSecurityKind { growth, preallocation };
enum class StorageActionSecurityError {
  none, invalid_request, source_failure, stale_context, materialization_failure,
  permission_denied, resource_exhausted, lock_failure
};
struct StorageActionSecurityResult;
// Thread-affine, single owning-database security fence. Not authentication,
// filespace ownership, native catalog/serving admission, MGA permission,
// resource approval, or a transferable execution capability. The engine must
// already own/authenticate the routed context and use its exact owner path.
// Keep this fence through the action's remaining admission and effects. Do not
// recursively mutate security/finality on this thread while consuming it.
class StorageActionSecurityLease {
 public:
  StorageActionSecurityLease(const StorageActionSecurityLease&)=delete;
  StorageActionSecurityLease& operator=(const StorageActionSecurityLease&)=delete;
  const EngineUuid& database() const noexcept {return database_;}
  const EngineUuid& filespace() const noexcept {return filespace_;}
  const EngineUuid& operation() const noexcept {return operation_;}
  StorageActionSecurityKind action() const noexcept {return action_;}
  const EngineMaterializedAuthorizationContext& authorization() const noexcept {return authorization_;}
 private:
  friend StorageActionSecurityResult AcquireStorageActionSecurityLease(
      const EngineRequestContext&,const EngineUuid&,const EngineUuid&,StorageActionSecurityKind) noexcept;
  StorageActionSecurityLease(std::unique_lock<std::recursive_mutex> guard,
      const EngineUuid& database,const EngineUuid& filespace,const EngineUuid& operation,
      StorageActionSecurityKind action,EngineMaterializedAuthorizationContext authorization)
      : guard_(std::move(guard)),database_(database),filespace_(filespace),operation_(operation),
        action_(action),authorization_(std::move(authorization)) {}
  std::unique_lock<std::recursive_mutex> guard_;
  EngineUuid database_,filespace_,operation_;
  StorageActionSecurityKind action_;
  EngineMaterializedAuthorizationContext authorization_;
};
struct StorageActionSecurityResult {
  StorageActionSecurityError error=StorageActionSecurityError::invalid_request;
  std::unique_ptr<StorageActionSecurityLease> lease;
  std::vector<EngineApiDiagnostic> diagnostics;
  bool ok() const noexcept {return error==StorageActionSecurityError::none&&lease!=nullptr;}
};
// STORAGE-ACTION-CURRENT-SECURITY-FENCE-001. Consume the actual owning security
// loader and shared projection, never runtime option envelopes or supplied
// grant rows. Latest committed state is read under the same inventory guard
// held by engine security append/finality. A successful result retains it.
inline StorageActionSecurityResult AcquireStorageActionSecurityLease(
    const EngineRequestContext& authenticated,const EngineUuid& filespace,
    const EngineUuid& operation,StorageActionSecurityKind action) noexcept {
  using E=StorageActionSecurityError;
  StorageActionSecurityResult result;
  const auto fail=[&](E error){result.error=error;return std::move(result);};
  try {
    const auto& observed=authenticated.authorization_context;
    if(authenticated.database_path.empty()||authenticated.database_path.find('\0')!=std::string::npos||
        !authenticated.security_context_present||!observed.present||
        (action!=StorageActionSecurityKind::growth&&action!=StorageActionSecurityKind::preallocation))return result;
    for(const auto* id:{&authenticated.database_uuid,&authenticated.principal_uuid,&filespace,&operation})
      if(!core::uuid::IsEngineIdentityUuid(*id))return result;
    if(observed.authority_uuid!=authenticated.database_uuid||observed.principal_uuid!=authenticated.principal_uuid||
        !observed.security_context_generation||!observed.security_epoch||!observed.policy_epoch||
        !authenticated.catalog_generation_id||observed.catalog_generation_id!=authenticated.catalog_generation_id||
        authenticated.security_epoch!=observed.security_epoch)return fail(E::stale_context);
    auto guard=AcquireTransactionInventoryGuard(authenticated.database_path);
    EngineRequestContext source;
    source.database_path=authenticated.database_path;source.database_uuid=authenticated.database_uuid;
    // No caller transaction, snapshot or trace tags: uncommitted grants and
    // caller-retained snapshots must never establish current mutation rights.
    auto loaded=LoadSecurityPrincipalLifecycleState(source);
    if(!loaded.ok){result.diagnostics.push_back(std::move(loaded.diagnostic));return fail(E::source_failure);}
    const auto& state=loaded.state;
    if(state.security_context_generation!=observed.security_context_generation||
        !state.security_generation||state.security_generation!=observed.security_epoch||
        !state.policy_generation||state.policy_generation!=observed.policy_epoch)return fail(E::stale_context);
    auto durable=ProjectDurableAuthorizationState(state,{authenticated.database_uuid,
        authenticated.principal_uuid,state.security_generation,state.policy_generation,
        authenticated.catalog_generation_id});
    // Resolve immutable bootstrap-role identity from the same owning catalog;
    // never infer it from a role name or accept the caller's marker.
    const auto bootstrap=ResolveEngineOwnedSysarchRoleIdentity(source);
    if(!bootstrap.ok){result.diagnostics.push_back(bootstrap.diagnostic);return fail(E::source_failure);}
    durable.engine_owned_sysarch_role_uuid=bootstrap.role_uuid;
    auto materialized=MaterializeDurableAuthorizationContext(durable,
        {authenticated.principal_uuid,observed.security_epoch,observed.policy_epoch,observed.catalog_generation_id});
    if(!materialized.ok){result.diagnostics=std::move(materialized.diagnostics);return fail(E::materialization_failure);}
    const auto check=[&](const char* right,const EngineUuid& target){
      auto decision=EvaluateMaterializedAuthorization(authenticated,materialized.context,right,target);
      if(decision.authorized&&!decision.denied&&!decision.policy_recheck_required)return true;
      result.diagnostics=std::move(decision.diagnostics);
      if(result.diagnostics.empty())result.diagnostics.push_back(MakeSecurityDiagnostic(
          "SECURITY.AUTHORIZATION.DENIED",std::string("storage_action_unresolved_right:")+right));
      return false;
    };
    if(!check("OBS_AGENT_CONTROL",authenticated.database_uuid))return fail(E::permission_denied);
    if(action==StorageActionSecurityKind::growth&&
        (!check("FILESPACE_LIFECYCLE_CONTROL",filespace)||!check("OBS_AGENT_ACTION_APPROVE",filespace)))
      return fail(E::permission_denied);
    result.lease.reset(new StorageActionSecurityLease(std::move(guard),authenticated.database_uuid,
        filespace,operation,action,std::move(materialized.context)));
    result.error=E::none;return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
   catch(const std::system_error&){return fail(E::lock_failure);}
}
} // namespace scratchbird::engine::internal_api
