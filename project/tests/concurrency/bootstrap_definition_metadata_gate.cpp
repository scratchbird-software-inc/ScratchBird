// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// The repeated unit of the definitions vector that
// NativeInitialPolicySecurityRequest requires. Each identity conjunct is
// asserted separately, because a metadata envelope built from a bad identity
// encodes cleanly and is rejected much later by MatchesMetadata, where it
// presents as a missing object rather than a malformed one.

#include "../../src/storage/database/native_bootstrap_definition_metadata.hpp"

#include <cstdio>
#include <cstdlib>

namespace db = scratchbird::storage::database;
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
using E = db::BootstrapDefinitionMetadataError;

namespace {
int checks = 0;
void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) { std::fprintf(stderr, "FAIL bootstrap definition metadata: %s\n", what); std::exit(1); }
}
p::Uuid Id(std::uint8_t tail) {
  p::Uuid v{{0x01,0x8f,0x7a,0x10,0x12,0x80,0x70,0x00,0x80,0,0,0,0,0,0,0}};
  v.bytes[15] = tail; return v;
}
db::BootstrapDefinitionMetadataInput Good() {
  db::BootstrapDefinitionMetadataInput in;
  in.object_uuid = Id(0x51);
  in.row_uuid = Id(0x57);
  in.name_vector_uuid = Id(0x58);
  in.default_name_entry_uuid = Id(0x59);
  in.kind = c::CatalogRecordKind::config_profile;
  in.object_subtype = "agent_runtime_authority";
  in.owner_principal_uuid = Id(0x52);
  in.audit_object_uuid = Id(0x53);
  in.security_policy_uuid = Id(0x54);
  in.owning_schema_uuid = Id(0x55);
  in.creator_transaction_uuid = {p::UuidKind::transaction, Id(0x56)};
  return in;
}
// An admitted ASCII-lane identifier profile: no resource, platform or dialect
// binding, so the engine's key derivation can run without an admission this
// path cannot give. Lower-folding, so the folded and exact lanes differ and a
// test cannot pass by accident on identical lanes.
c::CatalogIdentifierResolutionProfile Profile() {
  c::CatalogIdentifierResolutionProfile profile;
  profile.profile_uuid = Id(0x5e);
  profile.database_uuid = Id(0x50);
  profile.generation = 1;
  profile.profile_name = "bootstrap_definition_metadata_gate_profile";
  profile.quote_styles = 1;
  profile.escape_rule = c::IdentifierEscapeRule::doubled_delimiter;
  profile.unquoted_fold_mode = c::IdentifierFoldMode::lower_ascii;
  profile.quoted_preserves_case = true;
  profile.comparison_mode = c::IdentifierComparisonMode::folded;
  profile.storage_display_mode = c::IdentifierDisplayMode::preserve_user_text;
  profile.reserved_word_behavior = c::IdentifierReservedWordMode::reserved_requires_quote;
  profile.maximum_identifier_length = 128;
  profile.length_unit = c::IdentifierLengthUnit::unicode_scalar_values;
  profile.origin_transaction_uuid = {p::UuidKind::transaction, Id(0x56)};
  profile.origin_local_transaction_id = 1;
  return profile;
}
// Static so the borrowed pointer and profile_name backing outlive every call.
const c::CatalogIdentifierResolutionProfile& SharedProfile() {
  static const c::CatalogIdentifierResolutionProfile profile = Profile();
  return profile;
}
db::BootstrapNamedCreationUnitInput GoodUnit() {
  db::BootstrapNamedCreationUnitInput in;
  in.definition = Good();
  in.name_vector_row_uuid = Id(0x5a);
  in.name_entry_row_uuid = Id(0x5b);
  in.language_uuid = c::kCatalogLanguage_en_US;
  in.name_text = "Supplied_Name";
  in.name_collision_policy_uuid = Id(0x5c);
  in.dialect_profile_uuid = Id(0x5d);
  in.identifier_profile = &SharedProfile();
  return in;
}
}  // namespace

