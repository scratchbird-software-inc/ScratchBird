// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_local_service_startup_selection.hpp"
#include "core/catalog/catalog_memory_configuration.hpp"
#include <tuple>

namespace scratchbird::server {

enum class NativeLocalServiceStartupConfigurationError {
  none, credential_unavailable, declaration_selection, profile_invalid,
  provider_selection_absent, provider_selection_ambiguous, provider_selection_inactive,
  provider_selection_unsupported, memory_profile_absent, memory_profile_ambiguous,
  memory_profile_inactive, memory_profile_unsupported, pack_generation_mismatch,
  memory_absent, memory_ambiguous, memory_inactive, memory_invalid,
  memory_binding_mismatch, source_generation_mismatch, origin_not_committed
};
enum class NativeStartupConfigurationBindingField {
  none, definition_generation, pack_generation, profile_identity, pack_identity,
  catalog_generation, schema_epoch, security_epoch, resource_epoch
};
// Exact observed policy inputs from the same retained native checkpoint as the
// comparison. The memory body is resolved by binary identity AND its semantic
// generation; an area/mode, declared count or filesystem path cannot supply it.
// This is not policy activation, installation of a memory policy, resource/
// reference admission, a verified pack signature, a session or BEGIN authority.
// Text borrows the credential owner's retained configuration reader.
struct NativeLocalServiceStartupConfiguration {
  NativeLocalServiceStartupDeclarations declarations;
  core::catalog::CatalogBootstrapPolicyProfile provider_selection, memory_profile;
  core::catalog::CatalogMemoryConfiguration memory;
  core::platform::Uuid provider_selection_version_uuid, memory_profile_version_uuid, memory_version_uuid;
};
struct NativeLocalServiceStartupConfigurationResult {
  NativeLocalServiceStartupConfigurationError error=
      NativeLocalServiceStartupConfigurationError::credential_unavailable;
  NativeLocalServiceStartupSelectionResult selection;
  core::catalog::CatalogValueError record_error=core::catalog::CatalogValueError::none;
  core::platform::Uuid failing_object_uuid;
  NativeStartupConfigurationBindingField failing_binding=NativeStartupConfigurationBindingField::none;
  core::platform::Uuid expected_identity, observed_identity;
  core::platform::u64 expected_generation=0, observed_generation=0;
  std::optional<NativeLocalServiceStartupConfiguration> configuration;
  bool ok() const noexcept {
    return error==NativeLocalServiceStartupConfigurationError::none&&configuration.has_value();
  }
};

// No supplied row array, source path or independently constructible observation
// can issue this result. The private comparison retains both source fences and
// the exact catalog backing. Selection is bounded by that prepared reader and
// performs no allocation, provider callback, policy activation or physical I/O.
inline NativeLocalServiceStartupConfigurationResult SelectNativeLocalServiceStartupConfiguration(
    const NativeRuntimePrincipalCredentialLease& credential) noexcept {
  namespace c=core::catalog;namespace mga=transaction::mga;
  using E=NativeLocalServiceStartupConfigurationError;
  using F=NativeStartupConfigurationBindingField;
  NativeLocalServiceStartupConfigurationResult out;
  const auto fail=[&](E error,core::platform::Uuid identity){
    out.error=error;out.failing_object_uuid=identity;out.configuration.reset();return out;
  };
  if(!credential.valid()||!credential.has_selected_source())return out;
  out.selection=SelectNativeLocalServiceStartupDeclarations(credential);
  if(!out.selection.ok())return fail(E::declaration_selection,out.selection.failing_object_uuid);
  const auto& declarations=*out.selection.declarations;
  const auto rows=credential.configuration_;
  const auto& catalog=credential.selected_catalog();
  const auto entries=credential.SourceInventory();
  const auto committed=[&](const auto& definition){
    const auto found=std::lower_bound(entries.begin(),entries.end(),definition.origin_local_transaction_id,
        [](const auto& e,auto n){return e.identity.local_id.value<n;});
    return found!=entries.end()&&found->identity.local_id.value==definition.origin_local_transaction_id&&
        found->identity.transaction_uuid.value==definition.origin_transaction_uuid.value&&
        found->identity.scope==mga::TransactionScope::local_node&&mga::HasCommittedInventoryOutcome(*found);
  };
  const auto active=[](const auto& row){
    return !row.provisional&&!row.metadata.record.header.deleted&&
        row.effective_lifecycle==c::CatalogObjectLifecycle::active&&row.effective_status==c::CatalogObjectStatus::active;
  };
  const auto provenance=[&](const auto& m){
    for(const auto& [field,expected,observed]:{
        std::tuple{F::catalog_generation,catalog.catalog_generation,m.catalog_generation},
        std::tuple{F::security_epoch,catalog.security_epoch,m.security_epoch},
        std::tuple{F::schema_epoch,catalog.schema_epoch,m.schema_epoch},
        std::tuple{F::resource_epoch,catalog.resource_epoch,m.resource_epoch}}){
      if(observed>expected||(!observed&&field!=F::resource_epoch)){
        out.failing_binding=field;out.expected_generation=expected;out.observed_generation=observed;return false;
      }
    }
    return true;
  };
  const storage::database::NativeCatalogVisibleRow* provider_row=nullptr;
  const storage::database::NativeCatalogVisibleRow* memory_row=nullptr;
  std::optional<c::CatalogBootstrapPolicyProfile> provider_profile,memory_profile;
  for(const auto& row:rows){
    const auto& m=row.metadata;const auto id=m.record.header.object_uuid.value;
    if((id==declarations.pack.policy_pack_uuid||id==declarations.posture.profile_uuid||
        id==declarations.provider.provider_uuid)&&!provenance(m))return fail(E::source_generation_mismatch,id);
    if(m.object_subtype!=scratchbird::core::catalog::CatalogObjectSubtype::bootstrap_policy_profile)continue;
    const auto decoded=c::DecodeCatalogBootstrapPolicyProfile(m.record.payload);
    out.record_error=decoded.error;
    if(!decoded.ok()||!c::CatalogBootstrapPolicyMatchesMetadata(m))return fail(E::profile_invalid,id);
    const auto& p=*decoded.record;
    if(p.policy_pack_uuid!=declarations.pack.policy_pack_uuid)continue;
    // 3b: area and mode are constants (SB_CATALOG_NO_TEXT_STEP3B).
    if(p.area!=c::BootstrapPolicyArea::security_provider_selection&&p.area!=c::BootstrapPolicyArea::memory_resource_governance)continue;
    if(p.policy_pack_generation!=declarations.pack.generation){
      out.failing_binding=F::pack_generation;out.expected_generation=declarations.pack.generation;
      out.observed_generation=p.policy_pack_generation;return fail(E::pack_generation_mismatch,id);
    }
    if(!provenance(m))return fail(E::source_generation_mismatch,id);
    if(!committed(p))return fail(E::origin_not_committed,id);
    if(p.area==c::BootstrapPolicyArea::security_provider_selection){
      if(provider_row)return fail(E::provider_selection_ambiguous,id);
      if(!active(row))return fail(E::provider_selection_inactive,id);
      if(p.mode!=c::BootstrapPolicyMode::local_password_only||p.resource_definition_uuid||p.resource_definition_generation)
        return fail(E::provider_selection_unsupported,id);
      provider_row=&row;provider_profile=p;
    }else{
      if(memory_row)return fail(E::memory_profile_ambiguous,id);
      if(!active(row))return fail(E::memory_profile_inactive,id);
      if(p.mode!=c::BootstrapPolicyMode::configured_policy_required||!p.resource_definition_uuid||!p.resource_definition_generation)
        return fail(E::memory_profile_unsupported,id);
      memory_row=&row;memory_profile=p;
    }
  }
  if(!provider_row)return fail(E::provider_selection_absent,declarations.pack.policy_pack_uuid);
  if(!memory_row)return fail(E::memory_profile_absent,declarations.pack.policy_pack_uuid);
  const storage::database::NativeCatalogVisibleRow* body=nullptr;
  for(const auto& row:rows)if(row.metadata.record.header.object_uuid.value==*memory_profile->resource_definition_uuid){
    if(body)return fail(E::memory_ambiguous,*memory_profile->resource_definition_uuid);body=&row;
  }
  const auto id=*memory_profile->resource_definition_uuid;
  if(!body)return fail(E::memory_absent,id);
  if(!active(*body))return fail(E::memory_inactive,id);
  if(!c::CatalogMemoryConfigurationMatchesMetadata(body->metadata))return fail(E::memory_invalid,id);
  const auto memory=c::DecodeCatalogMemoryConfiguration(body->metadata.record.payload);
  out.record_error=memory.error;
  if(!memory.ok())return fail(E::memory_invalid,id);
  const auto& m=*memory.record;
  if(m.generation!=*memory_profile->resource_definition_generation){
    out.failing_binding=F::definition_generation;out.expected_generation=*memory_profile->resource_definition_generation;
    out.observed_generation=m.generation;return fail(E::memory_binding_mismatch,id);
  }
  if(m.profile_uuid!=memory_profile->profile_uuid){
    out.failing_binding=F::profile_identity;out.expected_identity=memory_profile->profile_uuid;
    out.observed_identity=m.profile_uuid;return fail(E::memory_binding_mismatch,id);
  }
  if(m.policy_pack_uuid!=declarations.pack.policy_pack_uuid){
    out.failing_binding=F::pack_identity;out.expected_identity=declarations.pack.policy_pack_uuid;
    out.observed_identity=m.policy_pack_uuid;return fail(E::memory_binding_mismatch,id);
  }
  if(m.policy_pack_generation!=declarations.pack.generation){
    out.failing_binding=F::pack_generation;out.expected_generation=declarations.pack.generation;
    out.observed_generation=m.policy_pack_generation;return fail(E::memory_binding_mismatch,id);
  }
  if(!provenance(body->metadata))return fail(E::source_generation_mismatch,id);
  if(!committed(m))return fail(E::origin_not_committed,id);
  out.configuration.emplace(NativeLocalServiceStartupConfiguration{declarations,*provider_profile,*memory_profile,
      m,provider_row->version_uuid,memory_row->version_uuid,body->version_uuid});
  out.error=E::none;return out;
}
} // namespace scratchbird::server
