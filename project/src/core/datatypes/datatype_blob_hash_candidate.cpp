// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_blob_hash_candidate.hpp"

#include <algorithm>

namespace scratchbird::core::datatypes {
namespace {
constexpr std::array<byte,8> kMagic{'S','B','B','L','H','K','0','3'};
constexpr char kDomain[] = "ScratchBird/base.blob/profile-bound-equality/v3";
struct Family { platform::Uuid uuid; u8 ordering; bool hash; bool included = false; };
constexpr std::array<Family,16> kFamilies{{
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7e,0xd9,0x91,0xf1,0xce,0x92,0x48,0x84,0x07,0x58}},0,false}, // aggregate_sketch
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7c,0xdb,0xa2,0x54,0x02,0x6a,0x09,0x4d,0x36,0xea}},0,false}, // bitmap
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0x8b,0xa1,0x9e,0x82,0x78,0xdb,0x41,0x6b,0xb7}},8,false}, // brin_like
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0xb9,0x8a,0x56,0x85,0xb4,0xc1,0xc3,0x5f,0x19}},9,false}, // btree
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0x79,0xa1,0x11,0xdd,0x23,0x09,0x92,0x35,0x1c}},10,false}, // columnar_zone_map
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0xc8,0x9b,0xa3,0x6c,0x33,0x07,0x41,0x1f,0x66}},0,false,true}, // covering_included
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0x4d,0x95,0x6a,0x2c,0xb0,0x5c,0x17,0xb8,0x95}},0,false}, // document_path
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x73,0x3c,0x84,0x93,0xef,0x59,0x7d,0xa5,0xc7,0x27}},0,false}, // expression
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x73,0x2b,0x97,0x21,0x62,0xe3,0x35,0xf4,0x18,0x31}},0,false}, // full_text
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0xcb,0x97,0x0b,0xd2,0x7f,0x5c,0x20,0x96,0x5c}},0,false}, // graph
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x75,0xb9,0xa6,0x4b,0x1b,0xd1,0x61,0x7b,0x87,0x45}},0,true}, // hash
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0xec,0x82,0x99,0xd4,0xde,0x95,0x9d,0x92,0x91}},0,false}, // partial_filtered
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x72,0xbf,0xa2,0x32,0x11,0x04,0x3c,0xef,0x8a,0x7b}},11,false}, // range_exclusion
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x70,0x95,0xaa,0xf9,0xd3,0xab,0xc9,0xf6,0x15,0xf9}},12,false}, // spatial
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0xa9,0xb5,0x5d,0xa7,0x62,0x4d,0x63,0x73,0x9e}},0,false}, // temporary_work
  {{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0xb6,0x9b,0x07,0x6d,0x71,0x6d,0x2c,0xeb,0xf4}},13,false}, // vector_ann
}};
static_assert(sizeof(kDomain) == 48);
constexpr platform::Uuid kEncodingDiagnostic{{
    0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0x8b,
    0xbe,0xac,0x36,0xa9,0xdc,0x66,0xaa,0x68}};

bool Range(const void* data, std::size_t size) noexcept {
  const auto start = reinterpret_cast<std::uintptr_t>(data);
  return size == 0 || (data && size <= std::numeric_limits<std::uintptr_t>::max()-start);
}
bool Overlap(const void* a, std::size_t an, const void* b, std::size_t bn) noexcept {
  if (!Range(a,an) || !Range(b,bn)) return true;
  if (!an || !bn) return false;
  const auto x = reinterpret_cast<std::uintptr_t>(a);
  const auto y = reinterpret_cast<std::uintptr_t>(b);
  return x < y+bn && y < x+an;
}
void Put64(BlobHashCandidateFrameV3& h, std::size_t at, u64 value) noexcept {
  for (unsigned i=0; i<8; ++i) h[at+i] = static_cast<byte>(value >> (8*i));
}
u64 Get(std::span<const byte> h, std::size_t at, unsigned n=8) noexcept {
  u64 value=0;
  for (unsigned i=0; i<n; ++i) value |= static_cast<u64>(h[at+i]) << (8*i);
  return value;
}
void PutUuid(BlobHashCandidateFrameV3& h, std::size_t at, const platform::Uuid& id) noexcept {
  std::copy(id.bytes.begin(),id.bytes.end(),h.begin()+at);
}
BlobComponentPassResultV3 Encoding(u32 reason, u64 at) noexcept {
  return {BlobComponentPassFailureV3::canonical_encoding_invalid,
      "BLOB.CANONICAL_ENCODING_INVALID",reason,3,true,at,kEncodingDiagnostic,1};
}
void Identity(BlobHashCandidateFrameV3& h, const BlobValidatedProfileHandleV3& p) noexcept {
  PutUuid(h,40,p.identity.descriptor_uuid); Put64(h,56,p.identity.descriptor_generation);
  PutUuid(h,64,p.identity.type_uuid); Put64(h,80,p.identity.type_generation);
  PutUuid(h,88,p.identity.codec_uuid); Put64(h,104,p.identity.codec_version);
  Put64(h,112,p.identity.codec_generation); PutUuid(h,120,p.receipt.receipt_uuid);
  Put64(h,136,p.receipt.catalog_generation); Put64(h,144,p.receipt.registry_generation);
  PutUuid(h,152,p.profile_uuid); Put64(h,168,p.profile_generation);
  std::copy(p.profile_fingerprint.begin(),p.profile_fingerprint.end(),h.begin()+176);
  PutUuid(h,208,p.identity.hash_policy.uuid); Put64(h,224,p.identity.hash_policy.generation);
}
} // namespace

