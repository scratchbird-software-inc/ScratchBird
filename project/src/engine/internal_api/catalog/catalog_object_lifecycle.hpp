// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "api_diagnostics.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_OBJECT_LIFECYCLE
// Engine-owned catalog schema/object lifecycle primitives. SQL text and parser
// packages are not authority here; callers provide UUID-resolved internal API
// requests and receive UUID-first catalog identities.

inline constexpr const char* kCatalogObjectLifecycleEventMagic = "SBCATOBJ1";

inline constexpr const char* kCatalogObjectDiagnosticUuidRequired = "CATALOG.OBJECT.UUID_REQUIRED";
inline constexpr const char* kCatalogObjectDiagnosticKindRequired = "CATALOG.OBJECT.KIND_REQUIRED";
inline constexpr const char* kCatalogObjectDiagnosticSchemaUuidRequired = "CATALOG.OBJECT.SCHEMA_UUID_REQUIRED";
inline constexpr const char* kCatalogObjectDiagnosticNameRequired = "CATALOG.OBJECT.NAME_REQUIRED";
inline constexpr const char* kCatalogObjectDiagnosticDuplicateName = "CATALOG.OBJECT.DUPLICATE_NAME";
inline constexpr const char* kCatalogObjectDiagnosticNotFound = "CATALOG.OBJECT.NOT_FOUND";
inline constexpr const char* kCatalogObjectDiagnosticNameNotFound = "CATALOG.OBJECT.NAME_NOT_FOUND";
inline constexpr const char* kCatalogObjectDiagnosticDependencyTargetNotVisible = "CATALOG.OBJECT.DEPENDENCY_TARGET_NOT_VISIBLE";
inline constexpr const char* kCatalogObjectDiagnosticDependencyBlockedDrop = "CATALOG.OBJECT.DEPENDENCY_BLOCKED_DROP";
inline constexpr const char* kCatalogObjectDiagnosticSchemaOwnerDenied = "CATALOG.OBJECT.SCHEMA_OWNER_DENIED";
inline constexpr const char* kCatalogObjectDiagnosticMgaTransactionRequired = "CATALOG.OBJECT.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kCatalogObjectDiagnosticMgaVisibilityRefused = "CATALOG.OBJECT.MGA_VISIBILITY_REFUSED";
inline constexpr const char* kCatalogObjectDiagnosticCacheEpochStale = "CATALOG.OBJECT.CACHE_EPOCH_STALE";
inline constexpr const char* kCatalogObjectDiagnosticDatabasePathRequired = "CATALOG.OBJECT.DATABASE_PATH_REQUIRED";
inline constexpr const char* kCatalogObjectDiagnosticDatabaseWriteFailed = "CATALOG.OBJECT.DATABASE_WRITE_FAILED";
inline constexpr const char* kCatalogSynonymDiagnosticTargetMissing = "CATALOG.SYNONYM_TARGET_MISSING";
inline constexpr const char* kCatalogSynonymDiagnosticTargetClassMismatch = "CATALOG.SYNONYM_TARGET_CLASS_MISMATCH";
inline constexpr const char* kCatalogSynonymDiagnosticCycle = "CATALOG.SYNONYM_CYCLE";
inline constexpr const char* kCatalogSynonymDiagnosticDepthExceeded = "CATALOG.SYNONYM_DEPTH_EXCEEDED";
inline constexpr const char* kCatalogSynonymDiagnosticParentNotAllowed = "CATALOG.SYNONYM_PARENT_NOT_ALLOWED";
inline constexpr const char* kCatalogSynonymDiagnosticNameConflict = "CATALOG.SYNONYM_NAME_CONFLICT";
inline constexpr const char* kCatalogSynonymDiagnosticDependencyInvalid = "CATALOG.SYNONYM_DEPENDENCY_INVALID";
inline constexpr const char* kCatalogSynonymDiagnosticPermissionDenied = "CATALOG.SYNONYM_PERMISSION_DENIED";
inline constexpr std::uint64_t kCatalogSynonymMaxDereferenceDepth = 5;
inline constexpr const char* kCatalogConstraintDiagnosticUuidRequired = "CATALOG.CONSTRAINT_UUID_REQUIRED";
inline constexpr const char* kCatalogConstraintDiagnosticKindRequired = "CATALOG.CONSTRAINT_KIND_REQUIRED";
inline constexpr const char* kCatalogConstraintDiagnosticDescriptorInvalid = "CATALOG.CONSTRAINT_DESCRIPTOR_INVALID";
inline constexpr const char* kCatalogConstraintDiagnosticDuplicateName = "CATALOG.CONSTRAINT_DUPLICATE_NAME";
inline constexpr const char* kCatalogConstraintDiagnosticSupportRequired = "CATALOG.CONSTRAINT_SUPPORT_REQUIRED";
inline constexpr const char* kCatalogConstraintDiagnosticSupportFamilyUnsupported = "CATALOG.CONSTRAINT_SUPPORT_FAMILY_UNSUPPORTED";
inline constexpr const char* kCatalogConstraintDiagnosticDependencyInvalid = "CATALOG.CONSTRAINT_DEPENDENCY_INVALID";

