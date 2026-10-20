// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// The generated-definitions path, end to end against a real loaded seed pack.
//
// This gate exists because the consumers that decide whether a generated
// definition is well formed -- PreflightLocalNamedCreationRecord and
// AssembleLocalNamedCreationRecords -- are reachable only through the
// assembler. A unit test that asserts the fields the generator sets passes
// while the consumer rejects the result: that is exactly how a missing row
// identity survived a "mutation-verified" commit. So every assertion here runs
// on what the assembler returned, not on what the generator intended.
//
// No name, identity, normalization or placement authority is asserted. The
// identities below are fixture material, not create's.

#include "native_initial_policy_catalogs.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "catalog_security_record_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace db = scratchbird::storage::database;
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
using E = db::NativeInitialPolicyError;

namespace {
int checks = 0;
void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL generated definition creation: %s\n", what);
    std::exit(1);
  }
}
p::Uuid Id(unsigned n) {
  p::Uuid id{};
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  id.bytes[12] = static_cast<p::byte>(n >> 24); id.bytes[13] = static_cast<p::byte>(n >> 16);
  id.bytes[14] = static_cast<p::byte>(n >> 8); id.bytes[15] = static_cast<p::byte>(n);
  return id;
}
// The enrollment fixture's source identities: the selected provider and the
// deny-by-default policy profile the local service enrolls against.
p::Uuid SourceId(unsigned suffix) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(
      p::UuidKind::object, "018f7a10-1280-7000-8000-000000000001");
  if (!parsed.ok()) { std::fprintf(stderr, "FAIL fixture source identity\n"); std::exit(1); }
  auto id = parsed.value.value;
  id.bytes[14] = static_cast<p::byte>(suffix >> 8);
  id.bytes[15] = static_cast<p::byte>(suffix);
  return id;
}

constexpr p::Uuid kNil{};
const p::TypedUuid kTransaction{p::UuidKind::transaction, Id(7)};
const p::Uuid kDatabase = Id(1);
const p::Uuid kMemoryConfiguration = Id(12000);
const p::Uuid kBootstrapPrincipal = Id(14000);
const p::Uuid kBootstrapMembership = Id(14001);
// Fixture credential material, not a real secret and not a credential source.
const char* kVerifier =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=7b622f17d15a5e6f2d5122c606937dfc"
    "76f5982782adb52b03f7b1ca024f72c9";

// Schema roots, standing in for the identities create issues for the recursive
// catalog schema tree (`core/catalog/bootstrap_schema_roots.hpp`). The offsets
// mirror create's own: sys.security is +10024, sys.configuration +10025.
constexpr unsigned kSysSecuritySchema = 10024;
constexpr unsigned kSysConfigurationSchema = 10025;

// Where each enumerated family sits in that tree. Security records under
// sys.security; the pack, its providers and profiles, the memory configuration
// and the runtime authority binding under sys.configuration. This is a fixture
// placement decision, not an assertion about create's eventual choice -- the
// point under test is that placement is per object and is carried through.
p::Uuid SchemaFor(const db::NativeInitialDefinitionIdentity& object) {
  return c::IsCatalogSecurityRecordKind(object.kind) ? Id(kSysSecuritySchema)
                                                     : Id(kSysConfigurationSchema);
}

// Shared bootstrap identities. Schema placement is deliberately absent: it is
// per object. Create issues the first three today; the three naming-profile
// identities have no create-path source yet and are the caller's to issue
// either way.
db::NativeInitialGeneratedDefinitions Seed() {
  db::NativeInitialGeneratedDefinitions seed;
  seed.owner_principal_uuid = kBootstrapPrincipal;
  seed.audit_object_uuid = Id(10032);
  seed.security_policy_uuid = SourceId(0x402);  // the enrolled service policy
  seed.name_collision_policy_uuid = Id(20055);
  seed.dialect_profile_uuid = Id(20056);
  seed.identifier_profile_uuid = Id(20057);
  seed.trace_search_key = "NATIVE-GENERATED-DEFINITION-CREATION";
  seed.retention_class = "catalog_history";
  return seed;
}

std::array<db::NativeInitialMembershipIdentity, 5> Memberships() {
  std::array<db::NativeInitialMembershipIdentity, 5> out{};
  for (unsigned i = 0; i < out.size(); ++i)
    out[i] = {Id(13000 + i), SourceId(0x201 + i), SourceId(0x100 + i)};
  return out;
}

