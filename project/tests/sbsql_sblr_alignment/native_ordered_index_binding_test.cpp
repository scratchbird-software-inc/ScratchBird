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
key::OrderedIndexColumn Binding(const char* name) {
  const auto source = exec::MakeExecutorDescriptor(name, "nullability=nullable");
  key::OrderedIndexColumn binding;
  Check(dt::LookupDatatypeStorageIdentityV1(api::kBootstrapDatatypeCatalogUuid,
      api::kBootstrapDatatypeCatalogGeneration, api::kBootstrapDatatypeRegistryGeneration,
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
    auto source = exec::MakeExecutorDescriptor(name, "nullability=nullable");
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
  auto source = exec::MakeExecutorDescriptor(name, "canonical=" + std::string(name));
  scratchbird::engine::ExecutionTypeDescriptor projected;
  std::string detail;
  Check(key::BuildOrderedColumnExecutionDescriptor(source, bound.datatype, false, &projected, &detail) &&
        !projected.nullable_allowed, "typed storage nullability was replaced by a default");
  const auto& codec = *bound.datatype.codec;
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
int main() {
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
