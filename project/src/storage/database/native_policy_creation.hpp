// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_identifier_lookup_key.hpp"
#include "catalog_named_creation.hpp"
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::storage::database {
enum class NativeInitialPolicyError {
  none, invalid_request, source_invalid, invalid_definition, invalid_names,
  duplicate_identity, resource_exhausted
};
// One object's placement and human-facing label, keyed by the object identity
// the assembler enumerated.
//
// A catalog object has no name of its own: it is identified by its UUID, and
// the label a person sees is a row in the name registry, in language-specific
// versions, pointing back at that UUID. This record is the caller's half of
// that registry row for one object.
//
// `owning_schema_uuid` is PER OBJECT, not shared, because the system catalog is
// a recursive schema tree (`core/catalog/bootstrap_schema_roots.hpp` declares
// sys, sys.catalog, sys.security, sys.configuration and twenty more paths). The
// authoritative name index is keyed on the scope, so the same label in the same
// language resolves distinctly under two different parent schemas, and objects
// of different families belong under different parents. One shared schema for
// a whole cohort would flatten that tree and force labels to be disambiguated
// that the tree already distinguishes.
//
// Label text is never derived from the source pack. Lookup keys are not asked
// for: the engine's own name-to-UUID process derives both lanes from the
// identifier resolution profile supplied below, and verifies them.
struct NativeInitialDefinitionName {
  core::platform::Uuid object_uuid;
  core::platform::Uuid owning_schema_uuid;
  std::string_view name_text;
};
// Supplying this inverts the definitions contract. Without it the caller must
// reproduce the assembler's identity, kind and subtype assignment for every
// object and hand back a complete metadata envelope per object, which means
// duplicating roughly fifteen construction decisions and keeping them in
// lockstep forever. With it the assembler generates every metadata envelope and
// both name halves from the enumeration it already builds, and the caller
// supplies only what is genuinely its own: these shared identities, one name per
// object, and the fresh identities each object consumes.
//
// Obtain the object identities to key `names` from
// EnumerateNativeInitialPolicyDefinitions / ...SecurityDefinitions, which
// returns the same enumeration this path generates from. Predicting it is not
// required and not supported.
//
// DELIBERATE NARROWING, not a discovered constraint: all generated objects
// share one owner, one audit object and one security policy. The
// caller-supplied path admits a different owner or policy per object. The
// shared security policy is load-bearing for the service runtime authority
// binding, whose payload policy_uuid must equal its metadata security policy,
// so this path refuses a security policy that is not the local service policy.
//
// Schema placement is NOT shared: it is per object, in `names`, because the
// system catalog is a recursive schema tree and the name index is scoped by it.
struct NativeInitialGeneratedDefinitions {
  core::platform::Uuid owner_principal_uuid;
  core::platform::Uuid audit_object_uuid;
  core::platform::Uuid security_policy_uuid;
  // Required by the two name schemas. No create-path source exists for these
  // yet; they are the caller's to issue.
  core::platform::Uuid name_collision_policy_uuid;
  core::platform::Uuid dialect_profile_uuid;
  // Borrowed and caller-owned; must outlive the call, along with its
  // profile_name backing. Supplied as the profile rather than its identity
  // because both lookup lanes and the display form are derived from it by
  // DeriveCatalogIdentifierLookupKeysInto, and the entry's
  // identifier_profile_uuid is taken from the profile itself so the two cannot
  // disagree. Collision handling and lookup remain the name registry's.
  const core::catalog::CatalogIdentifierResolutionProfile* identifier_profile=nullptr;
  // Exactly five per enumerated object, consumed in enumeration order: the
  // definition row, the name-vector object and row, the name-entry object and
  // row. Exact rather than at-least, so an over-supplied pool cannot hide a
  // miscount. Identity issuance stays with the caller; nothing is minted here.
  std::span<const core::platform::Uuid> fresh_identities;
  // Exactly one per enumerated object, carrying that object's schema placement
  // and label. A missing or extra entry is a refusal.
  std::span<const NativeInitialDefinitionName> names;
  // Carried by every generated metadata version. Non-empty printable keys: the
  // metadata codec refuses an envelope without them, so there is no usable
  // default to fall back on and none is invented here.
  std::string_view trace_search_key;
  std::string_view retention_class;
};
struct NativeInitialPolicyRequest {
  std::string_view policy_seed_pack_root;
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  // Actual owner-issued identity of the materialized memory configuration.
  core::platform::Uuid memory_configuration_uuid;
  std::string_view database_default_language;
  // Exact complete metadata/names, with empty definition payloads. The source
  // pack supplies identities/typed values, never owner/audit/name authority.
  std::span<const core::catalog::CatalogNamedCreationInput> definitions;
  core::catalog::CatalogNamedCreationLimits limits;
  // When supplied, `definitions` must be empty and the assembler generates
  // every definition envelope and name half itself. When absent, `definitions`
  // must be complete, as above. Appended last: existing callers initialize this
  // aggregate positionally.
  std::optional<NativeInitialGeneratedDefinitions> generated_definitions=std::nullopt;
};
struct NativeInitialPolicyResult {
  NativeInitialPolicyError error=NativeInitialPolicyError::invalid_request;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::optional<core::catalog::CatalogNamedCreationSet> records;
  bool ok() const noexcept {return error==NativeInitialPolicyError::none&&records.has_value();}
};
// Owning creation-plan producer for all imported pack/provider/profile/memory
// definitions and their actual supplied aliases. Consumes exact hash-admitted
// source bytes and the same typed translators as production creation, not
// legacy catalog rows or a prototype schema cache. No physical I/O except pack
// intake; no partial package on refusal. Caller retains actual source/input/
// output/scratch memory and admits the referenced namespaces/governance/profiles.
// These limits are not grants. This is not the complete node/security/grant
// cohort, authentication, activation or serving success. Never enter under a
// storage guard: source intake and owning result assembly can allocate.
NativeInitialPolicyResult AssembleNativeInitialPolicyRecords(const NativeInitialPolicyRequest&) noexcept;

// Imported group-to-role membership declarations have no source object UUID.
// The actual construction owner supplies newly issued object identities and
// binds each to an exact binary source pair. No synthesized/hash identity and
// no caller-selected replacement for a source role/group/grant is admitted.
struct NativeInitialMembershipIdentity {
  core::platform::Uuid membership_uuid,member_uuid,parent_uuid;
};
struct NativeInitialBootstrapIdentity {
  core::platform::Uuid principal_uuid,membership_uuid;
  std::string_view principal_name,credential_fingerprint;
};
// Explicit default LOCAL startup enrollment, not a credential source or an
// authentication/authorization receipt. Grant identities, in order, name the
// actual CONNECT, CATALOG_MUTATE, OBS_AGENT_STATE_READ, OBS_AGENT_EVIDENCE_READ
// and OBS_AGENT_CONTROL declarations. They are node-scoped, not SYSARCH or an
// implicit role membership. Selected provider/policy must occur in the same
// admitted source snapshot as enabled local-password / deny-by-default.
struct NativeInitialLocalServiceIdentity {
  core::platform::Uuid principal_uuid,runtime_binding_uuid,credential_reference_uuid;
  core::platform::Uuid provider_uuid,policy_uuid;
  std::array<core::platform::Uuid,5> grant_uuids;
  std::string_view principal_name,credential_fingerprint;
  // L5 is a member of SYSARCH (D5, as legacy create writes it). Appended last:
  // callers initialize this aggregate positionally. Required with a service.
  core::platform::Uuid l5_membership_uuid{};
};
// SEARCH_KEY: SB_NATIVE_CREATE_AGENT_PRINCIPALS (owner 2026-10-29/30: create lays
// down every agent role and right from the policy files; rights not granted =
// CREATE FAILED). One per agent type the pack declares, in pack order: the
// principal (no credential; L5 binds sessions to it by UUID), one grant per
// right the pack's authority entry declares, in that order, and its task-group
// role membership. policy_uuid/link_uuid name the agent type's committed
// primary policy and the link record; both nil when the caller has no agent
// policy for the type. Identities are the caller's to issue.
struct NativeInitialAgentIdentity {
  std::string agent_type_id;
  core::platform::Uuid principal_uuid,membership_uuid;
  std::vector<core::platform::Uuid> grant_uuids;
  core::platform::Uuid policy_uuid{},link_uuid{};
};
struct NativeInitialAgentIdentitiesResult {
  NativeInitialPolicyError error=NativeInitialPolicyError::invalid_request;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::vector<NativeInitialAgentIdentity> agents;
  bool ok() const noexcept {return error==NativeInitialPolicyError::none;}
};
// Reads the pack's agent types and authority and issues every identity from
// `issue` (principal, membership, then one per right), so a caller cannot
// predict the shape wrongly. Policy and link stay nil for the caller to set.
NativeInitialAgentIdentitiesResult IssueNativeInitialAgentIdentities(
    std::string_view policy_seed_pack_root,
    const std::function<std::optional<core::platform::Uuid>()>& issue) noexcept;
// SEARCH_KEY: SB_NATIVE_POLICY_FAMILY_SEEDS (owner direction 2026-11-04). The
// identities of the five native policy families the pack declares
// (native_policy_families.hpp), one per seed in seed order. A descriptor has
// its policy and policy-version identities; a collision policy has a
// descriptor and a body. The two references name objects that already exist:
// the primary filespace (evidence storage "primary") and the sys.security
// schema (authorization schemas). Identities are the caller's to issue.
struct NativePolicyFamilyDescriptorIdentity {
  core::platform::Uuid policy_uuid,version_uuid;
};
struct NativeInitialPolicyFamilyIdentities {
  std::vector<NativePolicyFamilyDescriptorIdentity> verification_descriptors,transaction_descriptors,collision_descriptors;
  std::vector<core::platform::Uuid> verification_bodies,evidence_bodies,authorization_bodies;
  std::vector<core::platform::Uuid> transaction_bodies,lock_bodies,isolation_profiles;
  std::vector<core::platform::Uuid> management_profiles,management_descriptors;
  std::vector<core::platform::Uuid> identifier_profiles,dialect_profiles,collision_groups,collision_bodies;
  core::platform::Uuid primary_filespace_uuid{},security_schema_uuid{};
};
struct NativeInitialPolicyFamilyIdentitiesResult {
  NativeInitialPolicyError error=NativeInitialPolicyError::invalid_request;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::optional<NativeInitialPolicyFamilyIdentities> identities;
  bool ok() const noexcept {return error==NativeInitialPolicyError::none&&identities.has_value();}
};
// Reads the pack's native policy families and issues every identity from
// `issue` in the order the fields above are declared. Refuses a pack that
// declares none. The two references stay nil for the caller to set.
NativeInitialPolicyFamilyIdentitiesResult IssueNativeInitialPolicyFamilyIdentities(
    std::string_view policy_seed_pack_root,
    const std::function<std::optional<core::platform::Uuid>()>& issue) noexcept;
struct NativeInitialPolicySecurityRequest {
  NativeInitialPolicyRequest policy;
  std::span<const NativeInitialMembershipIdentity> group_memberships;
  // Explicit credentialed bootstrap enrollment, never a service identity or
  // authentication proof. When supplied, complete metadata/names must also
  // cover the canonical SYSARCH role, this principal and its membership.
  std::optional<NativeInitialBootstrapIdentity> bootstrap=std::nullopt;
  // Requires the distinct explicit bootstrap owner and all seven additional
  // complete metadata/name inputs. No protected secret is accepted or stored;
  // actual credential-source custody and release admission remain separate.
  std::optional<NativeInitialLocalServiceIdentity> local_service=std::nullopt;
  // Required, complete, with a local service whenever the pack declares agent
  // types (SB_NATIVE_CREATE_AGENT_PRINCIPALS); refused without one.
  std::span<const NativeInitialAgentIdentity> agents{};
  // SB_NATIVE_POLICY_FAMILY_SEEDS. When supplied, the pack's five native
  // policy families are assembled from it: their records, the transaction
  // profile's binding to its descriptor, and agreement with any generated
  // definitions' naming identities. Optional here so a caller that builds
  // these families itself (the enrollment fixture's fault variants) still
  // can. The production native create producer (not yet written, M1 step 2c)
  // is to require it. Requires a service.
  std::optional<NativeInitialPolicyFamilyIdentities> policy_families=std::nullopt;
};
// Complete imported policy AND security declaration cohort from one admitted
// source snapshot, with all complete caller metadata/names. Bootstrap is
// enrolled only from explicit validated inputs. Default local service records
// likewise require explicit identities/verifier/provider/policy/grants/binding;
// no credential, authentication, effective grant or activation is inferred.
// Physical publication and native reference closure remain owned
// by the enclosing actual node constructor.
NativeInitialPolicyResult AssembleNativeInitialPolicySecurityRecords(
    const NativeInitialPolicySecurityRequest&) noexcept;

// One enumerated object: the identity, kind and subtype the assembler assigns
// it from the admitted source snapshot plus the supplied bootstrap/service
// identities. This is the same enumeration the generating path builds its
// envelopes from, exposed so a caller can key its name authority and size its
// identity supply without reproducing any of that assignment.
struct NativeInitialDefinitionIdentity {
  core::platform::Uuid object_uuid;
  core::catalog::CatalogRecordKind kind=core::catalog::CatalogRecordKind::config_profile;
  std::string object_subtype;
};
struct NativeInitialDefinitionEnumerationResult {
  NativeInitialPolicyError error=NativeInitialPolicyError::invalid_request;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  // Empty on any refusal: no partial enumeration is reported.
  std::vector<NativeInitialDefinitionIdentity> definitions;
  bool ok() const noexcept {return error==NativeInitialPolicyError::none&&!definitions.empty();}
};
// Enumeration only. Reads the admitted source snapshot and runs the same
// identity/alias/source admission as assembly, then stops: no metadata, no
// names, no records and no placement. Neither `definitions` nor
// `generated_definitions` is consulted, so this is callable before either
// exists. Physical intake can allocate; never enter under a storage guard.
NativeInitialDefinitionEnumerationResult EnumerateNativeInitialPolicyDefinitions(
    const NativeInitialPolicyRequest&) noexcept;
NativeInitialDefinitionEnumerationResult EnumerateNativeInitialPolicySecurityDefinitions(
    const NativeInitialPolicySecurityRequest&) noexcept;
} // namespace scratchbird::storage::database
