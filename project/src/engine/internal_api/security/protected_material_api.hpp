// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_PROTECTED_MATERIAL_API
// Engine-owned protected-material and encryption-key lifecycle. The API may
// admit key authority and publish opaque handles, but it never returns or
// persists plaintext secret material.
struct EngineProtectedMaterialCacheEntry {
  std::string database_uuid;
  std::string key_uuid;
  std::string key_handle;
  std::string key_fingerprint;
  std::string key_label;
  std::string filespace_uuid;
  std::uint64_t generation = 0;
  std::uint64_t admitted_at_epoch_millis = 0;
  std::uint64_t expires_at_epoch_millis = 0;
  bool active = false;
  bool purged = false;
  bool expired = false;
};

struct EngineProtectedMaterialPolicySet {
  std::string retention_policy_uuid;
  std::string access_policy_uuid;
  std::string release_policy_uuid;
  std::string purge_policy_uuid;
  std::string audit_policy_uuid;
  std::uint64_t retention_until_epoch_millis = 0;
  bool legal_hold = false;
  std::vector<std::string> release_purposes;
};

struct EngineProtectedMaterialCatalogEntry {
  std::string database_uuid;
  std::string protected_material_uuid;
  std::string object_class;
  std::string owner_scope_uuid;
  std::string purpose_class;
  std::string storage_class;
  std::string lifecycle_state = "active";
  std::string active_version_uuid;
  EngineProtectedMaterialPolicySet policy;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t created_local_transaction_id = 0;
  std::uint64_t updated_local_transaction_id = 0;
  std::uint64_t security_epoch = 0;
  bool purged = false;
};

struct EngineProtectedMaterialVersionCatalogEntry {
  std::string database_uuid;
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::uint64_t version_number = 0;
  std::string protected_reference;
  std::string envelope_reference;
  std::string payload_hash;
  std::string storage_class;
  std::string rotation_state = "active";
  EngineProtectedMaterialPolicySet policy;
  std::uint64_t valid_from_local_transaction_id = 0;
  std::uint64_t valid_until_local_transaction_id = 0;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  bool active = false;
  bool purged = false;
};

struct EngineProtectedMaterialAuditEvent {
  std::string audit_event_uuid;
  std::string database_uuid;
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string actor_uuid;
  std::string event_kind;
  std::string decision;
  std::string diagnostic_code;
  std::string redacted_detail;
  std::uint64_t event_epoch_millis = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t catalog_generation_id = 0;
  bool redaction_applied = true;
};

struct EngineAdmitEncryptionKeyRequest : EngineApiRequest {
  std::string key_uuid;
  std::string key_label;
  std::string filespace_uuid;
  std::string secret_evidence;
  std::uint64_t cache_ttl_millis = 300000;
};

struct EngineAdmitEncryptionKeyResult : EngineApiResult {
  bool key_admitted = false;
  bool cache_entry_active = false;
  bool plaintext_material_returned = false;
  std::string key_handle;
  std::string key_fingerprint;
  std::uint64_t key_generation = 0;
  std::uint64_t expires_at_epoch_millis = 0;
};

struct EngineRotateEncryptionKeyRequest : EngineApiRequest {
  std::string key_uuid;
  std::string replacement_key_uuid;
  std::string replacement_secret_evidence;
  std::string rotation_reason;
  std::uint64_t cache_ttl_millis = 300000;
};

struct EngineRotateEncryptionKeyResult : EngineApiResult {
  bool rotated = false;
  bool rotation_metadata_persisted = false;
  bool plaintext_material_persisted = false;
  std::string previous_key_uuid;
  std::string active_key_uuid;
  std::string active_key_handle;
  std::uint64_t active_generation = 0;
};

struct EngineInspectProtectedMaterialCacheRequest : EngineApiRequest {
  std::string key_uuid;
};

struct EngineInspectProtectedMaterialCacheResult : EngineApiResult {
  std::vector<EngineProtectedMaterialCacheEntry> entries;
  std::uint64_t active_entry_count = 0;
  bool protected_material_redacted = true;
};

struct EnginePurgeProtectedMaterialRequest : EngineApiRequest {
  std::string purge_reason;
};

struct EnginePurgeProtectedMaterialResult : EngineApiResult {
  bool purged = false;
  std::uint64_t purged_entry_count = 0;
};

