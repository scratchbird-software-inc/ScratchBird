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
  in.trace_search_key = "BOOTSTRAP-DEFINITION-METADATA-GATE";
  in.retention_class = "catalog_history";
  in.owner_principal_uuid = Id(0x52);
  in.audit_object_uuid = Id(0x53);
  in.security_policy_uuid = Id(0x54);
  in.owning_schema_uuid = Id(0x55);
  in.creator_transaction_uuid = {p::UuidKind::transaction, Id(0x56)};
  return in;
}
db::BootstrapNamedCreationUnitInput GoodUnit() {
  db::BootstrapNamedCreationUnitInput in;
  in.definition = Good();
  in.name_vector_row_uuid = Id(0x5a);
  in.name_entry_row_uuid = Id(0x5b);
  in.object_class = "config_profile";
  in.language_tag = "en-US";
  in.name_text = "supplied_name";
  in.normalized_lookup_key.assign(in.name_text.begin(), in.name_text.end());
  in.name_collision_policy_uuid = Id(0x5c);
  in.dialect_profile_uuid = Id(0x5d);
  in.identifier_profile_uuid = Id(0x5e);
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
    Check(m.trace_search_key == "BOOTSTRAP-DEFINITION-METADATA-GATE" &&
          m.retention_class == "catalog_history",
          "both text keys the metadata codec requires are carried; an envelope "
          "without them cannot be encoded at all");
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
    auto no_trace = Good(); no_trace.trace_search_key.clear();
    auto spaced_trace = Good(); spaced_trace.trace_search_key = "not a key";
    auto no_retention = Good(); no_retention.retention_class.clear();
    auto nil_owner = Good(); nil_owner.owner_principal_uuid = {};
    auto nil_audit = Good(); nil_audit.audit_object_uuid = {};
    auto nil_policy = Good(); nil_policy.security_policy_uuid = {};
    auto nil_schema = Good(); nil_schema.owning_schema_uuid = {};
    auto wrong_tx_kind = Good(); wrong_tx_kind.creator_transaction_uuid = {p::UuidKind::object, Id(0x56)};
    auto zero_gen = Good(); zero_gen.definition_version = 0;
    auto zero_resource = Good(); zero_resource.resource_epoch = 0;

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
      {"an empty trace search key refuses, because the metadata codec requires one",
       no_trace, E::trace_search_key_invalid},
      {"a trace search key that is not a printable key refuses",
       spaced_trace, E::trace_search_key_invalid},
      {"an empty retention class refuses, because the metadata codec requires one",
       no_retention, E::retention_class_invalid},
      {"a nil owner refuses", nil_owner, E::owner_identity_invalid},
      {"a nil audit identity refuses", nil_audit, E::audit_identity_invalid},
      {"a nil security policy refuses", nil_policy, E::policy_identity_invalid},
      {"a nil owning schema refuses", nil_schema, E::schema_identity_invalid},
      {"a creator transaction of the wrong kind refuses", wrong_tx_kind, E::transaction_identity_invalid},
      {"a zero definition version refuses", zero_gen, E::generation_invalid},
      {"a zero resource epoch refuses, because the name entry must agree with it",
       zero_resource, E::generation_invalid},
    };
    for (const auto& k : cases) {
      const auto out = db::BuildBootstrapDefinitionMetadata(k.in);
      Check(out.error == k.expected, k.what);
      Check(!out.ok(), "a refusal never reports usable metadata");
      Check(db::BootstrapDefinitionMetadataDiagnosticCode(out.error)[0] != '\0',
            "a refusal carries a diagnostic code");
    }
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
    Check(v.object_class == "config_profile",
          "object_class is the supplied kind name, which the assembler cross-checks");
    Check(v.owning_schema_uuid.has_value() &&
          v.owning_schema_uuid->value == d.owning_schema_uuid.value,
          "the vector value carries the owning schema");
    Check(v.catalog_generation_id == vm.catalog_generation,
          "the vector generation is taken from its metadata version, not defaulted beside it");
    Check(v.security_policy_uuid.value == d.security_policy_uuid.value,
          "the vector value carries the definition's security policy");
    Check(v.lifecycle_state == c::CatalogNameLifecycle::active,
          "the vector is active, not left in the creating default");
    Check(v.default_language_tag == "en-US", "the vector default language is the supplied tag");

    Check(e.name_entry_uuid.value == d.default_name_uuid.value &&
          e.name_vector_uuid.value == d.name_vector_uuid.value &&
          e.object_uuid.value == d.record.header.object_uuid.value,
          "the entry value carries all three identities it links");
    Check(e.object_class == v.object_class, "entry and vector agree on object_class");
    Check(e.parent_schema_uuid.has_value() &&
          e.parent_schema_uuid->value == d.owning_schema_uuid.value,
          "the entry resolves within the owning schema");
    Check(e.scope_uuid.value == d.record.header.parent_uuid.value,
          "the entry scope is the schema, under the kind that field requires");
    Check(e.raw_name_text == "supplied_name" && e.display_name == "supplied_name",
          "the supplied name text is carried verbatim");
    Check(!e.normalized_lookup_key.empty(),
          "the supplied normalization is carried; none is derived here");
    Check(e.created_transaction_uuid.value == d.creator_transaction_uuid.value,
          "the entry records the creating transaction the metadata version names");
    Check(e.resource_epoch == em.resource_epoch,
          "the entry resource epoch is taken from its metadata version");
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
    auto no_class = GoodUnit(); no_class.object_class.clear();
    auto no_language = GoodUnit(); no_language.language_tag.clear();
    auto no_text = GoodUnit(); no_text.name_text.clear();
    auto no_key = GoodUnit(); no_key.normalized_lookup_key.clear();
    auto nil_collision = GoodUnit(); nil_collision.name_collision_policy_uuid = {};
    auto nil_dialect = GoodUnit(); nil_dialect.dialect_profile_uuid = {};
    auto nil_identifier = GoodUnit(); nil_identifier.identifier_profile_uuid = {};
    auto zero_resolution = GoodUnit(); zero_resolution.name_resolution_epoch = 0;

    const Case cases[] = {
      {"an invalid definition refuses the whole unit", bad_definition, U::definition_invalid},
      {"a nil name-vector row identity refuses", nil_vector_row, U::name_vector_row_identity_invalid},
      {"a name-vector row identity repeating the object identity refuses",
       vector_row_is_object, U::name_vector_row_identity_invalid},
      {"a nil name-entry row identity refuses", nil_entry_row, U::name_entry_row_identity_invalid},
      {"a name-entry row identity repeating the vector row identity refuses",
       repeated_entry_row, U::name_entry_row_identity_invalid},
      {"an empty object class refuses", no_class, U::object_class_empty},
      {"an empty language tag refuses", no_language, U::language_tag_empty},
      {"an empty name text refuses", no_text, U::name_text_empty},
      {"an absent normalized lookup key refuses rather than being derived from the text",
       no_key, U::normalized_lookup_key_empty},
      {"a nil name collision policy refuses", nil_collision, U::collision_policy_identity_invalid},
      {"a nil dialect profile refuses", nil_dialect, U::dialect_profile_identity_invalid},
      {"a nil identifier profile refuses", nil_identifier, U::identifier_profile_identity_invalid},
      {"a zero name resolution epoch refuses", zero_resolution, U::name_resolution_epoch_invalid},
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
