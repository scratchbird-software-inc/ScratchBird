// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_workspace.hpp"
#include "native_database_creation.hpp"
#include "native_policy_creation.hpp"
#include "catalog_schema_creation.hpp"
#include "catalog_language.hpp"
#include "catalog_identifier_profile.hpp"
#include "catalog_naming_collision_policy.hpp"
#include <optional>
#include <span>
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
  name_path,  // SB_NATIVE_NAME_PATH: a name entry's path does not resolve
  // SB_NATIVE_CREATE_NAME_COLLISION (owner Q178 = 1, 2026-11-14): two name
  // entries the plan would write hold the same name under the pack's
  // collision policy; NativeDatabaseCreateResult::name_collision says whose.
  name_collision
};
// SEARCH_KEY: SB_NATIVE_CREATE_NAME_COLLISION (owner Q178 = 1, 2026-11-14).
// Whose name the plan's own collision check found taken (the second line
// behind the pre-plan principal check, Q176): the caller's input first, in
// this order, else a pack or engine name. Never the name text.
enum class NativeCreateNameCollisionClass {
  none,database_name,bootstrap_principal_name,service_principal_name,built_in_name
};
const char* NativeCreateNameCollisionClassName(NativeCreateNameCollisionClass) noexcept;
// The plan's check itself, exposed for tests: among `entries` (active name
// entries one collision group binds, under its lanes), the first two of
// different objects that hold the same name by SB_NAME_COLLISION_ORDER
// (CompareCatalogNameCollisionScope, then CompareCatalogNameCollisionLanes),
// as indices into `entries`; nullopt when none do. Entries of one object
// never collide with each other. SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1):
// `own_lanes[i]` is entries[i]'s own-lane key derived under its identifier
// profile (DeriveCatalogNameEntryKeys(...).own); the sizes must be equal
// (nullopt otherwise is not a collision: the caller derives every lane).
// (Was: the entries' stored keys.)
std::optional<std::pair<std::size_t,std::size_t>> FindNativeCreateNameEntryCollision(
    std::span<const core::catalog::CatalogNameEntryView> entries,
    std::span<const core::catalog::CatalogNameDerivedText> own_lanes,
    core::catalog::CatalogCollisionLanguageLane language_lane,core::catalog::CatalogCollisionKeyLane key_lane);
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
// SEARCH_KEY: SB_NATIVE_CREATE_PRINCIPAL_NAME_COLLISION (owner Q152 = 1,
// Q153 = 1, 2026-11-14). Native CREATE DATABASE refuses, before the plan, a
// caller principal name that would hold the same name as another principal
// the create writes, under the pack's naming collision policy and the
// identifier profile the principals are named under. The class of collision
// only, never the name text (the pack's agent names are names too).
//   bootstrap_is_service_principal  bootstrap_principal_name: vs
//                                   internal_service_principal_name: (Q153)
//   bootstrap_is_agent_principal    bootstrap_principal_name: vs an agent
//                                   principal (the pack's agent_type_id) (Q152)
//   service_is_agent_principal      internal_service_principal_name: vs an
//                                   agent principal (same class of caller
//                                   input; refused the same way)
//   pack_unusable                   the pack's naming family could not be
//                                   turned into the profile/policy the plan
//                                   would use (the plan refuses such a pack)
// Update 2026-11-14 (owner Q176 = 1): every sys.security name the create
// writes, not only the principals: the pack's roles (roles.json role_code,
// and the engine's own ROLE_SYSARCH) and groups (groups.json group_code),
// each named as the plan names it (NativeSecurityReferenceName over the
// record the plan builds). Four classes join, appended (the values above
// keep their numbers):
//   bootstrap_is_role / bootstrap_is_group
//   service_is_role   / service_is_group
// The other sys.security names (grants, policies, the provider and
// configuration records) are their UUID text; a principal name must start
// with a letter or '_' (IsSafeBootstrapPrincipalNameCharacters), and the text
// of every UUID the default pack declares and the engine issues today starts
// with a digit (a UUIDv7's leading timestamp; the pack's 018f7a10-...), so
// none of those can collide with one. A pack declaring a UUID whose text
// starts with a letter is not covered here: the plan's own check
// (SB_NATIVE_CREATE_NAME_COLLISION) is the backstop for every name.
// (Was: principals only; a bootstrap principal named DBA, the pack's group,
// created.)
enum class NativeCreatePrincipalNameCollision {
  none,bootstrap_is_service_principal,bootstrap_is_agent_principal,service_is_agent_principal,pack_unusable,
  bootstrap_is_role,bootstrap_is_group,service_is_role,service_is_group
};
const char* NativeCreatePrincipalNameCollisionName(NativeCreatePrincipalNameCollision) noexcept;
// The comparison itself, over an admitted identifier profile and the pack's
// collision group: each principal name gets the name entry the plan would
// write for it (user_account, one scope, `profile`, `language`, unquoted,
// requires_exact_match false, its own lane from DeriveCatalogNameEntryKeys;
// was DeriveCatalogIdentifierLookupKeysInto into a backing)
// and two entries collide exactly when SB_NAME_COLLISION_ORDER
// (CompareCatalogNameCollisionScope / CompareCatalogNameCollisionLanes) says
// so. `user_accounts_bound` is whether the policy body binds user_account to
// `group` at all (false: nothing collides). A name whose keys cannot be
// derived (text or length rule) collides with nothing here; Q133 or the plan
// refuses it. Exposed for tests. (Was: "a profile that folds case can only
// be built by hand today (the pack carries no fold mode)"; since Q177 the
// default pack's profile folds unquoted names to upper ASCII.)
// Q176: role_names and group_names are the role and group names the create
// writes in the same scope (empty by default: the Q152 callers).
NativeCreatePrincipalNameCollision FindNativeCreatePrincipalNameCollision(
    const core::catalog::CatalogIdentifierResolutionProfile& profile,
    core::catalog::CatalogCollisionLanguageLane language_lane,core::catalog::CatalogCollisionKeyLane key_lane,
    bool user_accounts_bound,const core::platform::Uuid& language,std::string_view bootstrap_principal_name,
    std::string_view service_principal_name,std::span<const std::string> agent_principal_names,
    std::span<const std::string> role_names={},std::span<const std::string> group_names={}) noexcept;