struct EngineShutdownProtectedMaterialRequest : EngineApiRequest {
  std::string shutdown_reason;
};

struct EngineShutdownProtectedMaterialResult : EngineApiResult {
  bool shutdown_purge_complete = false;
  std::uint64_t purged_entry_count = 0;
};

struct EngineOpenEncryptedFilespaceRequest : EngineApiRequest {
  std::string database_uuid;
  std::string filespace_uuid;
  std::string key_uuid;
  std::string key_handle;
  bool encrypted_filespace = true;
  bool decryption_required = true;
};

struct EngineOpenEncryptedFilespaceResult : EngineApiResult {
  bool open_admitted = false;
  bool open_refused = false;
  bool encrypted_filespace = true;
  bool key_cache_hit = false;
  bool key_expired = false;
  bool plaintext_material_returned = false;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string key_handle;
  std::uint64_t key_generation = 0;
};

struct EngineRequestProtectedMaterialRequest : EngineApiRequest {
  std::string purpose;
  std::string key_handle;
};
struct EngineRequestProtectedMaterialResult : EngineApiResult {
  bool released = false;
  bool redaction_applied = true;
  bool plaintext_material_returned = false;
  std::string protected_material_ref;
};

struct EngineCreateProtectedMaterialRequest : EngineApiRequest {
  std::string protected_material_uuid;
  std::string object_class = "protected_material";
  std::string owner_scope_uuid;
  std::string purpose_class;
  std::string storage_class = "wrapped";
  EngineProtectedMaterialPolicySet policy;
  std::string initial_version_uuid;
  std::string protected_reference;
  std::string envelope_reference;
  std::string payload_hash;
};

struct EngineCreateProtectedMaterialResult : EngineApiResult {
  bool created = false;
  bool initial_version_created = false;
  bool plaintext_material_stored = false;
  bool protected_material_redacted = true;
  std::string protected_material_uuid;
  std::string active_version_uuid;
  std::uint64_t active_version_number = 0;
};

struct EngineAddProtectedMaterialVersionRequest : EngineApiRequest {
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string protected_reference;
  std::string envelope_reference;
  std::string payload_hash;
  std::string storage_class;
  std::string rotation_reason;
  EngineProtectedMaterialPolicySet policy_override;
};

struct EngineAddProtectedMaterialVersionResult : EngineApiResult {
  bool version_added = false;
  bool active_version_changed = false;
  bool plaintext_material_stored = false;
  bool protected_material_redacted = true;
  std::string protected_material_uuid;
  std::string active_version_uuid;
  std::uint64_t active_version_number = 0;
};

struct EngineResolveProtectedMaterialRequest : EngineApiRequest {
  std::string protected_material_uuid;
  std::string purpose;
};

struct EngineResolveProtectedMaterialResult : EngineApiResult {
  bool resolved = false;
  bool active_version_visible = false;
  bool protected_material_redacted = true;
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::uint64_t version_number = 0;
  std::string protected_material_ref;
};

struct EngineReleaseProtectedMaterialRequest : EngineApiRequest {
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string purpose;
};

struct EngineReleaseProtectedMaterialResult : EngineApiResult {
  bool released = false;
  bool policy_denied = false;
  bool redaction_applied = true;
  bool plaintext_material_returned = false;
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string release_handle;
  std::string audit_event_uuid;
};

struct EnginePurgeProtectedMaterialVersionRequest : EngineApiRequest {
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string purge_reason;
  std::string physical_erase_path;
  bool physical_erase_requested = false;
  bool physical_erase_authorized = false;
  bool physical_erase_retention_satisfied = false;
  bool physical_erase_legal_hold_clear = false;
};

struct EnginePurgeProtectedMaterialVersionResult : EngineApiResult {
  bool purged = false;
  bool refused_by_retention = false;
  bool audit_preserved = false;
  bool protected_reference_reachable = true;
  bool physical_erase_executed = false;
  bool physical_erase_verified = false;
  EngineApiU64 physical_erase_bytes = 0;
  std::string protected_material_uuid;
  std::string protected_material_version_uuid;
  std::string audit_event_uuid;
};

