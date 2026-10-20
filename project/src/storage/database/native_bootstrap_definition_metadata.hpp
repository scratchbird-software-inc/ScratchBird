// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_named_creation.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "uuid.hpp"

#include <array>

namespace scratchbird::storage::database {

// Builds the CatalogMetadataVersion envelope that
// NativeInitialPolicySecurityRequest::definitions requires, one object at a
// time. This is the repeated unit of that definitions vector: the enumeration
// of which objects exist is separate and is reported by
// EnumerateNativeInitialPolicy{,Security}Definitions.
//
// The envelope alone is not a submittable input. BuildBootstrapNamedCreationUnit
// at the foot of this file produces the whole unit -- this envelope plus the two
// name halves -- and is what the assembler's generating path calls.
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
  // Distinct from object_uuid and globally unique across the whole request.
  // AssembleNativeInitialPolicyRecordsImpl collects object_uuid AND row_uuid
  // from every definition, every name-vector metadata and every name-entry
  // metadata, sorts them, and refuses on any duplicate (:7102-7112). A missing
  // row_uuid is therefore not a cosmetic omission: every definition would carry
  // nil and the request would be refused as duplicate_identity.
  core::platform::Uuid row_uuid;
  // The two name identities this object's name half will carry.
  // PreflightLocalNamedCreationRecord reads them off the DEFINITION metadata
  // and requires the supplied name vector to agree with both
  // (catalog_named_creation.cpp: v.name_vector_uuid == d.name_vector_uuid and
  // v.default_name_entry_uuid == d.default_name_uuid). They are therefore part
  // of a complete definition envelope, not part of the name half: an envelope
  // without them encodes cleanly and is refused as invalid_names later.
  core::platform::Uuid name_vector_uuid;
  core::platform::Uuid default_name_entry_uuid;
  core::catalog::CatalogRecordKind kind = core::catalog::CatalogRecordKind::config_profile;
  // Must equal the subtype the assembler registered for this object, or the
  // definition is refused. Security records use CatalogRecordKindName(kind).
  std::string object_subtype;

