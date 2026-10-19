// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_name_verification.hpp"
#include "native_catalog_namespace_verification.hpp"
#include "native_catalog_filespace_verification.hpp"
#include "native_catalog_audit_verification.hpp"
#include "native_catalog_security_reference_verification.hpp"
#include "native_catalog_naming_profile_verification.hpp"
#include "native_catalog_collision_verification.hpp"

namespace scratchbird::engine::internal_api {
// A source-bound component observation, never a complete startup receipt.
// Resource references, general scoped policy admission, relation contents, policy evaluation and
// lifecycle verification remain obligations of the owning operation.
struct NativeCatalogMetadataVerification {
  NativeCatalogNameVerification names;
  NativeCatalogNamespaceVerification namespaces;
  NativeCatalogFilespaceVerification filespaces;
  NativeCatalogAuditVerification audit;
  NativeCatalogSecurityReferenceVerification security;
  NativeCatalogNamingProfileVerification naming_profiles;
  NativeCatalogCollisionVerification collisions;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  bool ok() const noexcept{return selection.has_value()&&names.ok()&&namespaces.ok()&&filespaces.ok()&&audit.ok()&&security.ok()&&naming_profiles.ok()&&collisions.ok();}
};
inline NativeCatalogMetadataVerification VerifyNativeCatalogMetadata(
    const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace) noexcept {
  NativeCatalogMetadataVerification out;
  out.names=VerifyNativeCatalogNameBindings(capture,workspace.order);
  if(!out.names.ok())return out;
  out.namespaces=VerifyNativeCatalogNamespaces(capture,workspace);
  if(!out.namespaces.ok())return out;
  out.filespaces=VerifyNativeCatalogFilespaces(capture,out.namespaces.database_object_uuid,workspace.colors);
  if(!out.filespaces.ok())return out;
  out.audit=VerifyNativeCatalogAuditReferences(capture,workspace.order);
  if(!out.audit.ok())return out;
  out.security=VerifyNativeCatalogSecurityReferences(capture,workspace.order);
  if(!out.security.ok())return out;
  out.naming_profiles=VerifyNativeCatalogNamingProfileReferences(capture,workspace.order);
  if(!out.naming_profiles.ok())return out;
  out.collisions=VerifyNativeCatalogNamingCollisions(capture,workspace);
  if(out.collisions.ok())out.selection=capture.selection();
  return out;
}
} // namespace scratchbird::engine::internal_api
