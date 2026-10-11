// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/core/datatypes/datatype_interval.hpp"
#include "../../src/core/hash/hash_digest_parts.hpp"
#include <algorithm>
#include <memory>
#include <stdexcept>
namespace scratchbird::tests {
// Transform only independent frozen oracle authority bytes. Preserve semantic
// mutations with XOR; recompute integrity through the separate hash backend.
inline void D711IntervalOracle(std::vector<core::platform::byte>& b) {
  using core::platform::byte;
  const auto xor_hex=[&](std::size_t off,std::string_view old,std::string_view next) {
    auto d=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<old.size();i+=2)
      b.at(off+i/2)^=byte((d(old[i])*16+d(old[i+1]))^(d(next[i])*16+d(next[i+1])));
  };
  const auto authority=[&](std::size_t off) {
    xor_hex(off,"019d000000007000800000000000d710","01a1095ff20572d3abea15c6d41a4ad4");
    b.at(off+16)^=1;b.at(off+24)^=1;
  };
  const auto profile=[&](std::size_t off) {
    xor_hex(off,"db1e0236a8dd8a04561a57868ccd1ab8bad508f3c1c751aa5d6ba8b5b42823eb",
        "287d76776e481d06ac384bcc73f6e38ebb5902b1180a2040c41379572880d3d4");
  };
  const auto value_hash=[&](std::size_t off,bool null) {
    xor_hex(off,null?"bc67ce9998c0a782fa11ca0f9ce418ef25047fc4fc38fbbcb8caa6c6cf0327cf":
                     "bdef69cea546852f3d832250e246be4dc9fd65029ced67abb75d84f3923ddf12",
                 null?"588f61138a1f79e6ca73e41825b32d29d5320474183d545870c298499727c7c7":
                      "8896ccacb1ce7451ccf73e62cc7f08cfc2b505df7656b27858035e44b725d4ea");
  };
  const auto reseal=[&](std::string_view domain,std::size_t offset) {
    std::array<byte,32> zero{};
    core::hash::HashDigestSegment parts[]={{reinterpret_cast<const byte*>(domain.data()),domain.size()},
      {b.data(),offset},{zero.data(),zero.size()},{b.data()+offset+32,b.size()-offset-32}};
    auto h=core::hash::ComputeSha256DigestPartsNative(parts,4);
    if(!h.ok())throw std::runtime_error("successor oracle integrity");
    std::copy(h.digest.begin(),h.digest.end(),b.begin()+offset);
  };
  const std::string_view magic(reinterpret_cast<const char*>(b.data()),std::min<std::size_t>(8,b.size()));
  if(magic=="SBINIH01") {
    authority(8);
    xor_hex(40,"c0783942e7db60aa79346ddc223e48fabcc197b1894cc81490a4f48b90d498f6",
        "e1b4a6390e3fb96b9e7e349068b91c9f9a5979cb399a77749abf338606d9f717");
    value_hash(128,b.at(120)==1);
  } else if(magic=="SBINTS01") {
    authority(32);profile(400);
    if(b.size()>448)value_hash(448,false);
    reseal("ScratchBird.BaseInterval.StatisticsRecords.V1",368);
  } else if(magic=="SBINBK01") {
    authority(16);profile(128);
    static constexpr char domain[]="ScratchBird.BaseInterval.BackupTuple.V1";
    reseal(std::string_view(domain,sizeof(domain)),328);
  }
}
inline void MutateIntervalNativeIdentity(core::datatypes::DatatypeTypeCodecIdentityRowV3& row, unsigned field) {
  auto& n=row.native_fields;
  switch(field) {
    case 0:n.present=!n.present;break;
    case 1:++n.canonical_value_minimum_bytes;break;
    case 2:++n.canonical_value_maximum_bytes;break;
    case 3:++n.canonical_value_transport_width;break;
    case 4:n.canonical_value_variable_width=!n.canonical_value_variable_width;break;
    case 5:n.policy_profile_uuid.bytes[0]^=1;break;
    case 6:++n.policy_profile_generation;break;
    case 7:n.profile_fingerprint_sha256[0]^=1;break;
    default:throw std::runtime_error("invalid native field mutation");
  }
}
inline std::shared_ptr<const core::datatypes::IntervalValidatedProfileHandleV3>
D711IntervalProfile() {
  namespace dt = core::datatypes;
  const dt::DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (row.legacy_fields.catalog_snapshot_uuid == dt::kDatatypeCohortV11 &&
        row.legacy_fields.canonical_binary_type_code ==
            static_cast<core::platform::u32>(dt::CanonicalTypeId::interval)) {
      if (identity) throw std::runtime_error("duplicate D711 INTERVAL");
      identity = &row;
    }
  }
  if (!identity) throw std::runtime_error("missing D711 INTERVAL");
  auto result = dt::BuildIntervalValidatedProfileHandleV3(
      {dt::kDatatypeCohortV11, dt::kDatatypeCohortV11, 11, 11}, *identity);
  if (!result.ok()) throw std::runtime_error("D711 INTERVAL admission failed");
  return std::make_shared<const dt::IntervalValidatedProfileHandleV3>(std::move(result.profile));
}
}  // namespace scratchbird::tests