BlobRawIndexPolicyResultV3 SelectBlobRawIndexPolicyNoAllocV3(
    const BlobValidatedProfileHandleV3& profile,
    const platform::Uuid& family, u64 generation,
    u64 length, u64 budget, u64 target_limit) noexcept {
  BlobRawIndexPolicyResultV3 result;
  if (!ValidateBlobProfileHandleV3(profile).ok()) return result;
  if (length>kBlobMaximumLogicalBytesV3) {
    result.disposition=BlobRawIndexPolicyDispositionV3::input_invalid;
    return result;
  }
  result.disposition=BlobRawIndexPolicyDispositionV3::family_binding_invalid;
  if (generation!=1) return result;
  for (const auto& row:kFamilies) {
    if (row.uuid.bytes!=family.bytes) continue;
    if (row.hash) {
      result.disposition=BlobRawIndexPolicyDispositionV3::hash_candidate_requires_recheck;
      return result;
    }
    result.disposition=BlobRawIndexPolicyDispositionV3::refused;
    result.diagnostic_generation=1;
    if (row.included) {
      result.diagnostic_code="BLOB.MATERIALIZATION_REFUSED";
      result.diagnostic_uuid={{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x70,0xa6,
          0x90,0x7e,0x2f,0x79,0xa7,0xd6,0x78,0x5e}};
      result.logical_length_u64=length;
      result.budget_octets_u64=budget;
      result.target_limit_u64=target_limit;
    } else if (row.ordering) {
      result.diagnostic_code="BLOB.ORDERING_REFUSED";
      result.diagnostic_uuid={{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0xef,
          0xb3,0xa1,0x1b,0xe3,0x36,0xb4,0x2a,0x20}};
      result.reason_enum=83;
      result.ordering_operation_enum=row.ordering;
    } else {
      result.diagnostic_code="CINL.LOB.DESCRIPTOR_INVALID";
      result.diagnostic_uuid={{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7a,0x4c,
          0x8e,0x41,0x52,0x7e,0x88,0x3a,0x2d,0xc4}};
      result.descriptor_uuid=profile.identity.descriptor_uuid;
      result.descriptor_generation=profile.identity.descriptor_generation;
      result.reason_enum=88;
    }
    return result;
  }
  return result;
}

