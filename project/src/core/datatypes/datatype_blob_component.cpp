// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_blob_component.hpp"

#include <algorithm>

namespace scratchbird::core::datatypes {
namespace {

constexpr char kLogicalDomain[] = "ScratchBird/base.blob/logical-content/v3";
constexpr char kNullDomain[] = "ScratchBird/base.blob/component-null-state/v3";
constexpr char kFrameDomain[] = "ScratchBird/base.blob/component-frame/v3";
constexpr std::array<byte,8> kMagic{{'S','B','B','L','O','B','0','3'}};
constexpr platform::Uuid kEncodingDiagnostic{{
  0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0x8b,0xbe,0xac,0x36,0xa9,0xdc,0x66,0xaa,0x68}};
constexpr platform::Uuid kIntegrityDiagnostic{{
  0x01,0xa1,0x09,0x5f,0xf2,0x05,0x78,0x79,0xb9,0x23,0x42,0x5b,0xe9,0x3f,0x2c,0x1a}};

void Put64(BlobComponentHeaderV3& h, std::size_t at, u64 value) noexcept {
  for (unsigned i = 0; i < 8; ++i) h[at+i] = static_cast<byte>(value >> (8*i));
}

u64 Get(std::span<const byte> h, std::size_t at, unsigned count) noexcept {
  u64 value = 0;
  for (unsigned i = 0; i < count; ++i) value |= static_cast<u64>(h[at+i]) << (8*i);
  return value;
}

void PutUuid(BlobComponentHeaderV3& h, std::size_t at,
             const platform::Uuid& id) noexcept {
  std::copy(id.bytes.begin(),id.bytes.end(),h.begin()+at);
}

void BuildHeader(BlobComponentHeaderV3& h,
    const BlobValidatedProfileHandleV3& p, BlobValueStateV3 state, u64 length) noexcept {
  h = {};
  std::copy(kMagic.begin(),kMagic.end(),h.begin());
  h[8] = 3;
  h[10] = 0x40; h[11] = 1;
  Put64(h,16,320+length);
  h[24] = static_cast<byte>(state);
  Put64(h,32,length);
  PutUuid(h,40,p.identity.descriptor_uuid); Put64(h,56,p.identity.descriptor_generation);
  PutUuid(h,64,p.identity.type_uuid); Put64(h,80,p.identity.type_generation);
  PutUuid(h,88,p.identity.codec_uuid); Put64(h,104,p.identity.codec_version);
  Put64(h,112,p.identity.codec_generation); PutUuid(h,120,p.receipt.receipt_uuid);
  Put64(h,136,p.receipt.catalog_generation); Put64(h,144,p.receipt.registry_generation);
  PutUuid(h,152,p.identity.descriptor_policy.uuid);
  PutUuid(h,168,p.identity.canonicalization_policy.uuid);
  PutUuid(h,184,p.identity.hash_policy.uuid);
  PutUuid(h,200,p.identity.operation_policy.uuid);
  std::copy(p.profile_fingerprint.begin(),p.profile_fingerprint.end(),h.begin()+216);
}

// Includes address-overflow checks before arithmetic or dereference. Public
// byte spans are structural inputs, never permission to read our live state.
bool Overlaps(const void* owner, std::size_t owner_size,
              const void* data, std::size_t bytes) noexcept {
  if (bytes == 0) return false;
  if (data == nullptr) return true;
  const auto a = reinterpret_cast<std::uintptr_t>(owner);
  const auto b = reinterpret_cast<std::uintptr_t>(data);
  constexpr auto max = std::numeric_limits<std::uintptr_t>::max();
  if (owner_size > max-a || bytes > max-b) return true;
  return a < b+bytes && b < a+owner_size;
}

void ScrubBytes(std::span<byte> bytes) noexcept {
  volatile byte* p = bytes.data();
  for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
}

bool HashUpdate(hash::Blake3IncrementalStateV3& state,
                std::span<const byte> bytes) noexcept {
  return hash::UpdateBlake3IncrementalV3(&state,bytes.data(),bytes.size()) ==
      hash::Blake3IncrementalStatusV3::kOk;
}

template <std::size_t N>
bool HashDomain(hash::Blake3IncrementalStateV3& state, const char (&text)[N]) noexcept {
  // sizeof includes exactly the normative terminal NUL.
  return HashUpdate(state,{reinterpret_cast<const byte*>(text),N});
}

} // namespace

void BlobComponentDigestPassV3::ClearWork() noexcept {
  hash::ScrubBlake3IncrementalV3(&content_hash_);
  hash::ScrubBlake3IncrementalV3(&frame_hash_);
  ScrubBytes(header_);
  ScrubBytes(expected_digests_);
}

void BlobComponentDigestPassV3::Scrub() noexcept {
  ClearWork();
  phase_ = Phase::empty; state_ = BlobValueStateV3::value;
  expected_ = consumed_ = hash_policy_generation_ = 0;
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::Fail(
    BlobComponentPassFailureV3 failure) noexcept {
  Scrub(); phase_ = Phase::failed;
  return {failure};
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::CanonicalFailure(
    u32 reason, u64 offset) noexcept {
  (void)Fail(BlobComponentPassFailureV3::canonical_encoding_invalid);
  return {BlobComponentPassFailureV3::canonical_encoding_invalid,
          "BLOB.CANONICAL_ENCODING_INVALID",reason,1,true,offset,kEncodingDiagnostic,1};
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::StartDigests() noexcept {
  using H = hash::Blake3IncrementalStatusV3;
  if (hash::InitializeBlake3IncrementalV3(&content_hash_) != H::kOk ||
      hash::InitializeBlake3IncrementalV3(&frame_hash_) != H::kOk)
    return Fail(BlobComponentPassFailureV3::hash_state_failed);
  const auto h = std::span<const byte>(header_);
  // Receipt/profile are excluded from logical identity. Immutable native
  // descriptor/type/codec/hash-policy identities and generations are not.
  const bool domain = state_ == BlobValueStateV3::sql_null
      ? HashDomain(content_hash_,kNullDomain) : HashDomain(content_hash_,kLogicalDomain);
  std::array<byte,8> hash_generation{};
  for (unsigned i = 0; i < 8; ++i)
    hash_generation[i] = static_cast<byte>(hash_policy_generation_ >> (8*i));
  if (!domain || !HashUpdate(content_hash_,h.subspan(40,80)) ||
      !HashUpdate(content_hash_,h.subspan(184,16)) ||
      !HashUpdate(content_hash_,hash_generation) ||
      (state_ == BlobValueStateV3::value && !HashUpdate(content_hash_,h.subspan(32,8))) ||
      !HashDomain(frame_hash_,kFrameDomain) || !HashUpdate(frame_hash_,h))
    return Fail(BlobComponentPassFailureV3::hash_state_failed);
  return {};
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::BeginEncode(
    const BlobValidatedProfileHandleV3& profile, BlobValueStateV3 state,
    u64 length) noexcept {
  if (phase_ != Phase::empty) return Fail(BlobComponentPassFailureV3::invalid_phase);
  if (!ValidateBlobProfileHandleV3(profile).ok())
    return Fail(BlobComponentPassFailureV3::profile_invalid);
  if ((state != BlobValueStateV3::value && state != BlobValueStateV3::sql_null) ||
      length > kBlobMaximumLogicalBytesV3 || (state == BlobValueStateV3::sql_null && length != 0))
    return Fail(BlobComponentPassFailureV3::invalid_encode_state);
  state_ = state; expected_ = length;
  hash_policy_generation_ = profile.identity.hash_policy.generation;
  BuildHeader(header_,profile,state,length);
  phase_ = Phase::encoding;
  return StartDigests();
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::BeginDecode(
    const BlobValidatedProfileHandleV3& profile, u64 extent,
    std::span<const byte> header, bool null_allowed) noexcept {
  if (Overlaps(this,sizeof(*this),header.data(),header.size()))
    return {BlobComponentPassFailureV3::input_overlap};
  if (phase_ != Phase::empty) return Fail(BlobComponentPassFailureV3::invalid_phase);
  if (!ValidateBlobProfileHandleV3(profile).ok())
    return Fail(BlobComponentPassFailureV3::profile_invalid);
  if (header.size() < 320 || extent < 320)
    return CanonicalFailure(41,std::min<u64>(header.size(),extent));
  if (header.size() != 320) return Fail(BlobComponentPassFailureV3::input_extent_invalid);
  for (std::size_t i = 0; i < kMagic.size(); ++i)
    if (header[i] != kMagic[i]) return CanonicalFailure(42,i);
  if (Get(header,8,2) != 3) return CanonicalFailure(43,8);
  if (Get(header,10,2) != 320) return CanonicalFailure(44,10);
  if (Get(header,12,4) != 0) return CanonicalFailure(45,12);
  for (std::size_t i = 25; i < 32; ++i)
    if (header[i] != 0) return CanonicalFailure(46,i);
  for (std::size_t i = 312; i < 320; ++i)
    if (header[i] != 0) return CanonicalFailure(46,i);
  const auto length = Get(header,32,8);
  if (length > kBlobMaximumLogicalBytesV3) return CanonicalFailure(55,32);
  const auto total = 320+length; // Prior bound makes this addition representable.
  if (Get(header,16,8) != total) return CanonicalFailure(47,16);
  if (extent != total) return CanonicalFailure(48,std::min(extent,total));
  if (header[24] > 1) return CanonicalFailure(49,24);
  const auto state = static_cast<BlobValueStateV3>(header[24]);
  if (state == BlobValueStateV3::sql_null && (length != 0 || !null_allowed))
    return CanonicalFailure(50,32);
  // Compare whole identity fields, not the first differing byte inside a
  // UUID/digest. The canonical diagnostic reports the first FIELD start.
  BuildHeader(header_,profile,state,length);
  constexpr std::array<std::size_t,16> starts{{
    40,56,64,80,88,104,112,120,136,144,152,168,184,200,216,248}};
  for (std::size_t i = 0; i+1 < starts.size(); ++i)
    if (!std::equal(header.begin()+starts[i],header.begin()+starts[i+1],header_.begin()+starts[i]))
      return CanonicalFailure(51,starts[i]);
  std::copy(header.begin()+248,header.begin()+312,expected_digests_.begin());
  state_ = state; expected_ = length;
  hash_policy_generation_ = profile.identity.hash_policy.generation;
  phase_ = Phase::decoding;
  return StartDigests();
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::UpdateContent(
    std::span<const byte> bytes) noexcept {
  if (Overlaps(this,sizeof(*this),bytes.data(),bytes.size()))
    return {BlobComponentPassFailureV3::input_overlap};
  if (phase_ != Phase::encoding && phase_ != Phase::decoding)
    return Fail(BlobComponentPassFailureV3::invalid_phase);
  if (bytes.size() > expected_-consumed_)
    return Fail(BlobComponentPassFailureV3::content_extent_mismatch);
  if (!HashUpdate(content_hash_,bytes) || !HashUpdate(frame_hash_,bytes))
    return Fail(BlobComponentPassFailureV3::hash_state_failed);
  consumed_ += bytes.size();
  return {};
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::FinishDigests() noexcept {
  if (consumed_ != expected_) return Fail(BlobComponentPassFailureV3::content_extent_mismatch);
  // Finalize directly into disjoint fixed output objects. Both hash states
  // scrub themselves, and every later failure scrubs all retained work too.
  hash::Blake3Digest content{},frame{};
  using H = hash::Blake3IncrementalStatusV3;
  if (hash::FinalizeBlake3IncrementalV3(&content_hash_,&content) != H::kOk ||
      hash::FinalizeBlake3IncrementalV3(&frame_hash_,&frame) != H::kOk) {
    ScrubBytes(content); ScrubBytes(frame);
    return Fail(BlobComponentPassFailureV3::hash_state_failed);
  }
  std::copy(content.begin(),content.end(),header_.begin()+248);
  std::copy(frame.begin(),frame.end(),header_.begin()+280);
  ScrubBytes(content); ScrubBytes(frame);
  return {};
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::FinishEncode(
    BlobComponentHeaderV3& output) noexcept {
  if (Overlaps(this,sizeof(*this),output.data(),output.size()))
    return {BlobComponentPassFailureV3::input_overlap};
  if (phase_ != Phase::encoding) return Fail(BlobComponentPassFailureV3::invalid_phase);
  const auto result = FinishDigests();
  if (!result.ok()) return result;
  output = header_; // Fixed-size, allocation-free and nonthrowing publication.
  Scrub(); phase_ = Phase::finished;
  return {};
}

BlobComponentPassResultV3 BlobComponentDigestPassV3::FinishDecode() noexcept {
  if (phase_ != Phase::decoding) return Fail(BlobComponentPassFailureV3::invalid_phase);
  const auto result = FinishDigests();
  if (!result.ok()) return result;
  const auto equal_digest = [&](std::size_t header_offset, std::size_t expected_offset) noexcept {
    byte difference = 0;
    for (std::size_t i = 0; i < 32; ++i)
      difference |= header_[header_offset+i] ^ expected_digests_[expected_offset+i];
    return difference == 0;
  };
  const u32 mismatch = !equal_digest(248,0) ? 30U : !equal_digest(280,32) ? 31U : 0U;
  Scrub(); phase_ = mismatch ? Phase::failed : Phase::finished;
  if (mismatch) return {BlobComponentPassFailureV3::integrity_failed,
      "BLOB.INTEGRITY_FAILED",mismatch,1,false,0,kIntegrityDiagnostic,1};
  return {};
}

} // namespace scratchbird::core::datatypes
