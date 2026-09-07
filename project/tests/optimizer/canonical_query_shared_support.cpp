// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_json_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace query = scratchbird::engine::sblr;

namespace {

// SEARCH_KEY: SB_TEST_CANONICAL_QUERY_SHARED_SUPPORT_BOUNDARY_MATRIX

api::EngineDescriptor Descriptor(std::string uuid,
                                 std::string type,
                                 std::string encoded = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::move(uuid);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = std::move(encoded);
  return descriptor;
}

}  // namespace

int main() {
  bool passed = true;
  const auto expect = [&](const bool condition, const char* detail) {
    if (!condition) {
      std::cerr << detail << '\n';
      passed = false;
    }
  };

  const std::string uuid = "019d0000-0000-7000-8000-000000000001";
  expect(query::CanonicalUuidText(uuid), "canonical UUID was refused");
  expect(!query::CanonicalUuidText("019D0000-0000-7000-8000-000000000001"),
         "uppercase UUID was admitted");
  expect(query::DerivedCanonicalUuid("scope", "purpose") ==
             query::DerivedCanonicalUuid("scope", "purpose"),
         "derived UUID was not deterministic");

  api::EngineTypedValue integer;
  integer.descriptor = Descriptor(uuid, "int64");
  integer.encoded_value = "-42";
  std::int64_t decoded = 0;
  std::string detail;
  expect(query::DecodeCanonicalInt64Scalar(integer, &decoded, &detail) &&
             decoded == -42,
         "text int64 decode failed");
  integer.encoded_value.clear();
  integer.binary_value = {0xd6, 0xff, 0xff, 0xff,
                          0xff, 0xff, 0xff, 0xff};
  expect(query::DecodeCanonicalInt64Scalar(integer, &decoded, &detail) &&
             decoded == -42,
         "binary int64 decode failed");
  std::string equality_key;
  expect(query::EncodeCanonicalScalarEqualityKey(integer, &equality_key,
                                                  &detail) &&
             !equality_key.empty(),
         "canonical scalar key encoding failed");

  auto expanded = query::ExpandCanonicalDocumentWildcard(
      R"({"items":[1,"two",{"nested":true}]})", "$.items[*]", 3, 4096);
  expect(expanded.ok && expanded.path_present && expanded.elements.size() == 3,
         "canonical JSON wildcard expansion failed");
  expanded = query::ExpandCanonicalDocumentWildcard(
      R"({"items":[1,2]})", "$.items[*]", 1, 4096);
  expect(!expanded.ok, "JSON wildcard row bound was not enforced");

  auto left_descriptor = Descriptor(uuid, "int64", "type=int64");
  auto right_descriptor = left_descriptor;
  expect(query::SameExactEngineDescriptorV1(left_descriptor,
                                             right_descriptor),
         "equal engine descriptors differed");
  right_descriptor.encoded_descriptor = "type=int32";
  expect(!query::SameExactEngineDescriptorV1(left_descriptor,
                                              right_descriptor),
         "different engine descriptors compared equal");
  expect(query::ResultNullability(api::RelationalNullability::kNullable) ==
             exec::CanonicalResultNullability::kNullable,
         "result nullability projection failed");

  std::uint64_t bytes = 0;
  expect(query::CheckedAdd(2, 3, &bytes) && bytes == 5,
         "checked addition failed");
  expect(!query::CheckedAdd(std::numeric_limits<std::uint64_t>::max(), 1,
                            &bytes),
         "checked addition admitted overflow");
  expect(query::CheckedMultiply(6, 7, &bytes) && bytes == 42,
         "checked multiplication failed");
  expect(!query::CheckedMultiply(std::numeric_limits<std::uint64_t>::max(), 2,
                                 &bytes),
         "checked multiplication admitted overflow");

  exec::DescriptorBatch batch;
  batch.columns.push_back({"value", left_descriptor, false, 1});
  api::EngineTypedValue value;
  value.descriptor = left_descriptor;
  value.encoded_value = "42";
  batch.rows.push_back({{value}});
  expect(query::RuntimeMaterializedBatchMemoryBytes(batch, &bytes) &&
             bytes == 3,
         "runtime batch payload accounting failed");
  expect(query::RuntimeTypedValueMemoryBytes(value, &bytes) && bytes > 2,
         "runtime typed-value accounting failed");
  bytes = 0;
  expect(query::AddBatchMemoryBytes(batch, &bytes) && bytes > 0,
         "retained batch accounting failed");
  expect(query::BoundDescriptorBatchLiveMemoryBytes(batch, &bytes) &&
             bytes > 0,
         "live batch accounting failed");
  expect(query::LogicalBitVectorPayloadBytes(9, &bytes) && bytes == 2,
         "bit-vector accounting failed");

  return passed ? 0 : 1;
}
