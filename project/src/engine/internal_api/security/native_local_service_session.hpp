// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_local_service_authentication.hpp"
#include "native_read_source_identity.hpp"
#include "core/agents/agent_startup_clock_policy.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeLocalServiceSessionError {
  none, not_ready, busy, source_mismatch, authorization_refused, identity_failure,
  crypto_scope_unavailable, cancelled, resource_policy_required, policy_required
};
// Reporting only. An identity cannot attach to this private service or be
// submitted to an ordinary client/agent/SQL route as a security context.
struct NativeLocalServiceSessionObservation {
  core::platform::Uuid session_uuid,authentication_decision_uuid,database_uuid,principal_uuid,original_operation_uuid;
  storage::database::NativeReadSourceIdentity source;
  core::platform::Uuid clock_policy_uuid,clock_policy_version_uuid;
  core::platform::Uuid engine_instance_uuid,attachment_uuid;
};
struct NativeLocalServiceSessionResult {
  NativeLocalServiceSessionError error=NativeLocalServiceSessionError::not_ready;
  NativeLocalServiceSessionObservation observation;
  bool ok()const noexcept{return error==NativeLocalServiceSessionError::none;}
};
// The actual internal startup/recovery session. Its only factory consumes the
// privately issued authentication object under the native host's retained
// source and credential ownership. Base rights remain subject to the owning
// operation's policy/row/column/runtime checks. This lifetime establishes no
// native transaction, policy activation, worker admission or client readiness.
class NativeLocalServiceSession final {
 public:
  NativeLocalServiceSession(const NativeLocalServiceSession&)=delete;
  NativeLocalServiceSession& operator=(const NativeLocalServiceSession&)=delete;
  NativeLocalServiceSession(NativeLocalServiceSession&&) noexcept=default;
  NativeLocalServiceSession& operator=(NativeLocalServiceSession&&)=delete;
 private:
  friend class scratchbird::server::NativeHostedDatabaseRuntime;
  NativeLocalServiceSession()=default;
  static NativeLocalServiceSessionResult Attach(const NativeLocalServiceAuthentication& authentication,
      const storage::database::NativeReadSourceIdentity& source,
      const core::agents::AgentStartupClockPolicySelection& clock,const core::platform::Uuid& engine_instance,
      std::optional<NativeLocalServiceSession>& issued) noexcept {
    using E=NativeLocalServiceSessionError;
    if(issued)return {E::busy};
    const auto& identity=authentication.observation_;
    if(!clock.ok()||clock.database_uuid!=identity.database_uuid)return {E::policy_required};
    if(!core::uuid::IsEngineIdentityUuid(engine_instance)||engine_instance==identity.database_uuid||
        engine_instance==identity.operation_uuid||engine_instance==identity.decision_uuid||
        source.database_uuid!=identity.database_uuid||!source.root_set_generation||!source.source_generation||
        !core::uuid::IsEngineIdentityUuid(source.root_set_uuid)||!core::uuid::IsEngineIdentityUuid(identity.decision_uuid))
      return {E::source_mismatch};
    if(!authentication.authorization_.Allows("CONNECT",identity.database_uuid))return {E::authorization_refused};
    if(core::hash::CheckExplicitCryptoMemoryScope(identity.database_uuid)!=core::hash::CryptoMemoryError::none)
      return {E::crypto_scope_unavailable};
    // This private owner registers both the actual internal attachment and its
    // session. Neither identity substitutes for the host, credential, policy or
    // transaction lifetime. Both retire with this affine owned service object.
    const auto attachment=core::uuid::IssueRuntimeIdentityV7();
    const auto session=core::uuid::IssueRuntimeIdentityV7();
    if(!attachment||!session||*attachment==*session||*attachment==engine_instance||*session==engine_instance)
      return {E::identity_failure};
    NativeLocalServiceSession owner;
    owner.observation_={*session,identity.decision_uuid,identity.database_uuid,identity.principal_uuid,identity.operation_uuid,source};
    owner.observation_.clock_policy_uuid=clock.policy_uuid;owner.observation_.clock_policy_version_uuid=clock.version_uuid;
    owner.observation_.engine_instance_uuid=engine_instance;owner.observation_.attachment_uuid=*attachment;
    owner.clock_policy_=clock;
    owner.authorization_=authentication.authorization_;
    issued.emplace(std::move(owner));
    return {E::none,issued->observation_};
  }
  NativeLocalServiceSessionObservation observation_;
  NativeRuntimeAuthorizationProjection authorization_;
  core::agents::AgentStartupClockPolicySelection clock_policy_;
};
} // namespace scratchbird::engine::internal_api