struct EngineInspectProtectedMaterialCatalogRequest : EngineApiRequest {
  std::string protected_material_uuid;
  bool include_versions = true;
  bool include_audit = true;
};

struct EngineInspectProtectedMaterialCatalogResult : EngineApiResult {
  std::vector<EngineProtectedMaterialCatalogEntry> materials;
  std::vector<EngineProtectedMaterialVersionCatalogEntry> versions;
  std::vector<EngineProtectedMaterialAuditEvent> audit_events;
  bool protected_material_redacted = true;
};

struct EngineExportProtectedMaterialPackageRequest : EngineApiRequest {
  std::string protected_material_uuid;
  bool include_versions = true;
  bool include_audit = false;
  bool include_purged_versions = false;
  std::string export_reason;
};

struct EngineExportProtectedMaterialPackageResult : EngineApiResult {
  bool exported = false;
  bool protected_material_redacted = true;
  bool plaintext_material_returned = false;
  std::string package_format = "scratchbird.protected_material.reference_package.v1";
  std::string package_digest;
  std::string encoded_package;
  std::string source_database_uuid;
  std::string protected_material_uuid;
  EngineApiU64 material_count = 0;
  EngineApiU64 version_count = 0;
  EngineApiU64 audit_event_count = 0;
};

struct EngineImportProtectedMaterialPackageRequest : EngineApiRequest {
  std::string encoded_package;
  std::string expected_package_digest;
  bool import_authorized = false;
  bool allow_uuid_conflict_replace = false;
  std::string import_reason;
};

struct EngineImportProtectedMaterialPackageResult : EngineApiResult {
  bool imported = false;
  bool protected_material_redacted = true;
  bool plaintext_material_returned = false;
  std::string package_format = "scratchbird.protected_material.reference_package.v1";
  std::string package_digest;
  EngineApiU64 material_count = 0;
  EngineApiU64 version_count = 0;
  EngineApiU64 audit_event_count = 0;
};

EngineAdmitEncryptionKeyResult EngineAdmitEncryptionKey(
    const EngineAdmitEncryptionKeyRequest& request);
EngineRotateEncryptionKeyResult EngineRotateEncryptionKey(
    const EngineRotateEncryptionKeyRequest& request);
EngineInspectProtectedMaterialCacheResult EngineInspectProtectedMaterialCache(
    const EngineInspectProtectedMaterialCacheRequest& request);
EnginePurgeProtectedMaterialResult EnginePurgeProtectedMaterial(
    const EnginePurgeProtectedMaterialRequest& request);
EngineShutdownProtectedMaterialResult EngineShutdownProtectedMaterial(
    const EngineShutdownProtectedMaterialRequest& request);
EngineOpenEncryptedFilespaceResult EngineOpenEncryptedFilespace(
    const EngineOpenEncryptedFilespaceRequest& request);
EngineRequestProtectedMaterialResult EngineRequestProtectedMaterial(
    const EngineRequestProtectedMaterialRequest& request);
EngineCreateProtectedMaterialResult EngineCreateProtectedMaterial(
    const EngineCreateProtectedMaterialRequest& request);
EngineAddProtectedMaterialVersionResult EngineAddProtectedMaterialVersion(
    const EngineAddProtectedMaterialVersionRequest& request);
EngineResolveProtectedMaterialResult EngineResolveProtectedMaterial(
    const EngineResolveProtectedMaterialRequest& request);
EngineReleaseProtectedMaterialResult EngineReleaseProtectedMaterial(
    const EngineReleaseProtectedMaterialRequest& request);
EnginePurgeProtectedMaterialVersionResult EnginePurgeProtectedMaterialVersion(
    const EnginePurgeProtectedMaterialVersionRequest& request);
EngineInspectProtectedMaterialCatalogResult EngineInspectProtectedMaterialCatalog(
    const EngineInspectProtectedMaterialCatalogRequest& request);
EngineExportProtectedMaterialPackageResult EngineExportProtectedMaterialPackage(
    const EngineExportProtectedMaterialPackageRequest& request);
EngineImportProtectedMaterialPackageResult EngineImportProtectedMaterialPackage(
    const EngineImportProtectedMaterialPackageRequest& request);
std::string RedactProtectedMaterialForDiagnostics(std::string text);

}  // namespace scratchbird::engine::internal_api