  // ValidateCatalogMetadataVersionView requires all three text keys to be
  // non-empty printable keys, so an envelope without them cannot be encoded at
  // all: CatalogMetadataVersionEncodedBytes returns nothing and the named
  // creation preflight refuses it as invalid_metadata. They are classification,
  // not identity, and are not guessed here.
  std::string trace_search_key;
  std::string retention_class;

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
  row_identity_invalid,
  name_vector_identity_invalid,
  name_entry_identity_invalid,
  owner_identity_invalid,
  audit_identity_invalid,
  policy_identity_invalid,
  schema_identity_invalid,
  transaction_identity_invalid,
  subtype_empty,
  subtype_invalid,
  trace_search_key_invalid,
  retention_class_invalid,
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
    case E::row_identity_invalid: return "CATALOG.DEFINITION_ROW_IDENTITY_INVALID";
    case E::name_vector_identity_invalid: return "CATALOG.DEFINITION_NAME_VECTOR_IDENTITY_INVALID";
    case E::name_entry_identity_invalid: return "CATALOG.DEFINITION_NAME_ENTRY_IDENTITY_INVALID";
    case E::owner_identity_invalid: return "CATALOG.DEFINITION_OWNER_IDENTITY_INVALID";
    case E::audit_identity_invalid: return "CATALOG.DEFINITION_AUDIT_IDENTITY_INVALID";
    case E::policy_identity_invalid: return "CATALOG.DEFINITION_POLICY_IDENTITY_INVALID";
    case E::schema_identity_invalid: return "CATALOG.DEFINITION_SCHEMA_IDENTITY_INVALID";
    case E::transaction_identity_invalid: return "CATALOG.DEFINITION_TRANSACTION_IDENTITY_INVALID";
    case E::subtype_empty: return "CATALOG.DEFINITION_SUBTYPE_EMPTY";
    case E::subtype_invalid: return "CATALOG.DEFINITION_SUBTYPE_INVALID";
    case E::trace_search_key_invalid: return "CATALOG.DEFINITION_TRACE_SEARCH_KEY_INVALID";
    case E::retention_class_invalid: return "CATALOG.DEFINITION_RETENTION_CLASS_INVALID";
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
  if (!IsEngineIdentityUuid(input.row_uuid) ||
      input.row_uuid == input.object_uuid) {
    out.error = E::row_identity_invalid;
    return out;
  }
  // Every name identity is distinct from every other identity on this object.
  // CatalogNamePayloadMatchesMetadata requires name_vector != object,
  // name_vector != name_entry and object != name_entry; the enclosing request
  // additionally refuses any repeat across its whole identity set.
  if (!IsEngineIdentityUuid(input.name_vector_uuid) ||
      input.name_vector_uuid == input.object_uuid ||
      input.name_vector_uuid == input.row_uuid) {
    out.error = E::name_vector_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.default_name_entry_uuid) ||
      input.default_name_entry_uuid == input.object_uuid ||
      input.default_name_entry_uuid == input.row_uuid ||
      input.default_name_entry_uuid == input.name_vector_uuid) {
    out.error = E::name_entry_identity_invalid;
    return out;
  }
  if (input.object_subtype.empty()) {
    out.error = E::subtype_empty;
    return out;
  }
  // The same key rule the metadata codec applies. Checking it here turns an
  // unusable envelope into a cause at its construction site, instead of an
  // invalid_metadata refusal several layers away with nothing naming the field.
  const auto valid_key = [](std::string_view key, std::size_t maximum) {
    if (key.empty() || key.size() > maximum) return false;
    for (const unsigned char ch : key)
      if (ch < 33 || ch > 126) return false;
    return true;
  };
  if (!valid_key(input.object_subtype, 256)) {
    out.error = E::subtype_invalid;
    return out;
  }
  if (!valid_key(input.trace_search_key, 4096)) {
    out.error = E::trace_search_key_invalid;
    return out;
  }
  if (!valid_key(input.retention_class, 256)) {
    out.error = E::retention_class_invalid;
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
  // resource_epoch is load-bearing for the name half: the name entry's own
  // resource_epoch must equal this one and must be non-zero, so a zero here
  // produces a name row that cannot validate.
  if (!input.definition_version || !input.catalog_generation ||
      !input.security_epoch || !input.resource_epoch ||
      !input.creator_local_transaction_id) {
    out.error = E::generation_invalid;
    return out;
  }

  auto& m = out.metadata;
  m.record.header.kind = input.kind;
  m.record.header.object_uuid = {p::UuidKind::object, input.object_uuid};
  m.record.header.row_uuid = {p::UuidKind::row, input.row_uuid};
  // Same value as the owning schema, under object kind. MatchesMetadata
  // requires both the kinds and the equality.
  m.record.header.parent_uuid = {p::UuidKind::object, input.owning_schema_uuid};
  m.record.payload.clear();  // By contract: the assembler fills this.
  // Read by PreflightLocalNamedCreationRecord against the supplied name vector.
  m.name_vector_uuid = {p::UuidKind::object, input.name_vector_uuid};
  m.default_name_uuid = {p::UuidKind::object, input.default_name_entry_uuid};

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
  m.trace_search_key = input.trace_search_key;
  m.retention_class = input.retention_class;

  out.error = E::none;
  return out;
}

