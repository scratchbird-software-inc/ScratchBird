// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_record_codec.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "uuid.hpp"

namespace scratchbird::storage::database {

// Builds the CatalogMetadataVersion envelope that
// NativeInitialPolicySecurityRequest::definitions requires, one object at a
// time. This is the repeated unit of that definitions vector: the enumeration
// of which objects exist is separate, mechanical, and belongs to the caller.
//
// Payloads stay empty by contract. AssembleNativeInitialPolicyRecordsImpl
// rejects any input whose record.payload is non-empty and fills payloads from
// its own map, so this factory must not supply one. The caller supplies
// identity, kind, subtype and name authority; the assembler supplies content.
//
// The four shared identity fields come from create's existing bootstrap
// identities rather than anything invented here:
//   owner_uuid           the bootstrap principal          (kind principal)
//   audit_uuid           sys_audit_object                 (kind object)
//   security_policy_uuid the object's governing policy     (kind object)
//   owning_schema_uuid   the owning schema root           (kind schema)
// and record.header.parent_uuid carries the same value as owning_schema_uuid
// under kind object, which CatalogRuntimeAuthorityBindingMatchesMetadata
// requires.
//
// ASSUMPTION, recorded deliberately rather than left to read as fact.
// `owning_schema` is presented with UuidKind::schema while create issues that
// identity as an object id for the schema *path* (see
// core/catalog/bootstrap_schema_roots.hpp: "Database-create assigns fresh
// UUIDv7 object IDs for these paths"). UuidKind is a label on the reference and
// is not encoded in the bytes, and the catalog-schema placement appendix treats
// schema identity as a plain UUIDv7 without constraining that label, so this is
// consistent with the specification. It is not stated by it. If the engine ever
// rejects these rows through MatchesMetadata, this labelling is the first thing
// to check: the symptom will be a binding that reads as absent rather than as
// malformed.

struct BootstrapDefinitionMetadataInput {
  core::platform::Uuid object_uuid;
  core::catalog::CatalogRecordKind kind = core::catalog::CatalogRecordKind::config_profile;
  // Must equal the subtype the assembler registered for this object, or the
  // definition is refused. Security records use CatalogRecordKindName(kind).
  std::string object_subtype;

  // Shared bootstrap identities.
  core::platform::Uuid owner_principal_uuid;
  core::platform::Uuid audit_object_uuid;
  core::platform::Uuid security_policy_uuid;
  core::platform::Uuid owning_schema_uuid;
  core::platform::TypedUuid creator_transaction_uuid;
  core::platform::u64 creator_local_transaction_id = 1;