BlobComponentPassResultV3 ValidateBlobHashCandidateNoAllocV3(
    const BlobValidatedProfileHandleV3& p, std::span<const byte> h) noexcept {
  if (!Range(h.data(),h.size())) return {BlobComponentPassFailureV3::input_extent_invalid};
  if (!ValidateBlobProfileHandleV3(p).ok()) return {BlobComponentPassFailureV3::profile_invalid};
  if (h.size()<304) return Encoding(41,h.size());
  for (std::size_t i=0; i<8; ++i) if (h[i]!=kMagic[i]) return Encoding(42,i);
  if (Get(h,8,2)!=3) return Encoding(43,8);
  if (Get(h,10,2)!=304) return Encoding(44,10);
  if (Get(h,12,4)!=0) return Encoding(45,12);
  if (Get(h,16)!=304) return Encoding(47,16);
  if (h[24]!=0) return Encoding(49,24);
  if (h[25]!=1) return Encoding(57,25);
  for (std::size_t i=26; i<32; ++i) if (h[i]) return Encoding(46,i);
  if (Get(h,32)>kBlobMaximumLogicalBytesV3) return Encoding(55,32);
  // No second frame or hash state is needed at this initial shape gate.
  const auto id = [&](std::size_t at, const platform::Uuid& uuid) noexcept {
    return std::equal(uuid.bytes.begin(),uuid.bytes.end(),h.begin()+at);
  };
  if (!id(40,p.identity.descriptor_uuid)) return Encoding(51,40);
  if (Get(h,56)!=p.identity.descriptor_generation) return Encoding(51,56);
  if (!id(64,p.identity.type_uuid)) return Encoding(51,64);
  if (Get(h,80)!=p.identity.type_generation) return Encoding(51,80);
  if (!id(88,p.identity.codec_uuid)) return Encoding(51,88);
  if (Get(h,104)!=p.identity.codec_version) return Encoding(51,104);
  if (Get(h,112)!=p.identity.codec_generation) return Encoding(51,112);
  if (!id(120,p.receipt.receipt_uuid)) return Encoding(51,120);
  if (Get(h,136)!=p.receipt.catalog_generation) return Encoding(51,136);
  if (Get(h,144)!=p.receipt.registry_generation) return Encoding(51,144);
  if (!id(152,p.profile_uuid)) return Encoding(51,152);
  if (Get(h,168)!=p.profile_generation) return Encoding(51,168);
  if (!std::equal(p.profile_fingerprint.begin(),p.profile_fingerprint.end(),h.begin()+176)) return Encoding(51,176);
  if (!id(208,p.identity.hash_policy.uuid)) return Encoding(51,208);
  if (Get(h,224)!=p.identity.hash_policy.generation) return Encoding(51,224);
  bool nil=true;
  for (std::size_t i=264; i<280; ++i) nil &= h[i]==0;
  if (nil) return Encoding(58,264);
  if (Get(h,280)==0) return Encoding(58,280);
  if (Get(h,288)==0) return Encoding(59,288);
  for (std::size_t i=296; i<304; ++i) if (h[i]) return Encoding(46,i);
  if (h.size()!=304) return Encoding(48,304);
  return {};
}

BlobComponentPassResultV3 EncodeBlobHashCandidateNoAllocV3(
    const BlobValidatedProfileHandleV3& p, u64 length,
    std::span<const byte> content_digest, const platform::Uuid& reference,
    u64 generation, u64 epoch, BlobHashCandidateFrameV3& output) noexcept {
  if (content_digest.size()!=32 || !Range(content_digest.data(),content_digest.size()))
    return {BlobComponentPassFailureV3::input_extent_invalid};
  if (Overlap(&output,sizeof(output),&p,sizeof(p)) ||
      Overlap(&output,sizeof(output),content_digest.data(),content_digest.size()) ||
      Overlap(&output,sizeof(output),&reference,sizeof(reference)))
    return {BlobComponentPassFailureV3::input_overlap};
  if (!ValidateBlobProfileHandleV3(p).ok()) return {BlobComponentPassFailureV3::profile_invalid};
  // Check private input before allocating hash workspace or touching output.
  if (length>kBlobMaximumLogicalBytesV3 || reference.is_nil() || !generation || !epoch)
    return {BlobComponentPassFailureV3::input_extent_invalid};
  BlobHashCandidateFrameV3 staged{};
  std::copy(kMagic.begin(),kMagic.end(),staged.begin());
  staged[8]=3; staged[10]=0x30; staged[11]=1; Put64(staged,16,304);
  staged[25]=1; Put64(staged,32,length); Identity(staged,p);
  PutUuid(staged,264,reference); Put64(staged,280,generation); Put64(staged,288,epoch);
  hash::Blake3IncrementalStateV3 hash_state{};
  hash::Blake3Digest projection{};
  using H=hash::Blake3IncrementalStatusV3;
  const auto update = [&](std::span<const byte> part) noexcept {
    return hash::UpdateBlake3IncrementalV3(&hash_state,part.data(),part.size())==H::kOk;
  };
  // Exact normative order, including ONE terminal NUL. The row reference and
  // rebuild epoch deliberately do not participate in content candidate identity.
  const auto h=std::span<const byte>(staged);
  const bool ok=hash::InitializeBlake3IncrementalV3(&hash_state)==H::kOk &&
      update({reinterpret_cast<const byte*>(kDomain),sizeof(kDomain)}) &&
      update(h.subspan(120,32)) && update(h.subspan(40,80)) &&
      update(h.subspan(152,80)) && update(content_digest) &&
      hash::FinalizeBlake3IncrementalV3(&hash_state,&projection)==H::kOk;
  hash::ScrubBlake3IncrementalV3(&hash_state);
  if (!ok) return {BlobComponentPassFailureV3::hash_state_failed};
  std::copy(projection.begin(),projection.end(),staged.begin()+232);
  output=staged;
  return {};
}

} // namespace scratchbird::core::datatypes