int main() {
  // 1. A complete input yields an envelope with every field the strict matcher
  //    reads, under the kind it reads it as.
  {
    const auto out = db::BuildBootstrapDefinitionMetadata(Good());
    Check(out.ok(), "a complete input builds");
    const auto& m = out.metadata;
    Check(m.record.header.kind == c::CatalogRecordKind::config_profile, "kind preserved");
    Check(m.record.header.object_uuid.value == Id(0x51) &&
          m.record.header.object_uuid.kind == p::UuidKind::object, "object identity and kind");
    Check(m.record.header.row_uuid.kind == p::UuidKind::row &&
          m.record.header.row_uuid.value == Id(0x57),
          "row identity is present and distinct; the request refuses duplicates "
          "across every definition, vector and entry identity");
    Check(m.record.header.row_uuid.value != m.record.header.object_uuid.value,
          "row identity differs from object identity");
    Check(m.record.payload.empty(),
          "payload stays empty; the assembler refuses a supplied payload");
    Check(m.name_vector_uuid.kind == p::UuidKind::object &&
          m.name_vector_uuid.value == Id(0x58),
          "the definition carries its name-vector identity, which the named "
          "creation preflight checks the supplied vector against");
    Check(m.default_name_uuid.kind == p::UuidKind::object &&
          m.default_name_uuid.value == Id(0x59),
          "the definition carries its default name-entry identity, which the "
          "named creation preflight checks the supplied vector against");
    Check(m.name_vector_uuid.value != m.record.header.object_uuid.value &&
          m.name_vector_uuid.value != m.default_name_uuid.value &&
          m.default_name_uuid.value != m.record.header.object_uuid.value,
          "object, name-vector and name-entry identities are all distinct");
    Check(m.object_subtype == "agent_runtime_authority", "subtype preserved verbatim");
    // (No trace search key or retention class to carry: SB_CATALOG_NO_TEXT_IDENTIFIERS,
    // owner Q57 = 1, Q58 = 1; the metadata codec no longer has them.)
    Check(m.owner_uuid.kind == p::UuidKind::principal && m.owner_uuid.value == Id(0x52),
          "owner is a principal identity");
    Check(m.audit_uuid.kind == p::UuidKind::object && m.audit_uuid.value == Id(0x53),
          "audit is an object identity");
    Check(m.security_policy_uuid.kind == p::UuidKind::object &&
          m.security_policy_uuid.value == Id(0x54), "security policy is an object identity");
    Check(m.owning_schema_uuid.kind == p::UuidKind::schema &&
          m.owning_schema_uuid.value == Id(0x55), "owning schema carries schema kind");
    Check(m.record.header.parent_uuid.kind == p::UuidKind::object &&
          m.record.header.parent_uuid.value == Id(0x55),
          "parent carries the same value under object kind");
    Check(m.owning_schema_uuid.value == m.record.header.parent_uuid.value,
          "schema and parent values are equal, as the strict matcher requires");
    Check(m.authority_scope == c::CatalogAuthorityScope::local, "authority scope local");
    Check(m.visibility == c::CatalogVisibilityClass::internal_only, "visibility internal_only");
    Check(m.lifecycle == c::CatalogObjectLifecycle::active, "lifecycle active");
    Check(m.status == c::CatalogObjectStatus::active, "status active");
    Check(m.creator_transaction_uuid.kind == p::UuidKind::transaction,
          "creator transaction carries transaction kind");
  }

  // 2. Each identity is refused on its own. One combined validity check would
  //    let a dropped conjunct hide behind the others.
  {
    struct Case { const char* what; db::BootstrapDefinitionMetadataInput in; E expected; };
    auto nil_object = Good(); nil_object.object_uuid = {};
    auto nil_row = Good(); nil_row.row_uuid = {};
    auto same_row = Good(); same_row.row_uuid = same_row.object_uuid;
    auto nil_vector = Good(); nil_vector.name_vector_uuid = {};
    auto vector_is_object = Good(); vector_is_object.name_vector_uuid = vector_is_object.object_uuid;
    auto nil_entry = Good(); nil_entry.default_name_entry_uuid = {};
    auto entry_is_vector = Good(); entry_is_vector.default_name_entry_uuid = entry_is_vector.name_vector_uuid;
    auto no_subtype = Good(); no_subtype.object_subtype.clear();
    auto spaced_subtype = Good(); spaced_subtype.object_subtype = "agent runtime authority";
    auto nil_owner = Good(); nil_owner.owner_principal_uuid = {};
    auto nil_audit = Good(); nil_audit.audit_object_uuid = {};
    auto nil_policy = Good(); nil_policy.security_policy_uuid = {};
    auto nil_schema = Good(); nil_schema.owning_schema_uuid = {};
    auto wrong_tx_kind = Good(); wrong_tx_kind.creator_transaction_uuid = {p::UuidKind::object, Id(0x56)};
    auto zero_gen = Good(); zero_gen.definition_version = 0;

    const Case cases[] = {
      {"a nil object identity refuses", nil_object, E::object_identity_invalid},
      {"a nil row identity refuses", nil_row, E::row_identity_invalid},
      {"a row identity equal to the object identity refuses", same_row, E::row_identity_invalid},
      {"a nil name-vector identity refuses", nil_vector, E::name_vector_identity_invalid},
      {"a name-vector identity equal to the object identity refuses",
       vector_is_object, E::name_vector_identity_invalid},
      {"a nil name-entry identity refuses", nil_entry, E::name_entry_identity_invalid},
      {"a name-entry identity equal to the name-vector identity refuses",
       entry_is_vector, E::name_entry_identity_invalid},
      {"an empty subtype refuses", no_subtype, E::subtype_empty},
      {"a subtype that is not a printable key refuses", spaced_subtype, E::subtype_invalid},
      // (The trace-key and retention-class refusals went with the fields,
      // owner Q57 = 1, Q58 = 1.)
      {"a nil owner refuses", nil_owner, E::owner_identity_invalid},
      {"a nil audit identity refuses", nil_audit, E::audit_identity_invalid},
      {"a nil security policy refuses", nil_policy, E::policy_identity_invalid},
      {"a nil owning schema refuses", nil_schema, E::schema_identity_invalid},
      {"a creator transaction of the wrong kind refuses", wrong_tx_kind, E::transaction_identity_invalid},
      {"a zero definition version refuses", zero_gen, E::generation_invalid},
      // Was "a zero resource epoch refuses, because the name entry must agree
      // with it" until 2026-11-04 (SB_DRIFT_DEFINITION_RESOURCE_EPOCH): the
      // entry agrees with its own metadata version, not the definition's, and
      // zero is the no-resource-dependency value. The non-zero refusal moved to
      // the unit's name_resource_epoch (section 4).
    };
    for (const auto& k : cases) {
      const auto out = db::BuildBootstrapDefinitionMetadata(k.in);
      Check(out.error == k.expected, k.what);
      Check(!out.ok(), "a refusal never reports usable metadata");
      Check(db::BootstrapDefinitionMetadataDiagnosticCode(out.error)[0] != '\0',
            "a refusal carries a diagnostic code");
    }
    auto no_resource = Good(); no_resource.resource_epoch = 0;
    const auto independent = db::BuildBootstrapDefinitionMetadata(no_resource);
    Check(independent.ok() && independent.metadata.resource_epoch == 0,
          "a definition with no resource dependency builds with a zero resource epoch");
    Check(db::BootstrapDefinitionMetadataInput{}.resource_epoch == 0,
          "the default definition declares no resource dependency");
  }

  // 3. The named-creation unit. These assertions cover the structure the
  //    consumers read; they do NOT establish that the consumers accept it.
  //    Only calling the assembler does that, which the generated-definition
  //    creation gate does with a real loaded seed pack.
  {
    const auto out = db::BuildBootstrapNamedCreationUnit(GoodUnit());
    Check(out.ok(), "a complete unit builds");
    auto unit = out.unit;
    unit.Bind();  // The unit was copied out of the result; rebind at its address.
    const auto& d = unit.definition;
    const auto& vm = unit.vector_metadata;
    const auto& em = unit.entry_metadata;
    const auto& v = unit.vector;
    const auto& e = unit.entry;

    Check(vm.record.header.kind == c::CatalogRecordKind::localized_name &&
          em.record.header.kind == c::CatalogRecordKind::localized_name,
          "both name versions carry the localized_name kind");
    Check(vm.object_subtype == "name_vector", "the vector version is subtyped name_vector");
    Check(em.object_subtype == "name_entry", "the entry version is subtyped name_entry");
    Check(d.record.header.kind == c::CatalogRecordKind::config_profile,
          "the definition keeps its own kind");

    Check(vm.record.header.object_uuid.value == d.name_vector_uuid.value,
          "the vector version is identified by the definition's name-vector identity");
    Check(vm.record.header.parent_uuid.value == d.record.header.object_uuid.value,
          "the vector version is parented to the object");
    Check(em.record.header.object_uuid.value == d.default_name_uuid.value,
          "the entry version is identified by the definition's name-entry identity");
    Check(em.record.header.parent_uuid.value == d.name_vector_uuid.value,
          "the entry version is parented to the name vector");
    Check(vm.record.header.row_uuid.value == Id(0x5a) &&
          em.record.header.row_uuid.value == Id(0x5b),
          "each name version carries its own supplied row identity");
    Check(vm.record.payload.empty() && em.record.payload.empty(),
          "name versions carry no definition payload");

    Check(v.name_vector_uuid.value == d.name_vector_uuid.value,
          "the vector value agrees with the definition on the vector identity");
    Check(v.default_name_entry_uuid.value == d.default_name_uuid.value,
          "the vector value agrees with the definition on the default entry identity");
    Check(v.object_uuid.value == d.record.header.object_uuid.value,
          "the vector value names the object it belongs to");
    // SB_CATALOG_NO_TEXT_IDENTIFIERS: the class is the definition's kind itself
    // (no supplied kind name to cross-check any more).
    Check(v.object_kind == d.record.header.kind && v.object_kind == c::CatalogRecordKind::config_profile,
          "object_kind is the definition's kind");
    Check(v.owning_schema_uuid.has_value() &&
          v.owning_schema_uuid->value == d.owning_schema_uuid.value,
          "the vector value carries the owning schema");
    Check(v.catalog_generation_id == vm.catalog_generation,
          "the vector generation is taken from its metadata version, not defaulted beside it");
    Check(v.security_policy_uuid.value == d.security_policy_uuid.value,
          "the vector value carries the definition's security policy");
    Check(v.lifecycle_state == c::CatalogNameLifecycle::active,
          "the vector is active, not left in the creating default");
    Check(v.default_language_uuid.value == c::kCatalogLanguage_en_US, "the vector default language is the supplied language");

    Check(e.name_entry_uuid.value == d.default_name_uuid.value &&
          e.name_vector_uuid.value == d.name_vector_uuid.value &&
          e.object_uuid.value == d.record.header.object_uuid.value,
          "the entry value carries all three identities it links");
    Check(e.object_kind == v.object_kind, "entry and vector agree on object_kind");
    Check(e.parent_schema_uuid.has_value() &&
          e.parent_schema_uuid->value == d.owning_schema_uuid.value,
          "the entry resolves within the owning schema");
    Check(e.scope_uuid.value == d.record.header.parent_uuid.value,
          "the entry scope is the schema, under the kind that field requires");
    Check(e.raw_name_text == "Supplied_Name",
          "the supplied label text is carried verbatim");
    Check(e.display_name == "Supplied_Name",
          "this profile preserves user text, so the display form is the supplied text");
    Check(e.identifier_profile_uuid.value == SharedProfile().profile_uuid,
          "the entry names the profile its keys were derived from, which is what the "
          "engine's verifier requires");
    const std::string folded = "supplied_name";
    Check(std::string(e.normalized_lookup_key.begin(), e.normalized_lookup_key.end()) == folded,
          "the folded lane is derived by the engine's own key derivation, not supplied");
    Check(std::string(e.exact_lookup_key.begin(), e.exact_lookup_key.end()) == "Supplied_Name",
          "the exact lane is derived too; it was previously left empty");
    Check(e.normalized_lookup_key != e.exact_lookup_key,
          "the two lanes actually differ under this profile, so neither is incidentally right");
    Check(!e.was_quoted && e.quote_style == c::CatalogNameQuoteStyle::none &&
          !e.requires_exact_match,
          "an engine-supplied label is unquoted and matched on the folded lane");
    Check(c::CheckCatalogIdentifierLookupKeys(SharedProfile(), c::BorrowCatalogNameEntry(e)) ==
          c::CatalogIdentifierKeyError::none,
          "the engine's own lookup-key verifier accepts the assembled entry");
    Check(e.created_transaction_uuid.value == d.creator_transaction_uuid.value,
          "the entry records the creating transaction the metadata version names");
    Check(e.resource_epoch == em.resource_epoch,
          "the entry resource epoch is taken from its metadata version");
    Check(em.resource_epoch == GoodUnit().name_resource_epoch && em.resource_epoch != 0 &&
          d.resource_epoch == GoodUnit().definition.resource_epoch && vm.resource_epoch == d.resource_epoch,
          "the entry carries the name key's resource epoch; the definition and vector keep the definition's");
    Check(e.catalog_generation_id == em.catalog_generation,
          "the entry generation is taken from its metadata version");
    Check(e.name_resolution_epoch != 0, "the entry carries a non-zero name resolution epoch");
    Check(e.default_for_object && e.default_for_language,
          "the single name is the object's default in the default language");
    Check(e.lifecycle_state == c::CatalogNameLifecycle::active, "the entry is active");

    const auto input = unit.Input();
    Check(input.definition == &unit.definition && input.vector_metadata == &unit.vector_metadata &&
          input.vector == &unit.vector && input.entries.size() == 1 &&
          input.entries[0].metadata == &unit.entry_metadata &&
          input.entries[0].entry == &unit.entry,
          "the bound input points at this unit's own storage");
  }

  // 4. Each caller obligation of the unit refuses on its own.
  {
    using U = db::BootstrapNamedCreationUnitError;
    struct Case { const char* what; db::BootstrapNamedCreationUnitInput in; U expected; };
    auto bad_definition = GoodUnit(); bad_definition.definition.owner_principal_uuid = {};
    auto nil_vector_row = GoodUnit(); nil_vector_row.name_vector_row_uuid = {};
    auto vector_row_is_object = GoodUnit();
    vector_row_is_object.name_vector_row_uuid = vector_row_is_object.definition.object_uuid;
    auto nil_entry_row = GoodUnit(); nil_entry_row.name_entry_row_uuid = {};
    auto repeated_entry_row = GoodUnit();
    repeated_entry_row.name_entry_row_uuid = repeated_entry_row.name_vector_row_uuid;
    auto no_language = GoodUnit(); no_language.language_uuid = Id(0xe1);  // an engine UUID that is no language constant
    auto no_text = GoodUnit(); no_text.name_text.clear();
    auto no_profile = GoodUnit(); no_profile.identifier_profile = nullptr;
    auto long_label = GoodUnit(); long_label.name_text = std::string(200, 'a');
    static const auto dialect_bound = [] {
      auto profile = Profile(); profile.dialect_mode_flags = 1; return profile;
    }();
    auto needs_dialect = GoodUnit(); needs_dialect.identifier_profile = &dialect_bound;
    auto nil_collision = GoodUnit(); nil_collision.name_collision_policy_uuid = {};
    auto nil_dialect = GoodUnit(); nil_dialect.dialect_profile_uuid = {};
    auto zero_resolution = GoodUnit(); zero_resolution.name_resolution_epoch = 0;
    auto zero_name_resource = GoodUnit(); zero_name_resource.name_resource_epoch = 0;

    const Case cases[] = {
      {"an invalid definition refuses the whole unit", bad_definition, U::definition_invalid},
      {"a nil name-vector row identity refuses", nil_vector_row, U::name_vector_row_identity_invalid},
      {"a name-vector row identity repeating the object identity refuses",
       vector_row_is_object, U::name_vector_row_identity_invalid},
      {"a nil name-entry row identity refuses", nil_entry_row, U::name_entry_row_identity_invalid},
      {"a name-entry row identity repeating the vector row identity refuses",
       repeated_entry_row, U::name_entry_row_identity_invalid},
      // (The "empty object class" refusal is gone with the input field: the
      // class is the definition's kind, so there is nothing to leave empty.)
      {"a language that is not a language constant refuses", no_language, U::language_unknown},
      {"an empty name text refuses", no_text, U::name_text_empty},
      {"an absent identifier profile refuses: the keys cannot be derived without it",
       no_profile, U::identifier_profile_missing},
      {"a label longer than the profile admits is refused by the engine's derivation",
       long_label, U::lookup_key_derivation_refused},
      {"a profile needing a dialect admission this path cannot give is refused",
       needs_dialect, U::lookup_key_derivation_refused},
      {"a nil name collision policy refuses", nil_collision, U::collision_policy_identity_invalid},
      {"a nil dialect profile refuses", nil_dialect, U::dialect_profile_identity_invalid},
      {"a zero name resolution epoch refuses", zero_resolution, U::name_resolution_epoch_invalid},
      {"a zero name resource epoch refuses: the name entry schema requires the key's resource epoch",
       zero_name_resource, U::name_resource_epoch_invalid},
    };
    for (const auto& k : cases) {
      const auto out = db::BuildBootstrapNamedCreationUnit(k.in);
      Check(out.error == k.expected, k.what);
      Check(!out.ok(), "a refused unit never reports as usable");
      Check(db::BootstrapNamedCreationUnitDiagnosticCode(out.error)[0] != '\0',
            "a refused unit carries a diagnostic code");
    }
    const auto definition_cause = db::BuildBootstrapNamedCreationUnit(bad_definition);
    Check(definition_cause.definition_error == E::owner_identity_invalid,
          "a definition refusal keeps its own cause instead of being flattened");
  }

  std::printf("PASS bootstrap definition metadata checks=%d\n", checks);
  return 0;
}