struct EngineCatalogObjectRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string object_uuid;
  std::string object_kind;
  std::string schema_uuid;
  std::string owner_principal_uuid;
  std::string lifecycle_state = "active";
  std::uint64_t definition_epoch = 0;
  std::uint64_t metadata_epoch = 0;
  std::string payload;
  bool deleted = false;
};

struct EngineCatalogNameRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string name_entry_uuid;
  std::string object_uuid;
  std::string object_kind;
  std::string schema_uuid;
  std::string language_tag = "en";
  std::string name_class = "primary";
  std::string identifier_profile_uuid = "sbsql_v3";
  std::string raw_name_text;
  std::string display_name;
  std::string normalized_lookup_key;
  std::string exact_lookup_key;
  bool requires_exact_match = false;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogDependencyRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string source_uuid;
  std::string source_kind;
  std::string dependency_uuid;
  std::string dependency_kind;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogColumnMetadataRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string column_uuid;
  std::string owner_object_uuid;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string default_expression_envelope;
  std::uint32_t ordinal = 0;
  bool nullable = true;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogConstraintDescriptorRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string constraint_uuid;
  std::string constraint_class;
  std::string owner_object_uuid;
  std::string name_ref_uuid;
  std::string constraint_policy_version_uuid;
  std::string enforcement_timing = "immediate";
  std::string validation_state = "unvalidated";
  std::string trust_state = "untrusted";
  std::string support_requirement = "optional";
  std::string predicate_sblr_uuid;
  std::string diagnostic_profile_uuid;
  std::string metrics_profile_uuid;
  std::string conformance_profile_uuid;
  std::string constraint_hash;
  std::string canonical_constraint_envelope;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogKeyDescriptorRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string key_descriptor_uuid;
  std::string constraint_uuid;
  std::string key_class;
  std::string owner_object_uuid;
  std::string component_order_hash;
  std::string comparison_profile_hash;
  std::string null_policy = "not_applicable";
  std::string canonical_encoding_uuid;
  bool candidate_reference_allowed = true;
  std::string key_state = "active";
  std::string key_hash;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogConstraintSubjectRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string subject_uuid;
  std::string constraint_uuid;
  std::string subject_kind;
  std::string subject_object_uuid;
  std::string subject_descriptor;
  std::string expression_sblr_uuid;
  std::uint32_t ordinal = 0;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogConstraintDependencyRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string dependency_uuid;
  std::string constraint_uuid;
  std::string dependency_kind;
  std::string dependency_object_uuid;
  std::string dependency_version_uuid;
  std::string invalidation_action;
  std::string dependency_hash;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogConstraintSupportStructureRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string support_binding_uuid;
  std::string constraint_uuid;
  std::string support_uuid;
  std::string support_class;
  std::string support_family;
  std::string coverage_scope_hash;
  std::string durability_class;
  std::string residency_class;
  std::string validity_state;
  std::string enforcement_role;
  std::string binding_hash;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineCatalogObjectLifecycleState {
  std::vector<EngineCatalogObjectRecord> objects;
  std::vector<EngineCatalogNameRecord> names;
  std::vector<EngineCatalogDependencyRecord> dependencies;
  std::vector<EngineCatalogColumnMetadataRecord> columns;
  std::vector<EngineCatalogConstraintDescriptorRecord> constraints;
  std::vector<EngineCatalogKeyDescriptorRecord> key_descriptors;
  std::vector<EngineCatalogConstraintSubjectRecord> constraint_subjects;
  std::vector<EngineCatalogConstraintDependencyRecord> constraint_dependencies;
  std::vector<EngineCatalogConstraintSupportStructureRecord> constraint_support_structures;
  std::uint64_t metadata_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
};

struct EngineCatalogSynonymResolutionResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineCatalogObjectRecord final_object;
  std::vector<std::string> synonym_chain;
};

struct EngineLoadCatalogObjectLifecycleStateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineCatalogObjectLifecycleState state;
};

struct EngineCatalogCreateObjectRequest : EngineApiRequest {};
struct EngineCatalogCreateObjectResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogCreateObjectResult EngineCatalogCreateObject(const EngineCatalogCreateObjectRequest& request);

struct EngineCatalogApplyConstraintsRequest : EngineApiRequest {};
struct EngineCatalogApplyConstraintsResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogApplyConstraintsResult EngineCatalogApplyConstraintsToObject(
    const EngineCatalogApplyConstraintsRequest& request);

struct EngineCatalogAlterObjectRequest : EngineApiRequest {};
struct EngineCatalogAlterObjectResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogAlterObjectResult EngineCatalogAlterObject(const EngineCatalogAlterObjectRequest& request);

struct EngineCatalogRenameObjectRequest : EngineApiRequest {};
struct EngineCatalogRenameObjectResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogRenameObjectResult EngineCatalogRenameObject(const EngineCatalogRenameObjectRequest& request);

struct EngineCatalogDropObjectRequest : EngineApiRequest {};
struct EngineCatalogDropObjectResult : EngineApiResult {
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogDropObjectResult EngineCatalogDropObject(const EngineCatalogDropObjectRequest& request);

struct EngineCatalogLookupObjectRequest : EngineApiRequest {};
struct EngineCatalogLookupObjectResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogLookupObjectResult EngineCatalogLookupObjectByUuid(const EngineCatalogLookupObjectRequest& request);

struct EngineCatalogResolveObjectNameRequest : EngineApiRequest {};
struct EngineCatalogResolveObjectNameResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogResolveObjectNameResult EngineCatalogResolveObjectName(const EngineCatalogResolveObjectNameRequest& request);

struct EngineCatalogValidateMetadataCacheRequest : EngineApiRequest {};
struct EngineCatalogValidateMetadataCacheResult : EngineApiResult {
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCatalogValidateMetadataCacheResult EngineCatalogValidateMetadataCache(
    const EngineCatalogValidateMetadataCacheRequest& request);

EngineLoadCatalogObjectLifecycleStateResult LoadCatalogObjectLifecycleState(
    const EngineRequestContext& context);
bool EngineCatalogObjectCanOwnChildren(const std::string& object_kind);
EngineCatalogSynonymResolutionResult ResolveCatalogSynonymChain(
    const EngineCatalogObjectLifecycleState& state,
    const EngineCatalogObjectRecord& candidate,
    const EngineRequestContext& context,
    const std::string& required_final_object_kind = {});

}  // namespace scratchbird::engine::internal_api
