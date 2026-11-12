// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "local_password_credential.hpp"
#include "native_runtime_authorization_projection.hpp"
#include "core/catalog/catalog_bootstrap_policy.hpp"
#include "core/hash/crypto_memory_adapter.hpp"
#include "core/memory/memory.hpp"
#include <stop_token>

namespace scratchbird::server { class NativeHostedDatabaseRuntime; }

namespace scratchbird::engine::internal_api {

enum class NativeLocalServiceAuthenticationError {
  none, not_ready, busy, selected_guards_held, resource_policy_required,
  cancelled, provider_disabled, provider_unsupported, policy_mismatch,
  principal_mismatch, authorization_refused, protected_workspace_required,
  crypto_scope_unavailable, credential_rejected, provider_failure,
  identity_failure
};

// These values describe a completed in-process service authentication. They
// cannot be submitted as authority to attach a session or begin a transaction.
struct NativeLocalServiceAuthenticationObservation {
  core::platform::Uuid decision_uuid, database_uuid, principal_uuid;
  core::platform::Uuid provider_uuid, provider_version_uuid, binding_version_uuid;
  core::platform::Uuid policy_uuid, policy_version_uuid, selection_version_uuid;
  core::platform::Uuid operation_uuid, credential_reference_uuid;
  core::platform::u64 provider_generation=0, credential_source_generation=0;
  bool operator==(const NativeLocalServiceAuthenticationObservation&) const=default;
};
struct NativeLocalServiceAuthenticationResult {
  NativeLocalServiceAuthenticationError error=NativeLocalServiceAuthenticationError::not_ready;
  LocalPasswordCredentialError verifier_error=LocalPasswordCredentialError::none;
  core::hash::CryptoMemoryError crypto_error=core::hash::CryptoMemoryError::none;
  NativeLocalServiceAuthenticationObservation observation;
  bool ok() const noexcept {return error==NativeLocalServiceAuthenticationError::none;}
};

// An engine-issued authentication decision for the internal local service.
// The native host retains the actual checkpoint operation, protected credential
// generation and governed catalog/grant backing for its entire lifetime. This
// object has no public constructor or factory accepting observations/booleans.
// Only that custodial adapter may invoke the engine verifier below. External
// clients still require their own authentication exchange and session attach.
// Authentication does not issue configuration/resource epochs, activate MGA,
// attach a session, or admit coordinator children.
class NativeLocalServiceAuthentication final {
 public:
  NativeLocalServiceAuthentication(const NativeLocalServiceAuthentication&)=delete;
  NativeLocalServiceAuthentication& operator=(const NativeLocalServiceAuthentication&)=delete;
  NativeLocalServiceAuthentication(NativeLocalServiceAuthentication&&)=default;
  NativeLocalServiceAuthentication& operator=(NativeLocalServiceAuthentication&&)=delete;
 private:
  friend class scratchbird::server::NativeHostedDatabaseRuntime;
  friend class NativeLocalServiceSession;
  NativeLocalServiceAuthentication()=default;
  struct Inputs {
    const core::catalog::CatalogRuntimeAuthorityBinding& binding;
    const NativeRuntimeServicePrincipalObservation& principal;
    const core::catalog::CatalogBootstrapSecurityProvider& provider;
    const core::catalog::CatalogBootstrapPolicyProfile& posture;
    const core::catalog::CatalogBootstrapPolicyProfile& selection;
    const NativeRuntimeAuthorizationProjection& authorization;
    NativeLocalServiceAuthenticationObservation identity;
    // 3a-2: the principal's stored verifier as the typed record's binary parts.
    LocalPasswordStoredVerifier verifier;
    std::string_view credential;
    const core::memory::ScopedProtectedBuffer& derived;
    std::stop_token credential_stop, source_stop;
  };

