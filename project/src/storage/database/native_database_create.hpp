// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_workspace.hpp"
#include "native_database_creation.hpp"
#include "native_policy_creation.hpp"
#include "catalog_schema_creation.hpp"
#include "catalog_language.hpp"
#include <optional>
#include <string>
#include <utility>
#include <string_view>

namespace scratchbird::storage::database {
// SEARCH_KEY: SB_NATIVE_DATABASE_CREATE_PRODUCER (M1 step 2c). Owner direction
// 2026-11-03/04: everything in the one database file; CREATE produces a
// native-format file; the L5 service credential is required; the default
// language is "en"; one shared name/uuid catalog table.
// S2 (2026-11-13): the request carries the language as a catalog_language
// constant UUID, chosen by the caller ("en" above was the default the owner
// named, not one this producer applies).
//
// The production composition of a complete initial native database on an
// empty, exclusively owned primary device. The caller's issuer is bound to the
// database being created: its binding supplies the database and
// policy-snapshot identities, and every other identity is a UUIDv7 it issues,
// except the values that are the same in every database (the policy pack's own
// identities, the canonical SYSARCH role and the native catalog constants). The
// request is data only, as CREATE DATABASE carries it: the database name is
// data, never an identifier.
//
// It writes, in one construction:
//   - the database and primary filespace definitions;
//   - the system schema tree (bootstrap_schema_roots.hpp);
//   - the policy pack's security and policy cohort, including the native
//     policy families, the bootstrap principal, the L5 service principal and
//     every agent type's principal, grants, membership and policy link;
//   - the default agent bootstrap declarations;
//   - the catalog index profile and dependency-map rows, and the indexes;
//   - the construction-intent audit record.
// Names: an object addressed by name keeps its name; any other object's
// reference name is its UUID text (owner 2026-11-04).
//
// Every refusal happens before the first write, including the check that the
// complete cohort fits the bounds the hosted reader opens it with. This is
// construction only: the file remains initializing, and opening, serving and
// activation are separate. Not yet content parity with legacy create (no
// resource seed, datatype descriptors, domains, metric catalog or system
// tables). Never call under a device guard.
// Update 2026-11-05 (SB_NATIVE_BUILTIN_DATATYPE_DOMAIN_ROWS): the 85 built-in
// datatype descriptors and 85 system domains are now written. Still missing:
// the resource seed, metric catalog and system tables.
struct NativeDatabaseCreateRequest {
  std::string_view policy_seed_pack_root;
  std::string_view database_name;
  // S2 (owner Q92 = 1, 2026-11-13): the language is the catalog_language
  // constant (SB_CATALOG_LANGUAGE_CONSTANTS), 16 binary bytes, never a text
  // tag; a UUID that is no language constant is refused (invalid_request),
  // never defaulted. Was a text tag turned into the constant here.
  core::platform::Uuid database_default_language;
  core::platform::Uuid page_size_profile_uuid;
  core::platform::u32 page_size_bytes=0;
  core::platform::u64 total_pages=0;
  // An admitted positive 48-bit UTC millisecond observation.
  core::platform::u64 creation_utc_millis=0;
  // The constructing engine instance (construction-intent provenance).
  core::platform::Uuid engine_uuid;
  // Exact independently admitted datatype cohort; never defaulted here.
  core::catalog::CatalogDatabaseDatatypeCohort datatype_cohort;
  std::string_view bootstrap_principal_name,bootstrap_credential_fingerprint;
  std::string_view service_principal_name,service_credential_fingerprint;
  core::platform::u64 maximum_retained_image_bytes=0;
};
enum class NativeDatabaseCreateError {
  none,invalid_request,invalid_principal,identity_failure,policy_assembly,database_assembly,
  schema_assembly,agent_assembly,audit_failure,index_failure,capacity_exceeded,workspace_failure,
  resource_exhausted,datatype_assembly,
  name_path  // SB_NATIVE_NAME_PATH: a name entry's path does not resolve
};
struct NativeDatabaseCreateReceipt {
  // SB_ONE_DATABASE_UUID (owner Q111 = 1): database_uuid is the page-zero
  // identity AND the database catalog object's key, the schema-tree root and
  // the name scope; there is no separate database_object_uuid any more.
  core::platform::Uuid database_uuid,filespace_uuid;
  transaction::mga::TransactionIdentity construction_transaction;
  core::platform::Uuid bootstrap_principal_uuid,service_principal_uuid,service_credential_reference_uuid;
  core::platform::Uuid service_provider_uuid,service_policy_uuid,service_runtime_binding_uuid;
  NativeCreationWorkspaceReceipt workspace;
};
// SEARCH_KEY: SB_NATIVE_CREATE_NAMES_UNDER_PACK_PROFILE (owner Q130 = 2,
// 2026-11-14). The kind of object whose name was longer than the identifier
// profile it is named under allows (the profile's maximum_identifier_length,
// from the server's pack). Never the name text itself.
enum class NativeCreateNameClass {
  none,
  policy_cohort,        // a pack or engine name in the security/policy cohort
  bootstrap_principal,  // the caller's bootstrap principal name
  service_principal,    // the caller's internal service principal name
  database,filespace,schema,agent,datatype,index
};
const char* NativeCreateNameClassName(NativeCreateNameClass) noexcept;
struct NativeDatabaseCreateResult {
  NativeDatabaseCreateError error=NativeDatabaseCreateError::invalid_request;
  // Q130: set (with the stage's error) when a name was over the profile maximum.
  NativeCreateNameClass name_over_profile_maximum=NativeCreateNameClass::none;
  NativeInitialPolicyError policy_error=NativeInitialPolicyError::none;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  NativeInitialDatabaseError database_error=NativeInitialDatabaseError::none;
  core::catalog::CatalogInitialSchemaError schema_error=core::catalog::CatalogInitialSchemaError::none;
  NativeCreationWorkspaceError workspace_error=NativeCreationWorkspaceError::none;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::optional<NativeDatabaseCreateReceipt> receipt;
  bool ok() const noexcept {return error==NativeDatabaseCreateError::none&&receipt.has_value();}
};
// SEARCH_KEY: SB_NATIVE_DATA_FILE_OWNERSHIP (owner 2026-11-13, slice step S1)
// `empty_owned` must come from OpenNewNativeDatabaseFile (or any
// FileDevice::Open(..., DatabaseFileOwnership::native_data_file)): the native
// creation entry points refuse any other device (invalid_device), so a native
// file is never created beside an .sb.owner.lock, not even transiently.
NativeDatabaseCreateResult CreateNativeDatabaseOnOpenDevice(
    disk::FileDevice& empty_owned,const NativeDatabaseCreateRequest&,core::uuid::StandaloneUuidV7Issuer&) noexcept;
// SEARCH_KEY: SB_NATIVE_CREATE_NAMES_UNDER_PACK_PROFILE (owner Q130 = 2,
// 2026-11-14). The same body as CreateNativeDatabaseOnOpenDevice, run with no
// device: every step before the first write (pack intake, identity issue,
// assembly of every name the create would write, layout and capacity checks),
// then it stops. It opens and writes no database file; the issuer's
// identities are spent and discarded. error == none (with no receipt) means
// the create's assembly would pass; any refusal carries the same error, and
// name_over_profile_maximum says which kind of name was too long for the
// pack's identifier profile. Native CREATE DATABASE calls it before its
// O_EXCL open, with a separate issuer bound like the create's.
NativeDatabaseCreateResult PlanNativeDatabaseCreate(
    const NativeDatabaseCreateRequest&,core::uuid::StandaloneUuidV7Issuer&) noexcept;
// Opens a new, empty native database file for CreateNativeDatabaseOnOpenDevice:
// O_CREAT|O_EXCL, then flock(LOCK_EX|LOCK_NB) on the new descriptor before any
// write, and no lock file. If another opener flocks the new file between the
// create and our flock, this refuses with SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD
// and leaves the (empty) file to its holder: it unlinks nothing.
inline disk::IoResult OpenNewNativeDatabaseFile(disk::FileDevice& device,std::string path) {
  return device.Open(std::move(path),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file);
}
} // namespace scratchbird::storage::database