// ---------------------------------------------------------------------------
// The name halves.
//
// A definition envelope alone is not a submittable creation input.
// AssembleLocalNamedCreationRecords consumes CatalogNamedCreationInput, which
// carries a definition, a name-vector metadata version plus its CatalogNameVector
// value, and one metadata version plus CatalogNameEntry value per name. So one
// object costs three metadata versions and two name value structs, and five
// fresh identities beyond its own object identity: the definition row, the name
// vector object and row, and the name entry object and row.
//
// Everything structural here is derived from the definition envelope, because
// PreflightLocalNamedCreationRecord and CatalogNamePayloadMatchesMetadata check
// the two halves against each other field for field. Only genuinely caller-owned
// material is an input: the name text, its identifier-profile normalization, and
// the three naming-profile identities the name schemas require
// (name_collision_policy, dialect_profile, identifier_profile).
//
// Normalization is NOT derived from the name text here. The normalized lookup
// key is identifier-profile authority and a guessed one would resolve names
// wrongly while encoding cleanly, so an absent key is a refusal.

struct BootstrapNamedCreationUnitInput {
  // Carries the object identity, the definition row identity, the two name
  // identities, the kind, the subtype and the shared bootstrap identities.
  BootstrapDefinitionMetadataInput definition;

  // Fresh row identities for the two name metadata versions.
  core::platform::Uuid name_vector_row_uuid;
  core::platform::Uuid name_entry_row_uuid;

  // Must equal CatalogRecordKindName(definition.kind): the assembler refuses a
  // name vector whose object_class disagrees with the kind it registered.
  std::string object_class;
  // Must equal the request's database_default_language: the default name entry
  // is the one the default language resolves to.
  std::string language_tag;

  // Caller authority. The source pack supplies identities and typed values,
  // never name authority.
  std::string name_text;
  std::vector<core::platform::byte> normalized_lookup_key;

  core::platform::Uuid name_collision_policy_uuid;
  core::platform::Uuid dialect_profile_uuid;
  core::platform::Uuid identifier_profile_uuid;

  // Non-zero by schema. Distinct from resource_epoch, which the metadata
  // version owns and the name entry must agree with.
  core::platform::u64 name_resolution_epoch = 1;
};

enum class BootstrapNamedCreationUnitError {
  none,
  definition_invalid,
  name_vector_row_identity_invalid,
  name_entry_row_identity_invalid,
  object_class_empty,
  language_tag_empty,
  name_text_empty,
  normalized_lookup_key_empty,
  collision_policy_identity_invalid,
  dialect_profile_identity_invalid,
  identifier_profile_identity_invalid,
  name_resolution_epoch_invalid,
};

inline const char* BootstrapNamedCreationUnitDiagnosticCode(
    BootstrapNamedCreationUnitError error) noexcept {
  using E = BootstrapNamedCreationUnitError;
  switch (error) {
    case E::none: return "";
    case E::definition_invalid: return "CATALOG.NAMED_UNIT_DEFINITION_INVALID";
    case E::name_vector_row_identity_invalid: return "CATALOG.NAMED_UNIT_VECTOR_ROW_IDENTITY_INVALID";
    case E::name_entry_row_identity_invalid: return "CATALOG.NAMED_UNIT_ENTRY_ROW_IDENTITY_INVALID";
    case E::object_class_empty: return "CATALOG.NAMED_UNIT_OBJECT_CLASS_EMPTY";
    case E::language_tag_empty: return "CATALOG.NAMED_UNIT_LANGUAGE_TAG_EMPTY";
    case E::name_text_empty: return "CATALOG.NAMED_UNIT_NAME_TEXT_EMPTY";
    case E::normalized_lookup_key_empty: return "CATALOG.NAMED_UNIT_NORMALIZED_KEY_EMPTY";
    case E::collision_policy_identity_invalid: return "CATALOG.NAMED_UNIT_COLLISION_POLICY_IDENTITY_INVALID";
    case E::dialect_profile_identity_invalid: return "CATALOG.NAMED_UNIT_DIALECT_PROFILE_IDENTITY_INVALID";
    case E::identifier_profile_identity_invalid: return "CATALOG.NAMED_UNIT_IDENTIFIER_PROFILE_IDENTITY_INVALID";
    case E::name_resolution_epoch_invalid: return "CATALOG.NAMED_UNIT_NAME_RESOLUTION_EPOCH_INVALID";
  }
  return "CATALOG.NAMED_UNIT_DEFINITION_INVALID";
}