  // Generation fields. These are cross-checked against payload contents for
  // records that carry their own: the runtime authority binding requires
  // definition_version == payload generation, catalog_generation and
  // security_epoch to agree, and security_policy_uuid to equal the payload's
  // policy_uuid. Supplying them per object rather than defaulting keeps that
  // agreement the caller's explicit choice.
  core::platform::u64 definition_version = 1;
  core::platform::u64 catalog_generation = 1;
  core::platform::u64 security_epoch = 1;
  core::platform::u64 schema_epoch = 1;
  core::platform::u64 resource_epoch = 1;
  core::platform::u64 dependency_generation = 1;
  core::platform::u64 invalidation_generation = 1;
};

enum class BootstrapDefinitionMetadataError {
  none,
  object_identity_invalid,
  owner_identity_invalid,
  audit_identity_invalid,
  policy_identity_invalid,
  schema_identity_invalid,
  transaction_identity_invalid,
  subtype_empty,
  generation_invalid,
};

struct BootstrapDefinitionMetadataResult {
  BootstrapDefinitionMetadataError error =
      BootstrapDefinitionMetadataError::object_identity_invalid;
  core::catalog::CatalogMetadataVersion metadata;
  bool ok() const noexcept {
    return error == BootstrapDefinitionMetadataError::none;
  }
};

inline const char* BootstrapDefinitionMetadataDiagnosticCode(
    BootstrapDefinitionMetadataError error) noexcept {
  using E = BootstrapDefinitionMetadataError;
  switch (error) {
    case E::none: return "";
    case E::object_identity_invalid: return "CATALOG.DEFINITION_OBJECT_IDENTITY_INVALID";
    case E::owner_identity_invalid: return "CATALOG.DEFINITION_OWNER_IDENTITY_INVALID";
    case E::audit_identity_invalid: return "CATALOG.DEFINITION_AUDIT_IDENTITY_INVALID";
    case E::policy_identity_invalid: return "CATALOG.DEFINITION_POLICY_IDENTITY_INVALID";
    case E::schema_identity_invalid: return "CATALOG.DEFINITION_SCHEMA_IDENTITY_INVALID";
    case E::transaction_identity_invalid: return "CATALOG.DEFINITION_TRANSACTION_IDENTITY_INVALID";
    case E::subtype_empty: return "CATALOG.DEFINITION_SUBTYPE_EMPTY";
    case E::generation_invalid: return "CATALOG.DEFINITION_GENERATION_INVALID";
  }
  return "CATALOG.DEFINITION_OBJECT_IDENTITY_INVALID";
}

// Every identity is validated before use. A metadata envelope assembled from a
// nil or non-engine identity encodes cleanly and is rejected much later by
// MatchesMetadata, where the failure presents as a missing object rather than a
// malformed one, so the refusal belongs here.
inline BootstrapDefinitionMetadataResult BuildBootstrapDefinitionMetadata(
    const BootstrapDefinitionMetadataInput& input) {
  namespace c = core::catalog;
  namespace p = core::platform;
  using E = BootstrapDefinitionMetadataError;
  using core::uuid::IsEngineIdentityUuid;

  BootstrapDefinitionMetadataResult out;
  if (!IsEngineIdentityUuid(input.object_uuid)) return out;
  if (input.object_subtype.empty()) {
    out.error = E::subtype_empty;
    return out;
  }
  if (!IsEngineIdentityUuid(input.owner_principal_uuid)) {
    out.error = E::owner_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.audit_object_uuid)) {
    out.error = E::audit_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.security_policy_uuid)) {
    out.error = E::policy_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.owning_schema_uuid)) {
    out.error = E::schema_identity_invalid;
    return out;
  }
  if (input.creator_transaction_uuid.kind != p::UuidKind::transaction ||
      !IsEngineIdentityUuid(input.creator_transaction_uuid.value)) {
    out.error = E::transaction_identity_invalid;
    return out;
  }
  if (!input.definition_version || !input.catalog_generation ||
      !input.security_epoch || !input.creator_local_transaction_id) {
    out.error = E::generation_invalid;
    return out;
  }

  auto& m = out.metadata;
  m.record.header.kind = input.kind;
  m.record.header.object_uuid = {p::UuidKind::object, input.object_uuid};
  // Same value as the owning schema, under object kind. MatchesMetadata
  // requires both the kinds and the equality.
  m.record.header.parent_uuid = {p::UuidKind::object, input.owning_schema_uuid};
  m.record.payload.clear();  // By contract: the assembler fills this.

  m.authority_scope = c::CatalogAuthorityScope::local;
  m.visibility = c::CatalogVisibilityClass::internal_only;
  m.lifecycle = c::CatalogObjectLifecycle::active;
  m.status = c::CatalogObjectStatus::active;

  m.definition_version = input.definition_version;
  m.catalog_generation = input.catalog_generation;
  m.security_epoch = input.security_epoch;
  m.schema_epoch = input.schema_epoch;
  m.resource_epoch = input.resource_epoch;
  m.dependency_generation = input.dependency_generation;
  m.invalidation_generation = input.invalidation_generation;
  m.creator_local_transaction_id = input.creator_local_transaction_id;

  m.owner_uuid = {p::UuidKind::principal, input.owner_principal_uuid};
  m.audit_uuid = {p::UuidKind::object, input.audit_object_uuid};
  m.security_policy_uuid = {p::UuidKind::object, input.security_policy_uuid};
  m.owning_schema_uuid = {p::UuidKind::schema, input.owning_schema_uuid};
  m.creator_transaction_uuid = input.creator_transaction_uuid;

  m.object_subtype = input.object_subtype;

  out.error = E::none;
  return out;
}

}  // namespace scratchbird::storage::database