db::NativeInitialLocalServiceIdentity Service() {
  return {Id(15000), Id(15001), Id(15002), SourceId(0x10), SourceId(0x402),
          {Id(15100), Id(15101), Id(15102), Id(15103), Id(15104)}, "NodeService", kVerifier};
}

// Owning backing for the caller's half of the generated path.
struct Names {
  std::vector<std::string> text;
  std::vector<std::vector<p::byte>> keys;
  std::vector<db::NativeInitialDefinitionName> supplied;
  std::vector<p::Uuid> identities;
};

// Built from the enumeration, which is the whole point: nothing here predicts
// the object set, its kinds or its subtypes. Labels are unique per object here
// only because a fixture needs something to assert against; the tree, not the
// text, is what keeps two identical labels apart, which section 7 exercises.
Names NameEveryObject(const std::vector<db::NativeInitialDefinitionIdentity>& enumerated,
                      unsigned identity_base = 40000) {
  Names out;
  out.text.reserve(enumerated.size());
  out.keys.reserve(enumerated.size());
  out.supplied.reserve(enumerated.size());
  out.identities.reserve(5 * enumerated.size());
  for (unsigned at = 0; at < enumerated.size(); ++at) {
    out.text.push_back(enumerated[at].object_subtype + "_" + std::to_string(at));
    out.keys.emplace_back(out.text.back().begin(), out.text.back().end());
    for (unsigned slot = 0; slot < 5; ++slot)
      out.identities.push_back(Id(identity_base + 5 * at + slot));
  }
  for (unsigned at = 0; at < enumerated.size(); ++at)
    out.supplied.push_back({enumerated[at].object_uuid, SchemaFor(enumerated[at]),
                            out.text[at], out.keys[at]});
  return out;
}

db::NativeInitialPolicySecurityRequest ServiceRequest(
    const std::array<db::NativeInitialMembershipIdentity, 5>& memberships) {
  db::NativeInitialPolicySecurityRequest request{};
  request.policy.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  request.policy.database_uuid = kDatabase;
  request.policy.construction_transaction = kTransaction;
  request.policy.memory_configuration_uuid = kMemoryConfiguration;
  request.policy.database_default_language = "en-US";
  request.policy.limits = {1000, 4 * 1024 * 1024};
  request.group_memberships = memberships;
  request.bootstrap = db::NativeInitialBootstrapIdentity{
      kBootstrapPrincipal, kBootstrapMembership, "BootstrapAdmin", kVerifier};
  request.local_service = Service();
  return request;
}
}  // namespace