// The same, from the server's pack, as the plan reads it: the agent principal
// names are the pack's agent_type_id values, the profile is the one the
// pack's first dialect profile names (built by the same seed conversion the
// plan uses), the policy is the pack's (one) collision policy and its group.
// Q176: the role and group names are the pack's roles and groups and the
// engine's ROLE_SYSARCH, named by the record builders the plan uses.
NativeCreatePrincipalNameCollision CheckNativeCreatePrincipalNameCollisions(
    std::string_view policy_seed_pack_root,const core::platform::Uuid& language,
    std::string_view bootstrap_principal_name,std::string_view service_principal_name) noexcept;
struct NativeDatabaseCreateResult {
  NativeDatabaseCreateError error=NativeDatabaseCreateError::invalid_request;
  // Q130: set (with the stage's error) when a name was over the profile maximum.
  NativeCreateNameClass name_over_profile_maximum=NativeCreateNameClass::none;
  // Q178 (SB_NATIVE_CREATE_NAME_COLLISION): with error name_collision, whose name.
  NativeCreateNameCollisionClass name_collision=NativeCreateNameCollisionClass::none;
  NativeInitialPolicyError policy_error=NativeInitialPolicyError::none;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  NativeInitialDatabaseError database_error=NativeInitialDatabaseError::none;
  core::catalog::CatalogInitialSchemaError schema_error=core::catalog::CatalogInitialSchemaError::none;
  NativeCreationWorkspaceError workspace_error=NativeCreationWorkspaceError::none;
  // Q140 (SB_NATIVE_CREATE_PARTIAL_FILE_UNLINK): how far the write got, from
  // WriteNativeDatabaseCreatePlan; unknown for a plan and elsewhere.
  NativeCreationWriteProgress write_progress=NativeCreationWriteProgress::unknown;
  // Q144 (SB_NATIVE_CREATE_CAPACITY_BOUND): with capacity_exceeded,
  // resource_exhausted or workspace_failure, the bound the plan met (none if
  // the refusal was not a capacity bound).
  NativeCreationCapacityBound capacity_bound=NativeCreationCapacityBound::none;
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
// Update 2026-11-14 (owner Q134 = 1, SB_NATIVE_CREATE_WRITE_CONSUMES_PLAN):
// the plan is no longer discarded. PlanNativeDatabaseCreate runs the WHOLE
// assembly, the workspace's included (its capacity, budget and layout checks,
// storage identities and every encoded page image), and returns it as a
// NativeDatabaseCreatePlan. Every refusal assembly can produce is produced
// here, before any device exists. WriteNativeDatabaseCreatePlan consumes the
// plan: device checks (owned native data file, open, writable, empty), the
// writes, sync, readback and page zero last; it assembles nothing, issues no
// identity and needs no issuer. The issuer's identities are the ones the file
// carries, so native CREATE DATABASE uses ONE issuer, bound to the database
// it creates. CreateNativeDatabaseOnOpenDevice is plan + write.
// (Was: the identities were spent and discarded, a separate issuer was used,
// and the create assembled everything a second time after the open.)
struct NativeDatabaseCreatePlan {
  // The receipt identities (workspace receipt filled by the write).
  std::optional<NativeDatabaseCreateReceipt> receipt;
  NativeCreationWorkspacePlan workspace;
  bool ready() const noexcept {return receipt.has_value()&&workspace.ready();}
};
struct NativeDatabaseCreatePlanResult:NativeDatabaseCreateResult {
  NativeDatabaseCreatePlan plan;  // ready() exactly when error == none
  bool ok() const noexcept {return error==NativeDatabaseCreateError::none&&plan.ready();}
};
NativeDatabaseCreatePlanResult PlanNativeDatabaseCreate(
    const NativeDatabaseCreateRequest&,core::uuid::StandaloneUuidV7Issuer&) noexcept;
// Consumes `plan` (moved from, whatever the outcome). `empty_owned` as for
// CreateNativeDatabaseOnOpenDevice. The only refusals are the device's
// (workspace_failure with invalid_device / device_not_empty / io_failure /
// readback_mismatch / graph_failure ...), or invalid_request for a plan that
// is not ready.
NativeDatabaseCreateResult WriteNativeDatabaseCreatePlan(
    disk::FileDevice& empty_owned,NativeDatabaseCreatePlan&& plan) noexcept;
// Opens a new, empty native database file for CreateNativeDatabaseOnOpenDevice:
// O_CREAT|O_EXCL, then flock(LOCK_EX|LOCK_NB) on the new descriptor before any
// write, and no lock file. If another opener flocks the new file between the
// create and our flock, this refuses with SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD
// and leaves the (empty) file to its holder: it unlinks nothing.
inline disk::IoResult OpenNewNativeDatabaseFile(disk::FileDevice& device,std::string path) {
  return device.Open(std::move(path),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file);
}
} // namespace scratchbird::storage::database
