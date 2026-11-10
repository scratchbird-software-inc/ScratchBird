// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_name_verification.hpp"
#include "native_catalog_name_path_verification.hpp"
#include "native_catalog_name_tree_verification.hpp"
#include "native_catalog_namespace_verification.hpp"
#include "native_catalog_filespace_verification.hpp"
#include "native_catalog_audit_verification.hpp"
#include "native_catalog_security_reference_verification.hpp"
#include "native_catalog_naming_profile_verification.hpp"
#include "native_catalog_collision_verification.hpp"
#include "native_catalog_index_verification.hpp"

namespace scratchbird::engine::internal_api {
// A source-bound component observation, never a complete startup receipt.
// Resource references, general scoped policy admission, relation contents, policy evaluation and
// lifecycle verification remain obligations of the owning operation.
struct NativeCatalogMetadataVerification {
  NativeCatalogNameVerification names;
  // SB_NATIVE_NAME_PATH: every name entry's stored full path.
  NativeCatalogNamePathVerification paths;
  // SB_NATIVE_NAME_TREES: every captured name tree cell against its row.
  NativeCatalogNameTreeVerification name_trees;
  // SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS: catalog index tuples against
  // their profile and dependency-map rows, and the shared names-relation identity.
  NativeCatalogIndexVerification indexes;
  NativeCatalogNamespaceVerification namespaces;
  NativeCatalogFilespaceVerification filespaces;
  NativeCatalogAuditVerification audit;
  NativeCatalogSecurityReferenceVerification security;
  NativeCatalogNamingProfileVerification naming_profiles;
  NativeCatalogCollisionVerification collisions;
  std::optional<storage::database::NativeReadSourceIdentity> source;
  bool ok() const noexcept{return source.has_value()&&names.ok()&&paths.ok()&&name_trees.ok()&&indexes.ok()&&namespaces.ok()&&filespaces.ok()&&audit.ok()&&security.ok()&&naming_profiles.ok()&&collisions.ok();}
};
inline NativeCatalogMetadataVerification VerifyNativeCatalogMetadata(
    const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace) noexcept {
  NativeCatalogMetadataVerification out;
  out.names=VerifyNativeCatalogNameBindings(capture,workspace.order);
  if(!out.names.ok())return out;
  out.indexes=VerifyNativeCatalogIndexBindings(capture);
  if(!out.indexes.ok())return out;
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
  if(!out.collisions.ok())return out;
  // Last: a path rests on the bindings, the namespace forest and the
  // filespace and policy references checked above.
  out.paths=VerifyNativeCatalogNamePaths(capture,workspace);
  if(!out.paths.ok())return out;
  out.name_trees=VerifyNativeCatalogNameTrees(capture,workspace);
  if(out.name_trees.ok())out.source=capture.source();
  return out;
}
} // namespace scratchbird::engine::internal_api