// Owning storage for one object's complete creation input. `entries` and the
// input returned by Input() hold pointers into this object, so a unit that is
// copied or moved must be rebound at its final address before use. Bind() is
// deliberately separate from construction for that reason: build, store, bind.
struct BootstrapNamedCreationUnit {
  core::catalog::CatalogMetadataVersion definition;
  core::catalog::CatalogMetadataVersion vector_metadata;
  core::catalog::CatalogMetadataVersion entry_metadata;
  core::catalog::CatalogNameVector vector;
  core::catalog::CatalogNameEntry entry;
  std::array<core::catalog::CatalogCreationNameEntryInput, 1> entries{};

  // Call this at the unit's final address. The builder also calls it, for a
  // caller that uses the unit in place, but that call is not what makes the
  // input valid: a unit moved into a container must be rebound there.
  void Bind() noexcept { entries[0] = {&entry_metadata, &entry}; }
  core::catalog::CatalogNamedCreationInput Input() const noexcept {
    return {&definition, &vector_metadata, &vector, entries};
  }
};

struct BootstrapNamedCreationUnitResult {
  BootstrapNamedCreationUnitError error =
      BootstrapNamedCreationUnitError::definition_invalid;
  // Set only when error == definition_invalid, so the definition cause is not
  // flattened into one opaque refusal.
  BootstrapDefinitionMetadataError definition_error =
      BootstrapDefinitionMetadataError::none;
  BootstrapNamedCreationUnit unit;
  bool ok() const noexcept {
    return error == BootstrapNamedCreationUnitError::none;
  }
};