  // Call only after the actual device guards have retired. Cryptographic
  // provider allocation belongs to the database's admitted memory scope.
  static NativeLocalServiceAuthenticationResult Authenticate(const Inputs& in,
      std::optional<NativeLocalServiceAuthentication>& issued) noexcept {
    using E=NativeLocalServiceAuthenticationError;
    namespace c=core::catalog;
    NativeLocalServiceAuthenticationResult out;
    const auto fail=[&](E error){out.error=error;return out;};
    if(issued)return fail(E::busy);
    const auto cancelled=[&]{return in.credential_stop.stop_requested()||in.source_stop.stop_requested();};
    if(cancelled())return fail(E::cancelled);
    if(!in.provider.enabled_by_default)return fail(E::provider_disabled);
    if(in.provider.family!=c::BootstrapProviderFamily::local_password||
        in.provider.authority!=c::BootstrapProviderAuthority::durable_catalog||in.provider.external_provider||
        in.provider.verifier_policy!=c::BootstrapVerifierPolicy::local_password_verifier_required)
      return fail(E::provider_unsupported);
    if(in.selection.area!="security_provider_selection"||in.selection.mode!="local_password_only"||
        in.posture.area!="default_security_posture"||in.posture.mode!="deny_by_default"||
        in.posture.profile_uuid!=in.binding.policy_uuid||
        in.selection.policy_pack_uuid!=in.provider.policy_pack_uuid||
        in.posture.policy_pack_uuid!=in.provider.policy_pack_uuid||
        in.selection.policy_pack_generation!=in.provider.policy_pack_generation||
        in.posture.policy_pack_generation!=in.provider.policy_pack_generation)
      return fail(E::policy_mismatch);
    const auto& identity=in.identity;
    for(const auto& id:{identity.database_uuid,identity.principal_uuid,identity.provider_uuid,
        identity.provider_version_uuid,identity.binding_version_uuid,identity.policy_uuid,
        identity.policy_version_uuid,identity.selection_version_uuid,identity.operation_uuid,
        identity.credential_reference_uuid})
      if(!core::uuid::IsEngineIdentityUuid(id))return fail(E::principal_mismatch);
    if(identity.database_uuid!=in.binding.database_uuid||
        identity.principal_uuid!=in.binding.service_principal_uuid||
        identity.principal_uuid!=in.principal.principal_uuid||
        identity.provider_uuid!=in.binding.provider_uuid||identity.provider_uuid!=in.provider.provider_uuid||
        identity.policy_uuid!=in.binding.policy_uuid||
        identity.provider_generation!=in.binding.provider_generation||
        identity.provider_generation!=in.provider.generation||!identity.credential_source_generation||
        in.binding.credential_reference_uuid!=identity.credential_reference_uuid||
        in.authorization.binding().binding_uuid!=in.binding.binding_uuid||
        in.authorization.principal().principal_uuid!=identity.principal_uuid)
      return fail(E::principal_mismatch);
    if(!in.authorization.Allows(core::catalog::CatalogSecurityRight::CONNECT,identity.database_uuid))return fail(E::authorization_refused);
    const auto& derived=in.derived;
    if(!derived.valid()||derived.size()!=32||!derived.evidence().platform_lock_succeeded||
        !derived.evidence().no_dump_succeeded||
        derived.tag().binary_ownership[core::memory::MemoryBinaryScopeKind::database]!=identity.database_uuid.bytes)
      return fail(E::protected_workspace_required);
    struct Erase {void* bytes;~Erase(){OPENSSL_cleanse(bytes,32);}} erase{derived.data()};
    out.crypto_error=core::hash::CheckExplicitCryptoMemoryScope(identity.database_uuid);
    if(out.crypto_error!=core::hash::CryptoMemoryError::none)return fail(E::crypto_scope_unavailable);
    const auto verified=VerifyLocalPasswordCredential(in.verifier,in.credential,
        std::span<std::byte,32>{static_cast<std::byte*>(derived.data()),32});
    out.verifier_error=verified.error;
    if(!verified.verified())return fail(verified.error==LocalPasswordCredentialError::provider_failure?
        E::provider_failure:E::credential_rejected);
    if(cancelled())return fail(E::cancelled);
    out.crypto_error=core::hash::CheckExplicitCryptoMemoryScope(identity.database_uuid);
    if(out.crypto_error!=core::hash::CryptoMemoryError::none)return fail(E::crypto_scope_unavailable);
    const auto decision=core::uuid::IssueRuntimeIdentityV7();
    if(!decision)return fail(E::identity_failure);
    if(cancelled())return fail(E::cancelled);
    out.observation=identity;out.observation.decision_uuid=*decision;
    NativeLocalServiceAuthentication decision_owner;
    decision_owner.observation_=out.observation;decision_owner.authorization_=in.authorization;
    issued.emplace(std::move(decision_owner));
    out.error=E::none;return out;
  }
  NativeLocalServiceAuthenticationObservation observation_;
  NativeRuntimeAuthorizationProjection authorization_;
};
} // namespace scratchbird::engine::internal_api
