// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_runtime_principal_credential.hpp"

namespace scratchbird::server {

enum class NativeLocalServiceStartupSelectionError {
  none, credential_unavailable, policy_absent, policy_ambiguous,
  policy_inactive, policy_invalid, posture_not_supported, pack_absent,
  pack_ambiguous, pack_inactive, pack_invalid, pack_binding_mismatch,
  origin_not_committed
};

// Observed declarations only: neither a signature/creation-import proof nor
// provider activation, effective rights, a writable mount or BEGIN authority.
// The actual credential lease and both of its prepared readers must outlive
// every borrowed text field. Declared pack counts/signature labels are DATA,
// never evidence that a complete package was admitted or activated.
struct NativeLocalServiceStartupDeclarations {
  core::catalog::CatalogBootstrapPolicyPack pack;
  core::catalog::CatalogBootstrapPolicyProfile posture;
  core::catalog::CatalogBootstrapSecurityProvider provider;
  core::platform::Uuid pack_version_uuid, posture_version_uuid, provider_version_uuid;
  core::platform::u64 pack_definition_generation=0, posture_definition_generation=0;
};

struct NativeLocalServiceStartupSelectionResult {
  NativeLocalServiceStartupSelectionError error=
      NativeLocalServiceStartupSelectionError::credential_unavailable;
  core::catalog::CatalogValueError record_error=core::catalog::CatalogValueError::none;
  core::platform::Uuid failing_object_uuid;
  std::optional<NativeLocalServiceStartupDeclarations> declarations;
  bool ok() const noexcept {
    return error==NativeLocalServiceStartupSelectionError::none&&declarations.has_value();
  }
};

// Consume only rows privately retained by the real credential comparison.
// There is no arbitrary observation/row-array overload and no pathname reopen.
// All catalog/device/credential fences remain held by that same acquiring-
// thread owner. Selection allocates nothing and has no physical effects.
inline NativeLocalServiceStartupSelectionResult SelectNativeLocalServiceStartupDeclarations(
    const NativeRuntimePrincipalCredentialLease& credential) noexcept {
  namespace c=core::catalog;
  namespace mga=transaction::mga;
  using E=NativeLocalServiceStartupSelectionError;
  NativeLocalServiceStartupSelectionResult out;
  const auto fail=[&](E error,const core::platform::Uuid& identity){
    out.error=error;out.failing_object_uuid=identity;out.declarations.reset();return out;
  };
  if(!credential.valid())return out;
  const auto& binding=credential.binding();
  const auto& provider=credential.provider();
  const auto rows=credential.configuration_;
  const storage::database::NativeCatalogVisibleRow* posture_row=nullptr;
  const storage::database::NativeCatalogVisibleRow* pack_row=nullptr;
  // This is the same complete committed configuration read already retained
  // by the comparison, not a separately refreshed/possibly mixed generation.
  for(const auto& row:rows){
    const auto& identity=row.metadata.record.header.object_uuid.value;
    if(identity==binding.policy_uuid){
      if(posture_row)return fail(E::policy_ambiguous,identity);
      posture_row=&row;
    }
    if(identity==provider.policy_pack_uuid){
      if(pack_row)return fail(E::pack_ambiguous,identity);
      pack_row=&row;
    }
  }
  const auto active=[](const storage::database::NativeCatalogVisibleRow& row){
    return !row.provisional&&!row.metadata.record.header.deleted&&
        row.effective_lifecycle==c::CatalogObjectLifecycle::active&&
        row.effective_status==c::CatalogObjectStatus::active;
  };
  if(!posture_row)return fail(E::policy_absent,binding.policy_uuid);
  if(!active(*posture_row))return fail(E::policy_inactive,binding.policy_uuid);
  const auto& policy_metadata=posture_row->metadata;
  if(!c::CatalogBootstrapPolicyMatchesMetadata(policy_metadata)||
      policy_metadata.object_subtype!="bootstrap_policy_profile")
    return fail(E::policy_invalid,binding.policy_uuid);
  const auto posture=c::DecodeCatalogBootstrapPolicyProfile(policy_metadata.record.payload);
  out.record_error=posture.error;
  if(!posture.ok()||posture.record->profile_uuid!=binding.policy_uuid)
    return fail(E::policy_invalid,binding.policy_uuid);
  // This is the explicitly specified local-service default, not a fallback
  // for external/cluster providers or other architect-selected profiles.
  if(posture.record->area!="default_security_posture"||posture.record->mode!="deny_by_default"||
      posture.record->resource_definition_uuid||posture.record->resource_definition_generation)
    return fail(E::posture_not_supported,binding.policy_uuid);
  if(posture.record->policy_pack_uuid!=provider.policy_pack_uuid||
      posture.record->policy_pack_generation!=provider.policy_pack_generation)
    return fail(E::pack_binding_mismatch,binding.policy_uuid);
  if(!pack_row)return fail(E::pack_absent,provider.policy_pack_uuid);
  if(!active(*pack_row))return fail(E::pack_inactive,provider.policy_pack_uuid);
  const auto& pack_metadata=pack_row->metadata;
  if(!c::CatalogBootstrapPolicyMatchesMetadata(pack_metadata)||
      pack_metadata.object_subtype!="bootstrap_policy_pack")
    return fail(E::pack_invalid,provider.policy_pack_uuid);
  const auto pack=c::DecodeCatalogBootstrapPolicyPack(pack_metadata.record.payload);
  out.record_error=pack.error;
  if(!pack.ok()||pack.record->policy_pack_uuid!=provider.policy_pack_uuid)
    return fail(E::pack_invalid,provider.policy_pack_uuid);
  if(pack.record->generation!=provider.policy_pack_generation)
    return fail(E::pack_binding_mismatch,provider.policy_pack_uuid);
  // Verify the actual native origin decisions; metadata visibility alone
  // does not prove that the family's separately retained origin committed.
  const auto entries=credential.source_->selection().checkpoint_inventory.inventory.entries;
  const auto committed=[&](const auto& record){
    const auto found=std::lower_bound(entries.begin(),entries.end(),record.origin_local_transaction_id,
        [](const auto& entry,auto number){return entry.identity.local_id.value<number;});
    return found!=entries.end()&&found->identity.local_id.value==record.origin_local_transaction_id&&
        found->identity.transaction_uuid.value==record.origin_transaction_uuid.value&&
        found->identity.scope==mga::TransactionScope::local_node&&mga::HasCommittedInventoryOutcome(*found);
  };
  if(!committed(*posture.record))return fail(E::origin_not_committed,binding.policy_uuid);
  if(!committed(*pack.record))return fail(E::origin_not_committed,provider.policy_pack_uuid);
  // Pack/profile/provider definition generations, context policy epoch and
  // principal event generations are distinct. Do not infer one from another.
  out.declarations.emplace(NativeLocalServiceStartupDeclarations{
      *pack.record,*posture.record,provider,pack_row->version_uuid,posture_row->version_uuid,
      credential.provider_version_uuid(),pack_metadata.definition_version,policy_metadata.definition_version});
  out.error=E::none;return out;
}
} // namespace scratchbird::server