inline BootstrapNamedCreationUnitResult BuildBootstrapNamedCreationUnit(
    const BootstrapNamedCreationUnitInput& input) {
  namespace c = core::catalog;
  namespace p = core::platform;
  using E = BootstrapNamedCreationUnitError;
  using core::uuid::IsEngineIdentityUuid;

  BootstrapNamedCreationUnitResult out;
  const auto built = BuildBootstrapDefinitionMetadata(input.definition);
  if (!built.ok()) {
    out.definition_error = built.error;
    return out;
  }
  if (!IsEngineIdentityUuid(input.name_vector_row_uuid) ||
      input.name_vector_row_uuid == input.definition.object_uuid ||
      input.name_vector_row_uuid == input.definition.row_uuid ||
      input.name_vector_row_uuid == input.definition.name_vector_uuid ||
      input.name_vector_row_uuid == input.definition.default_name_entry_uuid) {
    out.error = E::name_vector_row_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.name_entry_row_uuid) ||
      input.name_entry_row_uuid == input.definition.object_uuid ||
      input.name_entry_row_uuid == input.definition.row_uuid ||
      input.name_entry_row_uuid == input.definition.name_vector_uuid ||
      input.name_entry_row_uuid == input.definition.default_name_entry_uuid ||
      input.name_entry_row_uuid == input.name_vector_row_uuid) {
    out.error = E::name_entry_row_identity_invalid;
    return out;
  }
  if (input.object_class.empty()) {
    out.error = E::object_class_empty;
    return out;
  }
  if (input.language_tag.empty()) {
    out.error = E::language_tag_empty;
    return out;
  }
  if (input.name_text.empty()) {
    out.error = E::name_text_empty;
    return out;
  }
  if (input.normalized_lookup_key.empty()) {
    out.error = E::normalized_lookup_key_empty;
    return out;
  }
  if (!IsEngineIdentityUuid(input.name_collision_policy_uuid)) {
    out.error = E::collision_policy_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.dialect_profile_uuid)) {
    out.error = E::dialect_profile_identity_invalid;
    return out;
  }
  if (!IsEngineIdentityUuid(input.identifier_profile_uuid)) {
    out.error = E::identifier_profile_identity_invalid;
    return out;
  }
  if (!input.name_resolution_epoch) {
    out.error = E::name_resolution_epoch_invalid;
    return out;
  }

  auto& unit = out.unit;
  unit.definition = built.metadata;
  // Read back from the envelope rather than re-deriving from the input, so the
  // two halves cannot disagree about a kind label.
  const auto object = unit.definition.record.header.object_uuid;
  const auto name_vector = unit.definition.name_vector_uuid;
  const auto name_entry = unit.definition.default_name_uuid;
  const auto owning_schema = unit.definition.owning_schema_uuid;
  const auto schema_as_object = unit.definition.record.header.parent_uuid;
  const auto policy = unit.definition.security_policy_uuid;

  // Both name versions inherit the definition's authority scope, visibility,
  // lifecycle, status, generations, owner, audit, policy and owning schema.
  // Only identity, kind, parentage and subtype differ.
  unit.vector_metadata = unit.definition;
  unit.vector_metadata.record.header.kind = c::CatalogRecordKind::localized_name;
  unit.vector_metadata.record.header.object_uuid = name_vector;
  unit.vector_metadata.record.header.row_uuid = {p::UuidKind::row, input.name_vector_row_uuid};
  unit.vector_metadata.record.header.parent_uuid = object;
  unit.vector_metadata.object_subtype = "name_vector";

  unit.entry_metadata = unit.definition;
  unit.entry_metadata.record.header.kind = c::CatalogRecordKind::localized_name;
  unit.entry_metadata.record.header.object_uuid = name_entry;
  unit.entry_metadata.record.header.row_uuid = {p::UuidKind::row, input.name_entry_row_uuid};
  unit.entry_metadata.record.header.parent_uuid = name_vector;
  unit.entry_metadata.object_subtype = "name_entry";

  auto& v = unit.vector;
  v.name_vector_uuid = name_vector;
  v.object_uuid = object;
  v.object_class = input.object_class;
  v.owning_schema_uuid = owning_schema;
  v.default_language_tag = input.language_tag;
  v.default_name_entry_uuid = name_entry;
  v.name_collision_policy_uuid = {p::UuidKind::object, input.name_collision_policy_uuid};
  // Taken from the metadata version, not defaulted in parallel with it: the
  // matcher requires equality and two independent defaults drift silently.
  v.catalog_generation_id = unit.vector_metadata.catalog_generation;
  v.security_policy_uuid = policy;
  v.lifecycle_state = c::CatalogNameLifecycle::active;

  auto& e = unit.entry;
  e.name_entry_uuid = name_entry;
  e.name_vector_uuid = name_vector;
  e.object_uuid = object;
  e.object_class = input.object_class;
  // The schema the name resolves within, under the kind each field requires.
  e.scope_uuid = schema_as_object;
  e.parent_schema_uuid = owning_schema;
  e.language_tag = input.language_tag;
  e.name_class = c::CatalogNameClass::primary;
  e.dialect_profile_uuid = {p::UuidKind::object, input.dialect_profile_uuid};
  e.identifier_profile_uuid = {p::UuidKind::object, input.identifier_profile_uuid};
  e.raw_name_text = input.name_text;
  e.display_name = input.name_text;
  e.normalized_lookup_key = input.normalized_lookup_key;
  e.catalog_generation_id = unit.entry_metadata.catalog_generation;
  e.created_transaction_uuid = unit.definition.creator_transaction_uuid;
  e.security_policy_uuid = policy;
  e.resource_epoch = unit.entry_metadata.resource_epoch;
  e.name_resolution_epoch = input.name_resolution_epoch;
  // This is the object's default name in its default language. An object with
  // exactly one name has exactly one default, which the assembler requires.
  e.default_for_language = true;
  e.default_for_object = true;
  e.lifecycle_state = c::CatalogNameLifecycle::active;

  unit.Bind();
  out.error = E::none;
  return out;
}

}  // namespace scratchbird::storage::database