int main() {
  const auto memberships = Memberships();

  // 1. The enumeration is obtainable without supplying or predicting anything.
  const auto enumerated = db::EnumerateNativeInitialPolicySecurityDefinitions(ServiceRequest(memberships));
  if (!enumerated.ok())
    std::fprintf(stderr, "enumeration error=%d objects=%zu\n",
                 static_cast<int>(enumerated.error), enumerated.definitions.size());
  Check(enumerated.ok(), "the enumeration of a real loaded seed pack succeeds");
  Check(enumerated.error == E::none, "a successful enumeration reports no error");
  // 1 policy pack + 12 providers + 14 profiles + 1 memory configuration
  // + 5 roles + 6 groups + 5 memberships + 10 grants
  // + bootstrap role/principal/membership
  // + service principal, 5 grants and the runtime authority binding.
  Check(enumerated.definitions.size() == 64,
        "the enumeration covers every object of the default local-password pack");
  {
    unsigned subtypes[6]{};
    for (const auto& row : enumerated.definitions) {
      subtypes[0] += row.object_subtype == "bootstrap_policy_pack";
      subtypes[1] += row.object_subtype == "bootstrap_security_provider";
      subtypes[2] += row.object_subtype == "bootstrap_policy_profile";
      subtypes[3] += row.object_subtype == "memory_configuration";
      subtypes[4] += row.object_subtype == "agent_runtime_authority";
      subtypes[5] += c::IsCatalogSecurityRecordKind(row.kind) &&
                     row.object_subtype == c::CatalogRecordKindName(row.kind);
    }
    Check(subtypes[0] == 1, "exactly one policy pack is enumerated");
    Check(subtypes[1] == 12, "every security provider is enumerated");
    Check(subtypes[2] == 14, "every policy profile is enumerated");
    Check(subtypes[3] == 1, "the memory configuration is enumerated");
    Check(subtypes[4] == 1, "the service runtime authority binding is enumerated");
    Check(subtypes[5] == 35, "every security record is enumerated under its own kind name");
    Check(subtypes[0] + subtypes[1] + subtypes[2] + subtypes[3] + subtypes[4] + subtypes[5] == 64,
          "every enumerated object falls in one of those families and none is unaccounted for");
  }
  {
    auto identities = std::vector<p::Uuid>();
    for (const auto& row : enumerated.definitions) identities.push_back(row.object_uuid);
    std::sort(identities.begin(), identities.end());
    Check(std::adjacent_find(identities.begin(), identities.end()) == identities.end(),
          "every enumerated object identity is distinct");
    const auto binding = std::find_if(enumerated.definitions.begin(), enumerated.definitions.end(),
        [](const auto& row) { return row.object_subtype == "agent_runtime_authority"; });
    Check(binding != enumerated.definitions.end() &&
          binding->object_uuid == Service().runtime_binding_uuid,
          "the enumerated binding identity is the one the caller supplied");
  }

  // 2. The generated path assembles, and the consumers accept what it built.
  auto names = NameEveryObject(enumerated.definitions);
  const auto request = [&](const Names& supplied) {
    auto value = ServiceRequest(memberships);
    auto seed = Seed();
    seed.names = supplied.supplied;
    seed.fresh_identities = supplied.identities;
    value.policy.generated_definitions = seed;
    return value;
  };
  {
    const auto records = db::AssembleNativeInitialPolicySecurityRecords(request(names));
    if (!records.ok())
      std::fprintf(stderr, "assembly error=%d name_error=%d\n",
                   static_cast<int>(records.error), static_cast<int>(records.name_error));
    Check(records.ok(), "the generated path assembles a complete record set");
    Check(records.error == E::none && records.name_error == c::CatalogNamedCreationError::none,
          "no name or definition cause is reported alongside success");
    Check(records.records->definitions.size() == 64,
          "one definition record per enumerated object reaches the record set");
    Check(records.records->names.size() == 128,
          "each object carries exactly one name vector and one name entry");
    Check(records.records->encoded_bytes != 0, "the assembled set reports an encoded extent");

    // Every definition carries its payload and both name halves.
    unsigned vectors = 0, entries = 0;
    for (const auto& row : records.records->names) {
      Check(row.metadata.record.header.kind == c::CatalogRecordKind::localized_name,
            "every name row is a localized_name version");
      Check(row.name.has_value(), "every name row carries a name payload");
      vectors += row.metadata.object_subtype == "name_vector";
      entries += row.metadata.object_subtype == "name_entry";
    }
    Check(vectors == 64, "one name vector per object");
    Check(entries == 64, "one name entry per object");
    for (const auto& row : records.records->definitions) {
      Check(!row.metadata.record.payload.empty(),
            "every generated definition is completed with the assembler's own payload");
      Check(!row.name.has_value(), "a definition row carries no name payload");
      Check(row.metadata.owner_uuid.kind == p::UuidKind::principal &&
            row.metadata.owner_uuid.value == kBootstrapPrincipal,
            "every generated definition is owned by the bootstrap principal");
      Check(row.metadata.owning_schema_uuid.kind == p::UuidKind::schema &&
            row.metadata.record.header.parent_uuid.value == row.metadata.owning_schema_uuid.value,
            "the owning schema is presented under schema kind and matches the parent");
      // Placement is per object, and a security record does not sit where a
      // configuration record sits.
      const auto expected_schema =
          c::IsCatalogSecurityRecordKind(row.metadata.record.header.kind)
              ? Id(kSysSecuritySchema) : Id(kSysConfigurationSchema);
      Check(row.metadata.owning_schema_uuid.value == expected_schema,
            "each object is placed under the parent schema supplied for it, not under "
            "one schema shared by the whole cohort");
      Check(row.metadata.definition_version == 1 && row.metadata.catalog_generation == 1 &&
            row.metadata.security_epoch == 1 && row.metadata.creator_local_transaction_id == 1,
            "generated definitions are first versions of an initial construction");
    }

    // The supplied name text survives into the assembled entry rows, under the
    // object it was supplied for.
    for (unsigned at = 0; at < enumerated.definitions.size(); ++at) {
      const auto found = std::find_if(records.records->names.begin(), records.records->names.end(),
          [&](const auto& row) {
            const auto* entry = row.name ? std::get_if<c::CatalogNameEntry>(&*row.name) : nullptr;
            return entry && entry->object_uuid.value == enumerated.definitions[at].object_uuid;
          });
      Check(found != records.records->names.end(),
            "every enumerated object has an assembled name entry");
      const auto& entry = std::get<c::CatalogNameEntry>(*found->name);
      Check(entry.raw_name_text == names.text[at],
            "the assembled entry carries the name text supplied for that object");
      Check(entry.parent_schema_uuid.has_value() &&
            entry.parent_schema_uuid->value == SchemaFor(enumerated.definitions[at]) &&
            entry.scope_uuid.value == SchemaFor(enumerated.definitions[at]),
            "the label resolves within the object's own parent schema: both the entry's "
            "parent schema and its lookup scope are that schema");
      Check(entry.default_for_object && entry.default_for_language,
            "the single supplied name is the object's default in the default language");
      Check(entry.object_class == c::CatalogRecordKindName(enumerated.definitions[at].kind),
            "the assembled entry is classed by the kind the assembler registered");
    }

    // The consumer that has never had data: the runtime authority binding is
    // only resolvable if its metadata and payload agree.
    const auto binding = std::find_if(records.records->definitions.begin(),
        records.records->definitions.end(),
        [](const auto& row) { return row.metadata.object_subtype == "agent_runtime_authority"; });
    Check(binding != records.records->definitions.end(),
          "the assembled set contains the runtime authority binding");
    Check(c::CatalogRuntimeAuthorityBindingMatchesHeader(binding->metadata.record),
          "the binding payload agrees with its own record header");
    Check(c::CatalogRuntimeAuthorityBindingMatchesMetadata(binding->metadata),
          "the strict runtime authority matcher accepts the generated binding metadata");
  }

  // 3. The same request routed to catalog placement, which is the next stage of
  //    the chain and the shape create would submit.
  {
    const auto routed = db::AssembleNativeInitialPolicySecurityCatalogs(request(names));
    Check(routed.ok(), "the generated record set routes to catalog placement");
    Check(routed.plan->catalogs[1].size() == 128, "all names are placed in the name relation");
    Check(routed.plan->catalogs[3].size() == 35, "every security definition is placed");
    Check(routed.plan->catalogs[4].size() == 29,
          "pack, provider, profile, memory and runtime configuration definitions are placed");
    Check(routed.plan->catalogs[0].empty() && routed.plan->catalogs[2].empty() &&
          routed.plan->catalogs[5].empty(), "no other catalog relation is touched");
  }

  // 4. Each obligation of the generated path refuses on its own, and no
  //    refusal reports a partial record set.
  {
    struct Case { const char* what; E expected; };
    const auto refuse = [](const db::NativeInitialPolicySecurityRequest& value, const Case& k) {
      const auto out = db::AssembleNativeInitialPolicySecurityRecords(value);
      Check(out.error == k.expected, k.what);
      Check(!out.ok() && !out.records, "a refusal reports no partial record set");
    };
    {
      auto value = request(names);
      value.policy.definitions = {};  // already empty on this path
      auto other = names;
      other.supplied.pop_back();
      auto seed = *value.policy.generated_definitions;
      seed.names = other.supplied;
      value.policy.generated_definitions = seed;
      refuse(value, {"one name short of the enumeration refuses", E::invalid_names});
    }
    {
      auto value = request(names);
      auto other = names;
      other.supplied.push_back(other.supplied.back());
      auto seed = *value.policy.generated_definitions;
      seed.names = other.supplied;
      value.policy.generated_definitions = seed;
      refuse(value, {"one name more than the enumeration refuses", E::invalid_names});
    }
    {
      auto value = request(names);
      auto other = names;
      other.supplied.back().object_uuid = Id(99001);  // names an object that does not exist
      auto seed = *value.policy.generated_definitions;
      seed.names = other.supplied;
      value.policy.generated_definitions = seed;
      refuse(value, {"a name keyed to an unenumerated object refuses", E::invalid_names});
    }
    {
      auto value = request(names);
      auto other = names;
      other.supplied.front().name_text = {};
      auto seed = *value.policy.generated_definitions;
      seed.names = other.supplied;
      value.policy.generated_definitions = seed;
      refuse(value, {"an empty name text refuses", E::invalid_names});
    }
    {
      auto value = request(names);
      auto other = names;
      other.supplied.front().normalized_lookup_key = {};
      auto seed = *value.policy.generated_definitions;
      seed.names = other.supplied;
      value.policy.generated_definitions = seed;
      refuse(value, {"an absent normalization refuses rather than being derived",
                     E::invalid_names});
    }
    {
      auto value = request(names);
      auto other = names;
      other.identities.pop_back();
      auto seed = *value.policy.generated_definitions;
      seed.fresh_identities = other.identities;
      value.policy.generated_definitions = seed;
      refuse(value, {"one identity short of five per object refuses", E::invalid_request});
    }
    {
      auto value = request(names);
      auto other = names;
      other.identities.push_back(Id(99002));
      auto seed = *value.policy.generated_definitions;
      seed.fresh_identities = other.identities;
      value.policy.generated_definitions = seed;
      refuse(value, {"an over-supplied identity pool refuses rather than ignoring the surplus",
                     E::invalid_request});
    }
    {
      auto value = request(names);
      auto other = names;
      other.identities[7] = other.identities[2];  // a repeat inside the pool
      auto seed = *value.policy.generated_definitions;
      seed.fresh_identities = other.identities;
      value.policy.generated_definitions = seed;
      refuse(value, {"a repeated identity in the pool refuses", E::duplicate_identity});
    }
    {
      auto value = request(names);
      auto other = names;
      other.identities[3] = kNil;
      auto seed = *value.policy.generated_definitions;
      seed.fresh_identities = other.identities;
      value.policy.generated_definitions = seed;
      refuse(value, {"a nil identity in the pool refuses", E::invalid_definition});
    }
    {
      auto value = request(names);
      auto seed = *value.policy.generated_definitions;
      seed.security_policy_uuid = Id(99003);
      value.policy.generated_definitions = seed;
      refuse(value, {"a security policy that is not the enrolled service policy refuses, "
                     "because the binding would read back as absent", E::invalid_definition});
    }
    {
      auto value = request(names);
      auto seed = *value.policy.generated_definitions;
      seed.owner_principal_uuid = Id(99004);
      value.policy.generated_definitions = seed;
      refuse(value, {"an owner that is not the bootstrap principal refuses", E::invalid_definition});
    }
    {
      auto value = request(names);
      auto seed = *value.policy.generated_definitions;
      seed.audit_object_uuid = kNil;
      value.policy.generated_definitions = seed;
      refuse(value, {"a nil audit identity refuses", E::invalid_definition});
    }
    {
      auto value = request(names);
      auto other = names;
      other.supplied.front().owning_schema_uuid = kNil;
      auto seed = *value.policy.generated_definitions;
      seed.names = other.supplied;
      value.policy.generated_definitions = seed;
      refuse(value, {"a nil schema placement for one object refuses the whole cohort",
                     E::invalid_definition});
    }
    {
      auto value = request(names);
      auto seed = *value.policy.generated_definitions;
      seed.name_collision_policy_uuid = kNil;
      value.policy.generated_definitions = seed;
      refuse(value, {"a nil name collision policy refuses", E::invalid_names});
    }
    {
      auto value = request(names);
      auto seed = *value.policy.generated_definitions;
      seed.dialect_profile_uuid = kNil;
      value.policy.generated_definitions = seed;
      refuse(value, {"a nil dialect profile refuses", E::invalid_names});
    }
    {
      auto value = request(names);
      auto seed = *value.policy.generated_definitions;
      seed.identifier_profile_uuid = kNil;
      value.policy.generated_definitions = seed;
      refuse(value, {"a nil identifier profile refuses", E::invalid_names});
    }
    {
      // Both authorities at once is a merge, not a request.
      auto value = request(names);
      std::vector<c::CatalogNamedCreationInput> supplied(1);
      value.policy.definitions = supplied;
      refuse(value, {"supplying both a definitions list and a metadata seed refuses",
                     E::invalid_request});
    }
    {
      // The caller-supplied path still requires a complete list.
      auto value = ServiceRequest(memberships);
      refuse(value, {"neither definitions nor a metadata seed refuses", E::invalid_request});
    }
  }

  // 5. The identity supply is consumed five per object in enumeration order.
  //    This is what makes a positional supply safe to hand over, so it is
  //    asserted against the returned rows rather than assumed.
  {
    constexpr unsigned base = 50000;
    const auto shifted = NameEveryObject(enumerated.definitions, base);
    const auto out = db::AssembleNativeInitialPolicySecurityRecords(request(shifted));
    Check(out.ok(), "a shifted identity supply assembles");
    Check(out.records->definitions.size() == enumerated.definitions.size(),
          "the assembled definitions line up one-to-one with the enumeration");
    bool positional = true, enumerated_order = true;
    for (unsigned at = 0; at < out.records->definitions.size(); ++at) {
      const auto& metadata = out.records->definitions[at].metadata;
      enumerated_order = enumerated_order &&
          metadata.record.header.object_uuid.value == enumerated.definitions[at].object_uuid;
      positional = positional &&
          metadata.record.header.row_uuid.value == Id(base + 5 * at) &&
          metadata.name_vector_uuid.value == Id(base + 5 * at + 1) &&
          metadata.default_name_uuid.value == Id(base + 5 * at + 2);
    }
    Check(enumerated_order,
          "assembly reports objects in the same order the enumeration did, which is what "
          "lets the caller supply names and identities positionally");
    Check(positional,
          "each object consumed exactly its own five identities: definition row, name "
          "vector object and row, name entry object and row");
  }

  // 6. The policy-only entry point. A different code shape: no memberships, no
  //    bootstrap, no service, so the security block never runs and both service
  //    guards on the seed are inert. Counts are derived from the enumeration
  //    rather than predicted, which is the whole point of having one.
  {
    db::NativeInitialPolicyRequest policy_only{};
    policy_only.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    policy_only.database_uuid = kDatabase;
    policy_only.construction_transaction = kTransaction;
    policy_only.memory_configuration_uuid = kMemoryConfiguration;
    policy_only.database_default_language = "en-US";
    policy_only.limits = {1000, 4 * 1024 * 1024};

    const auto policy_enumeration = db::EnumerateNativeInitialPolicyDefinitions(policy_only);
    if (!policy_enumeration.ok())
      std::fprintf(stderr, "policy-only enumeration error=%d\n",
                   static_cast<int>(policy_enumeration.error));
    Check(policy_enumeration.ok(), "the policy-only enumeration succeeds");
    Check(policy_enumeration.definitions.size() < enumerated.definitions.size(),
          "the policy-only enumeration excludes the security cohort");
    bool no_security = true;
    unsigned pack = 0, memory = 0;
    for (const auto& row : policy_enumeration.definitions) {
      no_security = no_security && !c::IsCatalogSecurityRecordKind(row.kind) &&
                    row.object_subtype != "agent_runtime_authority";
      pack += row.object_subtype == "bootstrap_policy_pack";
      memory += row.object_subtype == "memory_configuration";
    }
    Check(no_security,
          "no security record and no runtime authority binding is enumerated without "
          "the identities that declare them");
    Check(pack == 1 && memory == 1,
          "the policy pack and memory configuration are enumerated on the policy-only path");

    const auto policy_names = NameEveryObject(policy_enumeration.definitions, 60000);
    auto seed = Seed();
    // No service is enrolled, so the policy identity is unconstrained here.
    seed.security_policy_uuid = Id(20058);
    seed.names = policy_names.supplied;
    seed.fresh_identities = policy_names.identities;
    policy_only.generated_definitions = seed;

    const auto policy_records = db::AssembleNativeInitialPolicyRecords(policy_only);
    if (!policy_records.ok())
      std::fprintf(stderr, "policy-only assembly error=%d name_error=%d\n",
                   static_cast<int>(policy_records.error),
                   static_cast<int>(policy_records.name_error));
    Check(policy_records.ok(), "the policy-only generated path assembles");
    Check(policy_records.records->definitions.size() == policy_enumeration.definitions.size(),
          "one definition per enumerated policy object");
    Check(policy_records.records->names.size() == 2 * policy_enumeration.definitions.size(),
          "one name vector and one name entry per enumerated policy object");
    for (const auto& row : policy_records.records->definitions)
      Check(!c::IsCatalogSecurityRecordKind(row.metadata.record.header.kind),
            "no security definition is assembled on the policy-only path");
    const auto policy_routed = db::AssembleNativeInitialPolicyCatalogs(policy_only);
    Check(policy_routed.ok(), "the policy-only set routes to catalog placement");
    Check(policy_routed.plan->catalogs[3].empty(),
          "the security relation stays untouched without a security cohort");
    Check(policy_routed.plan->catalogs[4].size() == policy_enumeration.definitions.size(),
          "every policy definition is placed in the configuration relation");
  }

  // 7. The recursive tree, not the text, is what keeps labels apart. Two
  //    objects in two different parent schemas carry the SAME label in the SAME
  //    language, and both assemble with their scopes preserved distinctly.
  //
  //    Scope of this assertion: the authoritative name index is keyed on
  //    (scope, object class, identifier profile, language, lookup key), so the
  //    structure these rows carry is what distinguishes them. It does NOT
  //    assert that this layer enforces collisions -- it explicitly does not,
  //    and a duplicate label in the SAME scope would also assemble here. That
  //    enforcement belongs to the naming collision policy, elsewhere.
  {
    const std::string shared_label = "bootstrap_shared_label";
    auto twinned = NameEveryObject(enumerated.definitions, 70000);
    // One security object and one configuration object, which SchemaFor put
    // under different parents.
    unsigned security_at = enumerated.definitions.size(), configuration_at = security_at;
    for (unsigned at = 0; at < enumerated.definitions.size(); ++at) {
      const bool security = c::IsCatalogSecurityRecordKind(enumerated.definitions[at].kind);
      if (security && security_at == enumerated.definitions.size()) security_at = at;
      if (!security && configuration_at == enumerated.definitions.size()) configuration_at = at;
    }
    Check(security_at != enumerated.definitions.size() &&
          configuration_at != enumerated.definitions.size(),
          "the enumeration offers an object in each of the two fixture schemas");
    Check(SchemaFor(enumerated.definitions[security_at]) !=
          SchemaFor(enumerated.definitions[configuration_at]),
          "the two chosen objects really are placed under different parent schemas");
    for (const auto at : {security_at, configuration_at}) {
      twinned.text[at] = shared_label;
      twinned.keys[at].assign(shared_label.begin(), shared_label.end());
      twinned.supplied[at].name_text = twinned.text[at];
      twinned.supplied[at].normalized_lookup_key = twinned.keys[at];
    }

    const auto out = db::AssembleNativeInitialPolicySecurityRecords(request(twinned));
    if (!out.ok())
      std::fprintf(stderr, "twinned-label assembly error=%d name_error=%d\n",
                   static_cast<int>(out.error), static_cast<int>(out.name_error));
    Check(out.ok(),
          "the same label in the same language assembles for two objects under "
          "different parent schemas");

    std::vector<const c::CatalogNameEntry*> twins;
    for (const auto& row : out.records->names)
      if (row.name)
        if (const auto* entry = std::get_if<c::CatalogNameEntry>(&*row.name);
            entry && entry->raw_name_text == shared_label)
          twins.push_back(entry);
    Check(twins.size() == 2, "both labelled entries are present in the assembled set");
    Check(twins[0]->object_uuid.value != twins[1]->object_uuid.value,
          "they label two different objects");
    Check(twins[0]->language_tag == twins[1]->language_tag,
          "they are in the same language");
    Check(twins[0]->object_class != twins[1]->object_class ||
          twins[0]->scope_uuid.value != twins[1]->scope_uuid.value,
          "and they are still distinguishable by the index key, which carries the scope");
    Check(twins[0]->scope_uuid.value != twins[1]->scope_uuid.value,
          "specifically, their lookup scopes differ: the parent schema is what separates "
          "two identical labels in one language");
    Check(twins[0]->normalized_lookup_key == twins[1]->normalized_lookup_key,
          "their normalized lookup keys are identical, so nothing about the text "
          "disambiguated them");
  }

  std::printf("PASS generated definition creation checks=%d\n", checks);
  return 0;
}
