// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "../support/uuid_index_key_oracle.hpp"
#include "crud_support/bound_ordered_index_key.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace key = api::bound_index_key;
unsigned checks = 0;
void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) throw std::runtime_error(reason);
}
api::EngineDescriptor Source(const char* name, std::string metadata) {
  const auto type=dt::CanonicalTypeIdFromStableName(name);
  for(const auto& candidate:dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row=candidate.legacy_fields;
    if(row.catalog_snapshot_uuid!=dt::kDatatypeCohortV5 ||
       row.canonical_binary_type_code!=static_cast<std::uint32_t>(type))continue;
    api::EngineDescriptor source;
    source.descriptor_uuid=source.datatype_descriptor_uuid=row.descriptor_uuid;
    source.datatype_descriptor_generation=row.descriptor_generation;source.type_uuid=row.type_uuid;
    source.datatype_cohort={row.catalog_snapshot_uuid,row.catalog_generation,row.registry_generation};
    source.descriptor_kind="executor.scalar";source.canonical_type_name=name;
    source.encoded_descriptor=std::move(metadata);
    return source;
  }
  throw std::runtime_error("explicit D705 fixture row missing");
}
key::OrderedIndexColumn Binding(const char* name) {
  const auto source = Source(name, "nullability=nullable");
  key::OrderedIndexColumn binding;
  Check(dt::LookupDatatypeStorageIdentityV3(source.datatype_cohort.catalog_snapshot_uuid,
      source.datatype_cohort.catalog_generation, source.datatype_cohort.registry_generation,
      source.datatype_descriptor_uuid, source.datatype_descriptor_generation, &binding.datatype),
      "exact index fixture storage identity unavailable");
  binding.descriptor = {scratchbird::core::platform::UuidKind::object, source.datatype_descriptor_uuid};
  std::string detail;
  Check(key::BuildOrderedColumnExecutionDescriptor(source, binding.datatype, true,
      &binding.execution_descriptor, &detail), "exact index execution descriptor unavailable");
  return binding;
}
std::string Encode(const key::OrderedIndexColumn& binding, const api::CrudStoredValue& value) {
  std::string output;
  bool null_key = false;
  api::EngineApiDiagnostic diagnostic;
  const bool ok = key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}),
      {binding}, &output, &null_key, &diagnostic);
  if (!ok) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Check(ok && null_key == value.isSqlNull(), "bound ordered key refused or changed state");
  return output;
}
std::string Integer(std::int64_t value) {
  std::string bytes(8, '\0');
  for (unsigned byte = 0; byte < 8; ++byte)
    bytes[byte] = static_cast<char>(static_cast<std::uint64_t>(value) >> (8 * byte));
  return bytes;
}
void Refusals(const char* name, std::string bytes) {
  const auto bound = Binding(name);
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    auto changed = bound;
    switch (mutation) {
      case 0: changed.execution_descriptor = {}; break;
      case 1: ++changed.execution_descriptor.descriptor_epoch; break;
      case 2: changed.execution_descriptor.descriptor_uuid.bytes[0] ^= 1; break;
      case 3: changed.execution_descriptor.canonical_type_id = 0; break;
      case 4: changed.execution_descriptor.bit_width = 1; break;
      case 5: changed.execution_descriptor.precision = 1; break;
      case 6: changed.execution_descriptor.modifier_flags = 1; break;
      case 7: changed.execution_descriptor.domain_stack.push_back(changed.execution_descriptor.descriptor_uuid); break;
      case 8: changed.descriptor.value.bytes[0] ^= 1; break;
    }
    for (const auto& value : {api::CrudStoredValue{bytes}, api::CrudStoredValue::SqlNull()}) {
      std::string output = "unchanged";
      bool is_null = true;
      api::EngineApiDiagnostic diagnostic;
      Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}), {changed},
            &output, &is_null, &diagnostic) && output == "unchanged" && is_null && diagnostic.error,
            "invalid index execution binding published a key or changed outputs");
    }
  }
  auto nonnullable = bound;
  nonnullable.execution_descriptor.nullable_allowed = false;
  Check(Encode(nonnullable, bytes) == Encode(bound, bytes), "slot nullability changed PRESENT key");
  std::string output = "unchanged";
  bool is_null = false;
  api::EngineApiDiagnostic diagnostic;
  Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({api::CrudStoredValue::SqlNull()}),
      {nonnullable}, &output, &is_null, &diagnostic) && output == "unchanged" && !is_null,
      "nonnullable index admitted SQL NULL");
  for (const auto& wrong : {bytes.substr(1), bytes + '\0', std::string{"17"}})
    Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({wrong}), {bound},
        &output, &is_null, &diagnostic) && output == "unchanged" && !is_null,
        "index guessed an invalid or text carrier");
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    auto source = Source(name, "nullability=nullable");
    switch (mutation) {
      case 0: source.encoded_descriptor += ";precision=1"; break;
      case 1: source.encoded_descriptor += ";unknown=1"; break;
      case 2: source.encoded_descriptor += ";nullable=true"; break;
      case 3: source.encoded_descriptor = "not_null=true"; break;
      case 4: ++source.datatype_descriptor_generation; break;
      case 5: source.encoded_descriptor = "nullability=non_null"; break;
      case 6: source.charset_uuid = scratchbird::tests::FixtureUuid(1401, 123); break;
    }
    auto destination = bound.execution_descriptor;
    std::string detail;
    Check(!key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, true,
          &destination, &detail) && !detail.empty() &&
          destination.descriptor_epoch == bound.execution_descriptor.descriptor_epoch &&
          destination.nullable_allowed, "malformed column projection accepted or changed output");
  }
  auto source = Source(name, "canonical=" + std::string(name));
  scratchbird::engine::ExecutionTypeDescriptor projected;
  std::string detail;
  Check(key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, false, &projected, &detail) &&
        !projected.nullable_allowed, "typed storage nullability was replaced by a default");
  for (const auto label : {"canonical", "canonical_type", "type"}) {
    source.encoded_descriptor = std::string(label) + "=" + name;
    Check(key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, false,
          &projected, &detail) && !projected.nullable_allowed,
          "matching declaration label was refused");
    source.encoded_descriptor += ";nullable=false";
    const auto expected = projected;
    source.encoded_descriptor.replace(source.encoded_descriptor.find('=') + 1,
                                     std::string(name).size(), "different");
    Check(!key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, false,
          &projected, &detail) && std::equal(std::begin(projected.descriptor_uuid.bytes),
              std::end(projected.descriptor_uuid.bytes), std::begin(expected.descriptor_uuid.bytes)) &&
          projected.nullable_allowed == expected.nullable_allowed,
          "contradictory declaration label was accepted or changed output");
  }
  const auto& codec = bound.datatype.codec->legacy_fields;
  api::CatalogColumnMetadata fields;
  fields.identities = {{"type_uuid", bound.datatype.type_uuid},
      {"datatype_descriptor_uuid", bound.datatype.descriptor_uuid}, {"codec_uuid", codec.codec_uuid}};
  fields.text = {{"nullable", "true"}, {"datatype_descriptor_generation", std::to_string(codec.descriptor_generation)},
      {"type_generation", std::to_string(codec.type_generation)}, {"codec_id", codec.codec_id},
      {"codec_version", std::to_string(codec.codec_version)}, {"codec_generation", std::to_string(codec.codec_generation)},
      {"null_encoding", std::to_string(codec.null_encoding_code)}};
  Check(api::EncodeCatalogColumnMetadata(fields, &source.encoded_descriptor) &&
        key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, true, &projected, &detail),
        "exact DDL registry metadata was refused");
  for (const auto& [name, identity] : fields.identities) {
    auto changed = fields;
    changed.identities[name] = scratchbird::tests::FixtureUuid(1401, 999);
    Check(api::EncodeCatalogColumnMetadata(changed, &source.encoded_descriptor) &&
          !key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, true, &projected, &detail),
          "substituted DDL binary metadata identity was accepted");
  }
  for (const auto& [name, value] : fields.text) {
    auto changed = fields;
    changed.text[name] += "x";
    Check(api::EncodeCatalogColumnMetadata(changed, &source.encoded_descriptor) &&
          !key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, true, &projected, &detail),
          "substituted DDL codec metadata was accepted");
  }
}
void BooleanColumnProjection() {
  for (const unsigned generation : {5u, 10u, 11u}) {
    auto source = Source("boolean", "nullable=true");
    source.datatype_cohort = {
        generation == 5 ? dt::kDatatypeCohortV5 :
        generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11,
        generation, generation};
    key::OrderedIndexColumn binding;
    Check(dt::LookupDatatypeStorageIdentityV3(source.datatype_cohort.catalog_snapshot_uuid,
        generation, generation, source.datatype_descriptor_uuid,
        source.datatype_descriptor_generation, &binding.datatype),
        "BOOLEAN retained cohort storage identity unavailable");
    binding.descriptor = {scratchbird::core::platform::UuidKind::object,
                          source.datatype_descriptor_uuid};
    std::string detail;
    Check(key::BuildOrderedColumnExecutionDescriptor(source, binding.datatype, true,
        &binding.execution_descriptor, &detail), "short BOOLEAN column profile refused");
    const auto& row = binding.datatype.codec->legacy_fields;
    api::CatalogColumnMetadata fields;
    fields.identities = {{"type_uuid", row.type_uuid},
        {"datatype_descriptor_uuid", row.descriptor_uuid}, {"codec_uuid", row.codec_uuid}};
    fields.text = {{"nullable", "true"},
        {"datatype_descriptor_generation", std::to_string(row.descriptor_generation)},
        {"type_generation", std::to_string(row.type_generation)}, {"codec_id", row.codec_id},
        {"codec_version", std::to_string(row.codec_version)},
        {"codec_generation", std::to_string(row.codec_generation)},
        {"null_encoding", std::to_string(row.null_encoding_code)}};
    Check(api::EncodeCatalogColumnMetadata(fields, &source.encoded_descriptor),
          "BOOLEAN binary column metadata did not encode");
    // Catalog declarations are not runtime modifier authority. The runtime
    // receiver stays strict; the owning column boundary validates/project them.
    scratchbird::engine::ExecutionTypeDescriptor projected;
    Check(!exec::BuildBoundExecutionTypeDescriptor(source, dt::CanonicalTypeId::boolean,
          &projected, &detail), "runtime receiver admitted unprojected BOOLEAN declaration");
    if (!key::BuildOrderedColumnExecutionDescriptor(source, binding.datatype, true,
          &projected, &detail)) {
      std::cerr << "BOOLEAN projection: " << detail << '\n';
      Check(false, "exact binary BOOLEAN column codec metadata refused");
    }
    auto actual = binding;
    actual.execution_descriptor = projected;
    const std::string false_bytes(1, '\0'), true_bytes(1, '\1');
    for (const auto& value : {api::CrudStoredValue{false_bytes},
                              api::CrudStoredValue{true_bytes}, api::CrudStoredValue::SqlNull()})
      Check(Encode(actual, value) == Encode(binding, value),
            "projected BOOLEAN key differs from exact short profile");
    Check(Encode(actual, false_bytes) < Encode(actual, true_bytes) &&
          Encode(actual, api::CrudStoredValue::SqlNull()) < Encode(actual, false_bytes),
          "BOOLEAN NULL/FALSE/TRUE ordering changed");
    auto non_null_fields = fields;
    non_null_fields.text["nullable"] = "false";
    auto non_null_source = source;
    Check(api::EncodeCatalogColumnMetadata(non_null_fields, &non_null_source.encoded_descriptor) &&
          key::BuildOrderedColumnExecutionDescriptor(non_null_source, binding.datatype, false,
            &projected, &detail) && !projected.nullable_allowed,
          "exact nonnullable BOOLEAN column refused or widened");
    auto non_null_binding = binding;
    non_null_binding.execution_descriptor = projected;
    for (const auto& bytes : {false_bytes, true_bytes})
      Check(Encode(non_null_binding, bytes) == Encode(binding, bytes),
            "BOOLEAN nullability changed PRESENT key bytes");
    std::string null_output = "unchanged";
    bool null_flag = false;
    api::EngineApiDiagnostic null_diagnostic;
    Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({api::CrudStoredValue::SqlNull()}),
          {non_null_binding}, &null_output, &null_flag, &null_diagnostic) &&
          null_output == "unchanged" && !null_flag && null_diagnostic.error,
          "nonnullable BOOLEAN index admitted NULL or published a partial result");
    auto relabeled = binding.datatype;
    relabeled.codec->legacy_fields.canonical_name = "localized label";
    relabeled.codec->legacy_fields.codec_id = "localized codec label";
    Check(key::BuildOrderedColumnExecutionDescriptor(source, relabeled, true,
          &projected, &detail), "presentation labels replaced BOOLEAN binding authority");
    const auto original = source.encoded_descriptor;
    const auto refuse = [&](const api::EngineDescriptor& invalid, bool nullable = true) {
      auto destination = binding.execution_descriptor;
      Check(!key::BuildOrderedColumnExecutionDescriptor(invalid, binding.datatype, nullable,
            &destination, &detail) && !detail.empty() &&
            destination.descriptor_epoch == binding.execution_descriptor.descriptor_epoch &&
            destination.nullable_allowed == binding.execution_descriptor.nullable_allowed &&
            destination.canonical_type_id == binding.execution_descriptor.canonical_type_id &&
            destination.bit_width == binding.execution_descriptor.bit_width,
            "invalid BOOLEAN column metadata admitted or changed caller output");
    };
    const auto refuse_fields = [&](const api::CatalogColumnMetadata& invalid) {
      auto candidate = source;
      Check(api::EncodeCatalogColumnMetadata(invalid, &candidate.encoded_descriptor),
            "BOOLEAN negative binary metadata fixture invalid");
      refuse(candidate);
    };
    for (const auto& [name, id] : fields.identities) {
      for (unsigned byte = 0; byte < 16; ++byte) {
        auto invalid = fields;
        // Retained rows can have an absent codec UUID. Substitute a real v7
        // identity so the negative reaches the column boundary, not merely
        // the binary framing codec's invalid-UUID guard.
        if (id.is_nil())
          invalid.identities[name] = scratchbird::tests::FixtureUuid(1401, 999);
        invalid.identities[name].bytes[byte] ^= 1;
        refuse_fields(invalid);
      }
    }
    for (const auto name : {"charset_uuid", "collation_uuid", "timezone_uuid",
                            "domain_uuid", "security_policy_uuid", "unknown_uuid"}) {
      auto invalid = fields;
      invalid.identities[name] = source.type_uuid;
      refuse_fields(invalid);
    }
    for (const auto& [name, value] : fields.text) {
      if (name == "nullable") continue;
      auto invalid = fields;
      invalid.text.erase(name);
      refuse_fields(invalid);
      invalid = fields;
      invalid.text[name] += "x";
      refuse_fields(invalid);
    }
    for (const auto name : {"unknown", "precision", "scale", "length", "bit_profile_kind"}) {
      auto invalid = fields;
      invalid.text[name] = "1";
      refuse_fields(invalid);
    }
    auto declared = fields;
    declared.text.insert({{"canonical", "boolean"}, {"primary_key", "false"},
                          {"default", "false"}});
    auto declaration = source;
    Check(api::EncodeCatalogColumnMetadata(declared, &declaration.encoded_descriptor) &&
          key::BuildOrderedColumnExecutionDescriptor(declaration, binding.datatype, true,
            &projected, &detail), "validated BOOLEAN declaration attributes were refused");
    actual.execution_descriptor = projected;
    Check(Encode(actual, true_bytes) == Encode(binding, true_bytes),
          "BOOLEAN declaration attributes changed comparison bytes");
    declared.text["canonical"] = "int64";
    refuse_fields(declared);
    auto invalid = fields;
    invalid.text["nullability"] = "nullable";
    refuse_fields(invalid);
    refuse(source, false);
    auto changed = source;
    ++changed.datatype_descriptor_generation;
    refuse(changed);
    changed = source;
    ++changed.datatype_cohort.registry_generation;
    refuse(changed);
    changed = source;
    changed.charset_uuid = source.type_uuid;
    refuse(changed);
    changed = source;
    changed.collation_uuid = source.type_uuid;
    refuse(changed);
    changed = source;
    changed.encoded_descriptor = "nullable=true;type_uuid=text-is-not-an-engine-identity";
    refuse(changed);
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
      auto altered = binding.datatype;
      switch (mutation) {
        case 0: altered.codec.reset(); break;
        case 1: altered.codec->legacy_fields.codec_uuid.bytes[0] ^= 1; break;
        case 2: altered.codec->descriptor_policy.uuid.bytes[0] ^= 1; break;
        case 3: ++altered.codec->legacy_fields.catalog_generation; break;
        case 4: ++altered.codec->native_fields.canonical_value_transport_width; break;
        case 5: altered.codec->legacy_fields.canonical_binary_type_code =
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int64); break;
      }
      auto destination = binding.execution_descriptor;
      Check(!key::BuildOrderedColumnExecutionDescriptor(source, altered, true,
            &destination, &detail) && !detail.empty() &&
            destination.descriptor_epoch == binding.execution_descriptor.descriptor_epoch,
            "BOOLEAN projection trusted caller-substituted codec authority");
    }
    for (const auto& bytes : {std::string{}, std::string(2, '\0'), std::string(1, '\2'),
                              std::string{"false"}, std::string{"0"}}) {
      std::string output = "unchanged";
      bool null_key = true;
      api::EngineApiDiagnostic diagnostic;
      Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({api::CrudStoredValue{bytes}}),
            {binding}, &output, &null_key, &diagnostic) && output == "unchanged" &&
            null_key && diagnostic.error,
            "BOOLEAN index admitted malformed/text native bytes or published a prefix");
    }
    Check(source.encoded_descriptor == original,
          "BOOLEAN projection rewrote persisted metadata instead of staging a projection");
  }
}
void PolicyBearingBinding() {
  auto binding = Binding("int64");
  Check(dt::LookupDatatypeStorageIdentityV3(dt::kDatatypeCohortV10, 10, 10,
        binding.datatype.descriptor_uuid, binding.datatype.descriptor_generation,
        &binding.datatype), "current V3 storage identity unavailable");
  const auto row = dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV10,
      10, 10, binding.datatype.descriptor_uuid, binding.datatype.descriptor_generation);
  Check(row.ok && binding.datatype.codec && *binding.datatype.codec == row.row,
        "current storage binding discarded V3 authority fields");
  Check(Encode(binding, Integer(-1)) < Encode(binding, Integer(1)),
        "current policy-bearing binding lost native integer ordering");
  auto relabeled = binding;
  relabeled.datatype.codec->legacy_fields.canonical_name = "localized name";
  relabeled.datatype.codec->legacy_fields.codec_id = "localized codec label";
  Check(Encode(relabeled, Integer(1)) == Encode(binding, Integer(1)),
        "presentation labels changed native identity or ordering");
  for (unsigned mutation = 0; mutation < 19; ++mutation) {
    auto changed = binding;
    auto& codec = *changed.datatype.codec;
    switch (mutation) {
      case 0: codec.descriptor_policy.uuid.bytes[0] ^= 1; break;
      case 1: ++codec.descriptor_policy.generation; break;
      case 2: codec.canonicalization_policy.uuid.bytes[0] ^= 1; break;
      case 3: ++codec.canonicalization_policy.generation; break;
      case 4: codec.ordering_policy.uuid.bytes[0] ^= 1; break;
      case 5: ++codec.ordering_policy.generation; break;
      case 6: codec.hash_policy.uuid.bytes[0] ^= 1; break;
      case 7: ++codec.hash_policy.generation; break;
      case 8: codec.operation_policy.uuid.bytes[0] ^= 1; break;
      case 9: ++codec.operation_policy.generation; break;
      case 10: codec.native_fields.present = !codec.native_fields.present; break;
      case 11: ++codec.native_fields.canonical_value_minimum_bytes; break;
      case 12: ++codec.native_fields.canonical_value_maximum_bytes; break;
      case 13: ++codec.native_fields.canonical_value_transport_width; break;
      case 14: codec.native_fields.canonical_value_variable_width =
          !codec.native_fields.canonical_value_variable_width; break;
      case 15: codec.native_fields.policy_profile_uuid.bytes[0] ^= 1; break;
      case 16: ++codec.native_fields.policy_profile_generation; break;
      case 17: codec.native_fields.profile_fingerprint_sha256[0] ^= 1; break;
      case 18: changed.datatype.codec.reset(); break;
    }
    for (const auto& value : {api::CrudStoredValue{Integer(1)}, api::CrudStoredValue::SqlNull()}) {
      std::string output = "unchanged";
      bool is_null = false;
      api::EngineApiDiagnostic diagnostic;
      Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}), {changed},
            &output, &is_null, &diagnostic) && output == "unchanged" && !is_null && diagnostic.error,
            "altered V3 authority published a key or changed outputs");
    }
  }
}
void DateBoundKeys() {
  struct Cohort {
    scratchbird::core::platform::Uuid snapshot;
    std::uint64_t generation;
    std::string_view fingerprint;
  };
  const Cohort cohorts[] = {
      {dt::kDatatypeCohortV7, 7, "a87f551f3a950b377a854d3405bdc3b177c5187f944b4f9d6671f2ce02a4e6f8"},
      {dt::kDatatypeCohortV8, 8, "7d9471b28457df475b5aa182f5a17ed7a53e3a52f79953c51a9d969bdfd41d11"},
      {dt::kDatatypeCohortV9, 9, "da43f74cbf040c5cc734f83d1fa460886c14e5ab0eaf9888698d185d4e6ffac9"},
      {dt::kDatatypeCohortV10, 10, "ad456dafc3671a8f3122352c4da98586f6b7684ce26441ffc5c6cc10809a4237"},
      {dt::kDatatypeCohortV11, 11, "0d747769d2324170e000cb1973f8b1f0282990343cd308da83804a641b9c5796"}};
  for (const auto& cohort : cohorts) {
  auto date = Binding("date");
  Check(!key::BindOrderedDateProfile(&date) && !date.date_profile,
        "historical storage-only DATE acquired current semantic authority");
  Check(dt::LookupDatatypeStorageIdentityV3(cohort.snapshot, cohort.generation, cohort.generation,
        date.datatype.descriptor_uuid, date.datatype.descriptor_generation, &date.datatype) &&
        key::BindOrderedDateProfile(&date), "exact retained DATE index profile did not bind");
  const auto component = [](std::int32_t day) { return Integer(day).substr(0, 4); };
  const std::int32_t days[] = {INT32_MIN, -65536, -1, 0, 1, 255, 256, INT32_MAX};
  for (const auto day : days) {
    // Independent Core SBDATK01 oracle, not another call to the DATE encoder.
    std::string raw = "SBDATK01";
    raw.append(reinterpret_cast<const char*>(cohort.snapshot.bytes.data()), 16);
    raw += Integer(cohort.generation);
    raw += Integer(cohort.generation);
    constexpr unsigned char ordering_policy[] = {
        0x01,0xa1,0x00,0x8e,0xb7,0xf2,0x7f,0xeb,0x85,0xa8,0x2d,0x74,0xfe,0x2f,0xde,0x38};
    const auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (std::size_t i = 0; i < cohort.fingerprint.size(); i += 2)
      raw.push_back(static_cast<char>(digit(cohort.fingerprint[i]) * 16 + digit(cohort.fingerprint[i+1])));
    raw.append(reinterpret_cast<const char*>(ordering_policy), sizeof(ordering_policy));
    raw += Integer(1);
    raw.append("\0\0\1\4", 4);
    const auto biased = static_cast<std::uint32_t>(day) ^ 0x80000000u;
    for (int shift = 24; shift >= 0; shift -= 8) raw.push_back(static_cast<char>(biased >> shift));
    scratchbird::core::index::IndexKeyEncodingComponent expected;
    expected.type_descriptor_uuid = date.descriptor;
    expected.type_descriptor_epoch = date.datatype.descriptor_generation;
    expected.null_placement = scratchbird::core::index::IndexKeyNullPlacement::nulls_first;
    expected.payload.assign(raw.begin(), raw.end());
    const auto encoded = scratchbird::core::index::EncodeIndexKey({expected}, {});
    Check(encoded.ok() && raw.size() == 104 &&
          Encode(date, component(day)) == std::string(encoded.encoded.begin(), encoded.encoded.end()),
          "DATE index did not retain the complete Core profile-bound key");
  }
  for (unsigned i = 1; i < std::size(days); ++i)
    Check(Encode(date, component(days[i-1])) < Encode(date, component(days[i])),
          "DATE ordered key differs from signed day ordering");
  Check(Encode(date, api::CrudStoredValue::SqlNull()) < Encode(date, component(INT32_MIN)),
        "DATE NULL does not precede minimum day");
  auto relabeled = date;
  relabeled.datatype.codec->legacy_fields.canonical_name = "localized DATE";
  relabeled.datatype.codec->legacy_fields.codec_id = "localized codec";
  Check(Encode(relabeled, component(1)) == Encode(date, component(1)),
        "DATE presentation labels changed canonical key");
  auto required = date;
  required.execution_descriptor.nullable_allowed = false;
  Check(Encode(required, component(1)) == Encode(date, component(1)),
        "DATE containing nullability changed a PRESENT key");
  auto refuses = [](const key::OrderedIndexColumn& binding, const api::CrudStoredValue& value) {
    std::string logical;
    bool framing_refused = false;
    try {
      logical = api::EncodeStoredLogicalKey({value});
    } catch (const std::invalid_argument&) {
      framing_refused = true;
    }
    const bool invalid_state = !value.valid() || (!value.isPresent() && !value.isSqlNull());
    Check(framing_refused == invalid_state, "logical key encoder changed state admission");
    if (framing_refused) {
      // Independently construct the malformed wire frame that the safe
      // encoder correctly refused, then also exercise the receiving gate.
      logical = "SBCLKEY2";
      api::AppendBinaryU32(&logical, 1);
      api::AppendBinaryU8(&logical, static_cast<std::uint8_t>(value.state));
      Check(api::AppendBinaryString(&logical, value.bytes), "malformed key fixture extent");
      Check(!api::DecodeStoredLogicalKey(logical, 1), "logical key decoder admitted invalid state");
    }
    std::string output = "unchanged";
    bool null_key = false;
    api::EngineApiDiagnostic diagnostic;
    Check(!key::EncodeOrderedIndexKey(logical, {binding},
          &output, &null_key, &diagnostic) && output == "unchanged" && !null_key && diagnostic.error,
          "invalid DATE index input changed output");
  };
  refuses(required, api::CrudStoredValue::SqlNull());
  refuses(date, {api::EngineValueState::sql_null, component(0)});
  refuses(date, api::CrudStoredValue::Missing());
  refuses(date, api::CrudStoredValue::DefaultRequested());
  for (const auto& invalid : {std::string{}, std::string(3, '\0'), std::string(5, '\0'),
                             std::string{"1970-01-01"}}) refuses(date, invalid);
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    auto changed = date;
    switch (mutation) {
      case 0: changed.date_profile.reset(); break;
      case 1: ++changed.date_profile->receipt.catalog_generation; break;
      case 2: changed.date_profile->comparison_fingerprint[0] ^= 1; break;
      case 3: ++changed.datatype.codec->ordering_policy.generation; break;
      case 4: changed.execution_descriptor.canonical_type_id = 0; break;
      case 5: changed.execution_descriptor.bit_width = 64; break;
      case 6: changed.execution_descriptor.precision = 1; break;
      case 7: changed.execution_descriptor.domain_stack.push_back(changed.execution_descriptor.descriptor_uuid); break;
      case 8: changed.execution_descriptor.modifier_flags = 1; break;
    }
    refuses(changed, component(1));
    refuses(changed, api::CrudStoredValue::SqlNull());
  }
  }
}
void TimeBoundKeys() {
  for (unsigned generation : {10u, 11u}) {
    const auto cohort = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
    auto time = Binding("time");
    Check(!key::BindOrderedTimeProfile(&time) && !time.time_profile,
          "historical storage-only TIME acquired semantic authority");
    Check(dt::LookupDatatypeStorageIdentityV3(cohort, generation, generation,
        time.datatype.descriptor_uuid, time.datatype.descriptor_generation, &time.datatype) &&
        key::BindOrderedTimeProfile(&time), "exact TIME index profile did not bind");
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
      auto source = Source("time", "nullability=nullable");
      source.datatype_cohort = {cohort, generation, generation};
      switch (mutation) {
        case 0: source.encoded_descriptor += ";precision=1"; break;
        case 1: source.encoded_descriptor += ";unknown=1"; break;
        case 2: source.encoded_descriptor += ";nullable=true"; break;
        case 3: source.charset_uuid = scratchbird::tests::FixtureUuid(1401, 456); break;
        case 4: source.collation_uuid = scratchbird::tests::FixtureUuid(1401, 457); break;
      }
      auto output = time.execution_descriptor; std::string detail;
      Check(!key::BuildOrderedColumnExecutionDescriptor(source, time.datatype, true, &output, &detail) &&
          !detail.empty() && output.precision == 0 &&
          std::equal(std::begin(output.charset_uuid.bytes), std::end(output.charset_uuid.bytes),
              std::begin(time.execution_descriptor.charset_uuid.bytes)),
          "TIME projection erased unsupported source metadata");
    }
    const std::uint64_t values[] = {0, 1, 255, 256, 65536, dt::kTimeMaximumNanosecondsV3};
    for (const auto value : values) {
      std::string raw = "SBTIMK01";
      raw.append(reinterpret_cast<const char*>(cohort.bytes.data()), 16);
      raw += Integer(generation); raw += Integer(generation);
      const std::string_view fingerprint = generation == 10 ?
          "75deaba796da7e2008600237a5ebfb0dd8a76f2b62ea664b66876ba47f05b3d6" :
          "1673475a46395937c1eef4e90bdc92e72eec21a47404394bfe272224cf983f44";
      const auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
      for (std::size_t i = 0; i < fingerprint.size(); i += 2)
        raw.push_back(static_cast<char>(digit(fingerprint[i]) * 16 + digit(fingerprint[i+1])));
      constexpr unsigned char ordering_policy[] = {
          0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x36};
      raw.append(reinterpret_cast<const char*>(ordering_policy), sizeof(ordering_policy));
      raw += Integer(1); raw.append("\0\0\1\10", 4);
      for (int shift = 56; shift >= 0; shift -= 8) raw.push_back(static_cast<char>(value >> shift));
      scratchbird::core::index::IndexKeyEncodingComponent expected;
      expected.type_descriptor_uuid = time.descriptor;
      expected.type_descriptor_epoch = time.datatype.descriptor_generation;
      expected.null_placement = scratchbird::core::index::IndexKeyNullPlacement::nulls_first;
      expected.payload.assign(raw.begin(), raw.end());
      const auto encoded = scratchbird::core::index::EncodeIndexKey({expected}, {});
      Check(encoded.ok() && raw.size() == 108 && Encode(time, Integer(value)) ==
          std::string(encoded.encoded.begin(), encoded.encoded.end()), "TIME key differs from independent binary oracle");
    }
    for (unsigned i = 1; i < std::size(values); ++i)
      Check(Encode(time, Integer(values[i-1])) < Encode(time, Integer(values[i])), "TIME unsigned order changed");
    Check(Encode(time, api::CrudStoredValue::SqlNull()) < Encode(time, Integer(0)), "TIME NULL aliases midnight");
    auto refuses = [](const key::OrderedIndexColumn& binding, const api::CrudStoredValue& value) {
      std::string output = "unchanged"; bool null_key = false; api::EngineApiDiagnostic diagnostic;
      Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}), {binding},
          &output, &null_key, &diagnostic) && output == "unchanged" && !null_key && diagnostic.error,
          "invalid TIME input published a key or changed output");
    };
    for (const auto& invalid : {std::string{}, std::string(7, '\0'), std::string(9, '\0'),
          std::string{"00:00:00"}, Integer(dt::kTimeMaximumNanosecondsV3 + 1), Integer(-1)}) refuses(time, invalid);
    auto required = time; required.execution_descriptor.nullable_allowed = false;
    Check(Encode(required, Integer(1)) == Encode(time, Integer(1)), "TIME nullability changed PRESENT key");
    refuses(required, api::CrudStoredValue::SqlNull());
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
      auto changed = time;
      switch (mutation) {
        case 0: changed.time_profile.reset(); break;
        case 1: ++changed.time_profile->receipt.catalog_generation; break;
        case 2: changed.time_profile->comparison_fingerprint[0] ^= 1; break;
        case 3: ++changed.datatype.codec->ordering_policy.generation; break;
        case 4: changed.execution_descriptor.canonical_type_id = 0; break;
        case 5: changed.execution_descriptor.bit_width = 32; break;
        case 6: changed.execution_descriptor.precision = 1; break;
        case 7: changed.execution_descriptor.domain_stack.push_back(changed.execution_descriptor.descriptor_uuid); break;
        case 8: changed.execution_descriptor.modifier_flags = 1; break;
        case 9: ++changed.datatype.codec->native_fields.canonical_value_transport_width; break;
        case 10: changed.datatype.codec.reset(); break;
        case 11: changed.descriptor.value.bytes[0] ^= 1; break;
      }
      refuses(changed, Integer(1)); refuses(changed, api::CrudStoredValue::SqlNull());
    }
    auto relabeled = time;
    relabeled.datatype.codec->legacy_fields.canonical_name = "localized TIME";
    relabeled.datatype.codec->legacy_fields.codec_id = "localized codec";
    Check(Encode(relabeled, Integer(1)) == Encode(time, Integer(1)), "TIME labels changed key authority");
  }
}
void TimestampBoundKeys() {
  for (unsigned generation : {10u, 11u}) {
    const auto cohort = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
    auto timestamp = Binding("timestamp");
    Check(!key::BindOrderedTimestampProfile(&timestamp) && !timestamp.timestamp_profile,
          "historical UTC acquired local-civil authority");
    Check(dt::LookupDatatypeStorageIdentityV3(cohort, generation, generation,
        timestamp.datatype.descriptor_uuid, timestamp.datatype.descriptor_generation, &timestamp.datatype) &&
        key::BindOrderedTimestampProfile(&timestamp), "exact civil TIMESTAMP profile did not bind");
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
      auto source = Source("timestamp", "nullability=nullable");
      source.datatype_cohort = {cohort, generation, generation};
      switch (mutation) {
        case 0: source.encoded_descriptor += ";precision=1"; break;
        case 1: source.encoded_descriptor += ";unknown=1"; break;
        case 2: source.encoded_descriptor += ";nullable=true"; break;
        case 3: source.charset_uuid = scratchbird::tests::FixtureUuid(1401, 456); break;
        case 4: source.collation_uuid = scratchbird::tests::FixtureUuid(1401, 457); break;
        case 5: source.encoded_descriptor += ";timestamp_timezone_profile=utc"; break;
      }
      auto output = timestamp.execution_descriptor; std::string detail;
      Check(!key::BuildOrderedColumnExecutionDescriptor(source, timestamp.datatype, true, &output, &detail) &&
          !detail.empty() && output.precision == 0, "TIMESTAMP unsupported metadata erased");
    }
    const auto component = [](std::int64_t seconds, std::uint32_t fraction) {
      std::string bytes = Integer(seconds);
      for (unsigned n = 0; n < 4; ++n) bytes.push_back(static_cast<char>(fraction >> (8*n)));
      bytes.append(4, '\0');
      return bytes;
    };
    const std::pair<std::int64_t, std::uint32_t> values[] = {
        {dt::kTimestampMinimumCivilSecondV3, 0}, {-257, 0}, {-1, 999999999},
        {0, 0}, {0, 1}, {255, 0}, {256, 0}, {dt::kTimestampMaximumCivilSecondV3, 999999999}};
    std::string prior;
    for (const auto [seconds, fraction] : values) {
      std::string raw = "SBTSPK01";
      raw.append(reinterpret_cast<const char*>(cohort.bytes.data()), 16);
      raw += Integer(generation); raw += Integer(generation);
      const auto append_hex = [&](std::string_view hex) {
        const auto digit = [](char ch){return ch <= '9' ? ch-'0' : ch-'a'+10;};
        for (std::size_t n = 0; n < hex.size(); n += 2)
          raw.push_back(static_cast<char>(digit(hex[n])*16+digit(hex[n+1])));
      };
      append_hex(generation == 10 ? "35ce26afa08d48af46c79e08cd056a453b7deba80d3569e78ffdba5b1c93fee7" :
          "879004814d3fc6583cc0de3263cd72da1d97e15bb06d942609056f58ca9c2da4");
      append_hex("01a104ec8e3c7dff8e89868ecc2a8a0c");
      raw += Integer(1); raw.append("\0\0\1\14", 4);
      const auto sortable = static_cast<std::uint64_t>(seconds) ^ 0x8000000000000000ull;
      for (int shift = 56; shift >= 0; shift -= 8) raw.push_back(static_cast<char>(sortable >> shift));
      for (int shift = 24; shift >= 0; shift -= 8) raw.push_back(static_cast<char>(fraction >> shift));
      scratchbird::core::index::IndexKeyEncodingComponent expected;
      expected.type_descriptor_uuid = timestamp.descriptor;
      expected.type_descriptor_epoch = timestamp.datatype.descriptor_generation;
      expected.null_placement = scratchbird::core::index::IndexKeyNullPlacement::nulls_first;
      expected.payload.assign(raw.begin(), raw.end());
      const auto oracle = scratchbird::core::index::EncodeIndexKey({expected}, {});
      const auto actual = Encode(timestamp, component(seconds, fraction));
      Check(oracle.ok() && actual == std::string(oracle.encoded.begin(), oracle.encoded.end()),
            "TIMESTAMP independent native binary key oracle");
      Check(prior.empty() || prior < actual, "civil signed-second/unsigned-fraction ordering");
      prior = actual;
    }
    Check(Encode(timestamp, api::CrudStoredValue::SqlNull()) < Encode(timestamp, component(0, 0)),
          "TIMESTAMP NULL does not alias epoch");
    auto refuses = [](const auto& binding, const api::CrudStoredValue& value) {
      std::string output = "unchanged"; bool null_key = false; api::EngineApiDiagnostic diagnostic;
      Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}), {binding},
          &output, &null_key, &diagnostic) && output == "unchanged" && !null_key && diagnostic.error,
          "invalid TIMESTAMP binding or value changed outputs");
    };
    for (auto bytes : {std::string{}, std::string(15, '\0'), std::string(17, '\0'),
        std::string{"1970-01-01T00:00:00"}, component(0, 1000000000),
        component(dt::kTimestampMinimumCivilSecondV3-1, 0), component(dt::kTimestampMaximumCivilSecondV3+1, 0)})
      refuses(timestamp, bytes);
    for (unsigned n = 12; n < 16; ++n) {
      auto reserved = component(0, 0); reserved[n] = 1; refuses(timestamp, reserved);
    }
    auto required = timestamp; required.execution_descriptor.nullable_allowed = false;
    refuses(required, api::CrudStoredValue::SqlNull());
    Check(Encode(required, component(0, 1)) == Encode(timestamp, component(0, 1)), "nullability changed PRESENT key");
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
      auto changed = timestamp;
      switch (mutation) {
        case 0: changed.timestamp_profile.reset(); break;
        case 1: ++changed.timestamp_profile->receipt.catalog_generation; break;
        case 2: changed.timestamp_profile->comparison_fingerprint[0] ^= 1; break;
        case 3: ++changed.datatype.codec->ordering_policy.generation; break;
        case 4: changed.execution_descriptor.canonical_type_id = 0; break;
        case 5: changed.execution_descriptor.bit_width = 64; break;
        case 6: changed.execution_descriptor.precision = 1; break;
        case 7: changed.execution_descriptor.domain_stack.push_back(changed.execution_descriptor.descriptor_uuid); break;
        case 8: changed.execution_descriptor.modifier_flags = 1; break;
        case 9: ++changed.datatype.codec->native_fields.canonical_value_transport_width; break;
        case 10: changed.datatype.codec.reset(); break;
        case 11: changed.descriptor.value.bytes[0] ^= 1; break;
        case 12: changed.execution_descriptor.timezone_uuid.bytes[0] = 1; break;
      }
      refuses(changed, component(0, 1)); refuses(changed, api::CrudStoredValue::SqlNull());
    }
  }
}
void BitBoundKeys() {
  for(const unsigned generation:{10u,11u})for(unsigned kind=0;kind<3;++kind) {
    const auto cohort=generation==10?dt::kDatatypeCohortV10:dt::kDatatypeCohortV11;
    api::EngineDescriptor source;
    key::OrderedIndexColumn binding;
    for(const auto& candidate:dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
      const auto& row=candidate.legacy_fields;
      if(row.catalog_snapshot_uuid!=cohort||row.canonical_binary_type_code!=302)continue;
      source.descriptor_uuid=source.datatype_descriptor_uuid=row.descriptor_uuid;
      source.datatype_descriptor_generation=row.descriptor_generation;source.type_uuid=row.type_uuid;
      source.datatype_cohort={cohort,generation,generation};source.descriptor_kind="scalar";
      source.canonical_type_name="bit_string";
      source.encoded_descriptor="nullability=nullable;bit_profile_kind="+std::to_string(kind)+(kind?";length=9":"");
      binding.datatype={row.descriptor_uuid,row.descriptor_generation,row.type_uuid,dt::CanonicalTypeId::bit_string,candidate};
      binding.descriptor={scratchbird::core::platform::UuidKind::object,row.descriptor_uuid};
    }
    key::BoundBitStringProfile profile;
    api::EngineApiDiagnostic diagnostic;
    Check(key::BuildBoundBitStringProfile(source,binding.datatype,true,&profile,&diagnostic),"BIT ordered profile failed");
    binding.execution_descriptor=profile.execution;binding.bit_profile=profile.profile;binding.bit_profile_kind=profile.kind;
    const auto component=[](std::string_view bits) {
      std::string bytes(4+(bits.size()+7)/8,'\0');
      for(unsigned i=0;i<4;++i)bytes[i]=static_cast<char>(bits.size()>>(8*i));
      for(std::size_t i=0;i<bits.size();++i)if(bits[i]=='1')bytes[4+i/8]|=static_cast<char>(0x80>>(i%8));
      return api::CrudStoredValue(bytes);
    };
    const std::vector<std::string> ordered=kind==1
        ?std::vector<std::string>{"000000000","000000001","000000010","010000000","100000000","111111111"}
        :std::vector<std::string>{"","0","00","000","001","01","1","10","100","101","11","111111111"};
    std::string previous=Encode(binding,api::CrudStoredValue::SqlNull());
    for(const auto& bits:ordered) {
      const auto actual=Encode(binding,component(bits));
      Check(previous<actual,"BIT prefix/logical order changed");previous=actual;
      // Independent SBBITK01 literal, then the separately tested outer encoder.
      std::string raw="SBBITK01";
      raw.append(reinterpret_cast<const char*>(cohort.bytes.data()),16);
      raw+=Integer(generation);raw+=Integer(generation);
      constexpr std::string_view fingerprints[2][3]={
          {"e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c89",
           "e13e96cf92a0fea40f2d4cd716f5a3e060660b5514dcf4039d631ff77a7be18b",
           "1f8a8d9445aa82dbf108a1a6795729fb346c65930c6be7a23ca851466b55c39c"},
          {"903ff2109fa40d6fb97f5513b9e13713a28b56ef2d772e53a0d0bbd9e436a4aa",
           "b122f87d0c3b7d0e20b2438e10f8152004ed521470ab2e0280f9c447bd390ed7",
           "ace42fab9cd71093535f706a2108f46301d46b8eeaaa556b6c627e7904710cce"}};
      const auto fingerprint=fingerprints[generation-10][kind];
      const auto hex=[](char c){return c<='9'?c-'0':c-'a'+10;};
      for(std::size_t i=0;i<fingerprint.size();i+=2)raw.push_back(static_cast<char>(hex(fingerprint[i])*16+hex(fingerprint[i+1])));
      constexpr unsigned char policy[]={0x01,0xa0,0xff,0x27,0x27,0x17,0x7a,0x54,0xbd,0xbc,0xa3,0xc1,0xfe,0xe7,0xc5,0xe3};
      raw.append(reinterpret_cast<const char*>(policy),16);raw+=Integer(1);raw.append("\0\0\1\0\0\0\0\0",8);
      for(const char bit:bits)raw.push_back(bit=='1'?2:1);
      raw.push_back(0);
      scratchbird::core::index::IndexKeyEncodingComponent expected;
      expected.type_descriptor_uuid=binding.descriptor;expected.type_descriptor_epoch=binding.datatype.descriptor_generation;
      expected.null_placement=scratchbird::core::index::IndexKeyNullPlacement::nulls_first;
      expected.payload.assign(raw.begin(),raw.end());
      const auto encoded=scratchbird::core::index::EncodeIndexKey({expected},{});
      if(encoded.ok()&&actual!=std::string(encoded.encoded.begin(),encoded.encoded.end())) {
        std::size_t at=0;
        while(at<actual.size()&&at<encoded.encoded.size()&&static_cast<unsigned char>(actual[at])==encoded.encoded[at])++at;
        std::cerr<<"BIT oracle generation="<<generation<<" kind="<<kind<<" bits="<<bits.size()<<" mismatch="<<at
                 <<" sizes="<<actual.size()<<','<<encoded.encoded.size()<<'\n';
      }
      Check(encoded.ok()&&actual==std::string(encoded.encoded.begin(),encoded.encoded.end()),"BIT key differs from literal binary oracle");
    }
    const auto refuses=[&](const key::OrderedIndexColumn& changed,const api::CrudStoredValue& value) {
      std::string output="unchanged";bool null_key=false;
      Check(!key::EncodeOrderedIndexKey(api::EncodeStoredLogicalKey({value}),{changed},&output,&null_key,&diagnostic)&&
            output=="unchanged"&&!null_key&&diagnostic.error,"invalid BIT index binding published a key");
    };
    for(unsigned mutation=0;mutation<14;++mutation) {
      auto changed=binding;
      switch(mutation) {
        case 0: changed.bit_profile.reset();break;
        case 1: changed.bit_profile->profile_fingerprint[0]^=1;break;
        case 2: ++changed.bit_profile->length_bits;break;
        case 3: changed.bit_profile_kind=static_cast<dt::BitStringSurfaceProfileKindV3>(3);break;
        case 4: ++changed.execution_descriptor.length;break;
        case 5: ++changed.execution_descriptor.descriptor_epoch;break;
        case 6: ++changed.datatype.codec->native_fields.canonical_value_maximum_bytes;break;
        case 7: changed.datatype.codec.reset();break;
        case 8: changed.bit_profile->receipt.catalog_generation++;break;
        case 9: changed.bit_profile->fixed_length=!changed.bit_profile->fixed_length;break;
        case 10: changed.execution_descriptor.charset_uuid.bytes[0]=1;break;
        case 11: changed.execution_descriptor.domain_stack.push_back(changed.execution_descriptor.descriptor_uuid);break;
        case 12: changed.collation=changed.descriptor;break;
        case 13: changed.collation.value.bytes[0]=1;break;
      }
      refuses(changed,component("100000001"));refuses(changed,api::CrudStoredValue::SqlNull());
    }
    auto dirty=component("100000001");dirty.bytes.back()|=1;refuses(binding,dirty);
    auto nonnullable=binding;nonnullable.execution_descriptor.nullable_allowed=false;
    refuses(nonnullable,api::CrudStoredValue::SqlNull());
    Check(Encode(nonnullable,component("100000001"))==Encode(binding,component("100000001")),"BIT nullability changed PRESENT key");
  }
}

