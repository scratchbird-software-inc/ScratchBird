// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "nosql/native_descriptor_fields.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  auto identity = scratchbird::tests::FixtureUuid(1139, 1);
  identity.bytes[9] = 0; identity.bytes[15] = 0xff;
  a::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1139, 2);
  descriptor.type_uuid = identity;
  descriptor.descriptor_kind = "scalar";
  descriptor.encoded_descriptor = "nullability=non_null";
  const auto admitted = [&] { return a::ExactNativeDescriptorFields(descriptor,
      {{"type_uuid", identity}, {"nullability", "non_null"}}); };
  Check(admitted());
  descriptor.encoded_descriptor += ";type_uuid=018f0000-0000-7000-8000-000000000001";
  Check(!admitted());
  descriptor.encoded_descriptor = "nullability=non_null;nullability=non_null";
  Check(!admitted());
  descriptor.descriptor_kind = "canonical_type_descriptor";
  a::CatalogColumnMetadata fields;
  fields.identities = {{"type_uuid", identity}};
  fields.text = {{"nullability", "non_null"}};
  Check(a::EncodeCatalogColumnMetadata(fields, &descriptor.encoded_descriptor));
  Check(admitted());
  descriptor.type_uuid.bytes[15] ^= 1;
  Check(!admitted());
  const auto native_key = a::EncodeNoSqlBatchLookupKey(identity);
  Check(native_key.size() == 17 && native_key[0] == 2);
  Check(std::equal(identity.bytes.begin(), identity.bytes.end(),
        reinterpret_cast<const std::uint8_t*>(native_key.data() + 1)));
  const std::string text(identity.bytes.begin(), identity.bytes.end());
  Check(a::EncodeNoSqlBatchLookupKey(text) != native_key);
  Check(a::EngineNoSqlLookupRowUuid(identity).value == identity);
  auto invalid = identity; invalid.bytes[8] = 0;
  Check(!a::EngineNoSqlLookupRowUuid(invalid).valid());
  Check(!a::EngineNoSqlLookupRowUuid({}).valid());
}
