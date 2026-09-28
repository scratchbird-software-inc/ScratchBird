// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_storage_identity.hpp"
#include <iostream>
#include <stdexcept>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

int main() try {
  auto require = [](bool ok) { if (!ok) throw std::runtime_error("storage binding oracle failed"); };
  const p::Uuid int64_descriptor{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x11}};
  const p::Uuid int64_type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x12}};
  const p::Uuid blob_descriptor{{0xf4,0x01,0,0,0x62,0x6c,0x7f,0x62,0x80,0,0,0,0,0,0,0}};
  const p::Uuid unknown{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xff,0xff}};
  dt::DatatypeStorageIdentityV1 out;
  require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,int64_descriptor,1,&out));
  require(out.descriptor_uuid==int64_descriptor && out.descriptor_generation==1 &&
          out.type_uuid==int64_type && out.type_id==dt::CanonicalTypeId::int64);
  require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,blob_descriptor,1,&out));
  require(out.descriptor_uuid==blob_descriptor && out.descriptor_generation==1 &&
          out.type_uuid==blob_descriptor && out.type_id==dt::CanonicalTypeId::blob);
  // Storage recognition must not add an unimplemented literal codec.
  require(!dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV4,4,4,blob_descriptor,1).ok);
  const auto prior = out;
  unsigned rejected = 0;
  for (const auto& descriptor : {int64_descriptor, blob_descriptor}) {
    for (const auto generation : {0ULL, 2ULL, 5ULL, ~0ULL}) {
      require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,generation,&out));
      ++rejected;
    }
    for (const auto generation : {0ULL, 1ULL, 3ULL, 5ULL, ~0ULL}) {
      require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,generation,4,descriptor,1,&out));
      require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,generation,descriptor,1,&out));
      rejected += 2;
    }
    for (const auto& snapshot : {p::Uuid{}, unknown, dt::kDatatypeCohortV3}) {
      require(!dt::LookupDatatypeStorageIdentityV1(snapshot,4,4,descriptor,1,&out));
      ++rejected;
    }
    require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,1,nullptr));
    ++rejected;
  }
  for (const auto& descriptor : {p::Uuid{}, unknown}) {
    require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,1,&out));
    ++rejected;
  }
  require(out.descriptor_uuid==prior.descriptor_uuid && out.descriptor_generation==prior.descriptor_generation &&
          out.type_uuid==prior.type_uuid && out.type_id==prior.type_id);
  // Immutable predecessor codec receipts remain exact, not upgraded implicitly.
  require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV3,3,3,int64_descriptor,1,&out));
  require(out.type_uuid==int64_type && out.type_id==dt::CanonicalTypeId::int64);
  require(!dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV3,3,3,blob_descriptor,1,&out));
  std::cout << "datatype_storage_identity_conformance=passed rejected=" << rejected+1 << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