int main() {
  BooleanColumnProjection();
  BitBoundKeys();
  TimestampBoundKeys();
  TimeBoundKeys();
  DateBoundKeys();
  PolicyBearingBinding();
  const auto integer = Binding("int64");
  const std::int64_t ordered[] = {std::numeric_limits<std::int64_t>::min(), -257, -1,
      0, 2, 256, std::numeric_limits<std::int64_t>::max()};
  for (unsigned i = 1; i < std::size(ordered); ++i)
    Check(Encode(integer, Integer(ordered[i-1])) < Encode(integer, Integer(ordered[i])),
          "integer key ordering differs from independent signed oracle");
  Check(Encode(integer, api::CrudStoredValue::SqlNull()) < Encode(integer, Integer(0)),
        "NULL key is not distinct and before zero");
  const auto uuid = Binding("uuid");
  for (unsigned bit = 0; bit < 128; ++bit) {
    std::string lower(16, '\0'), upper(16, '\0');
    upper[15 - bit / 8] = static_cast<char>(1u << (bit % 8));
    Check(Encode(uuid, lower) < Encode(uuid, upper), "UUID unsigned canonical-byte order changed");
    std::string expected = "SBKO";
    expected.append("\x7f\0\0\0\1\4", 6);
    expected.append(reinterpret_cast<const char*>(uuid.datatype.descriptor_uuid.bytes.data()), 16);
    for (int shift = 56; shift >= 0; shift -= 8)
      expected.push_back(static_cast<char>(uuid.datatype.descriptor_generation >> shift));
    expected.push_back('\0');
    scratchbird::tests::AppendEscapedUuidIndexOracle(expected, uuid.datatype.descriptor_uuid,
        uuid.datatype.descriptor_generation, upper);
    Check(Encode(uuid, upper) == expected, "UUID key bytes differ from independent profile-bound oracle");
  }
  Check(Encode(uuid, api::CrudStoredValue::SqlNull()) < Encode(uuid, std::string(16, '\0')),
        "UUID SQL NULL aliases nil user value");
  Refusals("int64", Integer(17));
  Refusals("uuid", std::string(16, '\0'));
  std::cout << "native_ordered_index_binding checks=" << checks << "\n";
}
