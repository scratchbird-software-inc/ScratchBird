// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included inside the owned-persistence harness's private namespace.
std::vector<byte> BlobHashUnhex(std::string_view hex) {
  Check(hex.size()%2==0,"odd independent hash fixture");
  const auto nibble=[](char c)->byte {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    throw std::runtime_error("bad independent hash fixture hex");
  };
  std::vector<byte> output(hex.size()/2);
  for (std::size_t i=0;i<output.size();++i)
    output[i]=static_cast<byte>((nibble(hex[2*i])<<4)|nibble(hex[2*i+1]));
  return output;
}
std::uint64_t BlobHashGet(std::span<const byte> data,std::size_t at) {
  std::uint64_t result=0;
  for (unsigned i=0;i<8;++i) result |= static_cast<std::uint64_t>(data[at+i])<<(8*i);
  return result;
}
void BlobHashCandidateVectors() {
  Fixture f;
  for (const auto& vector:kBlobHashGoldenVectors) {
    const auto expected=BlobHashUnhex(vector.candidate_hex);
    const auto logical=BlobHashUnhex(vector.logical_digest_hex);
    const auto projection=BlobHashUnhex(vector.projection_hex);
    scratchbird::core::hash::Blake3Digest digest{};
    std::copy(logical.begin(),logical.end(),digest.begin());
    scratchbird::core::platform::Uuid reference{};
    std::copy_n(expected.begin()+264,16,reference.bytes.begin());
    dt::BlobHashCandidateFrameV3 output{};
    allocation_fault_triggered=false; allocation_budget=0;
    const auto result=dt::EncodeBlobHashCandidateNoAllocV3(f.profile,
        BlobHashGet(expected,32),digest,reference,1,1,output);
    const auto valid=dt::ValidateBlobHashCandidateNoAllocV3(f.profile,output);
    allocation_budget=-1;
    Check(result.ok() && valid.ok() && !allocation_fault_triggered && Equal(output,expected),
          "hash candidate differs from independent full304-byte vector or allocated");
    Check(Equal(std::span<const byte>(output).subspan(232,32),projection),
          "profile-bound projection differs from independent digest vector");
    // Same content in another row/version MUST share its lookup candidate.
    auto other_reference=reference; other_reference.bytes[15]^=1;
    dt::BlobHashCandidateFrameV3 other{};
    Check(dt::EncodeBlobHashCandidateNoAllocV3(f.profile,BlobHashGet(expected,32),
        digest,other_reference,2,3,other).ok() &&
        Equal(std::span<const byte>(other).subspan(232,32),projection) && other!=output,
        "row identity/generation/rebuild epoch contaminated content candidate");
    const auto before=output;
    for (const auto width:{0u,31u,33u}) {
      const std::array<byte,33> wrong_digest{};
      Check(!dt::EncodeBlobHashCandidateNoAllocV3(f.profile,0,
          std::span<const byte>(wrong_digest).first(width),reference,1,1,output).ok() && output==before,
          "wrong digest width published or changed candidate output");
    }
    Check(!dt::EncodeBlobHashCandidateNoAllocV3(f.profile,0,
        {static_cast<const byte*>(nullptr),32},reference,1,1,output).ok() && output==before,
        "null digest backing published");
    for (const auto length:{dt::kBlobMaximumLogicalBytesV3+1,UINT64_MAX})
      Check(!dt::EncodeBlobHashCandidateNoAllocV3(f.profile,length,digest,reference,1,1,output).ok() && output==before,
            "oversized hash candidate published");
    Check(dt::EncodeBlobHashCandidateNoAllocV3(f.profile,dt::kBlobMaximumLogicalBytesV3,
        digest,reference,UINT64_MAX,UINT64_MAX,output).ok() &&
        dt::ValidateBlobHashCandidateNoAllocV3(f.profile,output).ok(),
        "maximum structural candidate fields rejected");
    output=before;
    for (unsigned invalid=0;invalid<3;++invalid) {
      auto ref=reference; if (invalid==0) ref={};
      Check(!dt::EncodeBlobHashCandidateNoAllocV3(f.profile,0,digest,ref,
          invalid==1?0:1,invalid==2?0:1,output).ok() && output==before,
          "invalid recheck binding published a candidate");
    }
    auto bad=f.profile; bad.profile_fingerprint[0]^=1;
    Check(!dt::EncodeBlobHashCandidateNoAllocV3(bad,0,digest,reference,1,1,output).ok() && output==before,
          "invalid profile published a candidate");
    const auto alias=dt::EncodeBlobHashCandidateNoAllocV3(f.profile,0,
        std::span<const byte>(output).first(32),reference,1,1,output);
    Check(!alias.ok() && output==before,"candidate output/digest overlap published");
  }
  Clean(f);
}
void BlobHashCandidateMalformed() {
  Fixture f;
  const auto golden=BlobHashUnhex(kBlobHashGoldenVectors[2].candidate_hex);
  const auto fail=[&](const std::vector<byte>& bytes,unsigned reason,std::uint64_t at) {
    constexpr scratchbird::core::platform::Uuid expected_id{{
        0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0x8b,0xbe,0xac,0x36,0xa9,0xdc,0x66,0xaa,0x68}};
    allocation_fault_triggered=false; allocation_budget=0;
    const auto result=dt::ValidateBlobHashCandidateNoAllocV3(f.profile,bytes);
    allocation_budget=-1;
    Check(result.failure==dt::BlobComponentPassFailureV3::canonical_encoding_invalid &&
          result.diagnostic_code=="BLOB.CANONICAL_ENCODING_INVALID" && result.format_enum==3 &&
          result.reason_enum==reason && result.failure_offset_present && result.failure_offset==at &&
          result.diagnostic_uuid.bytes==expected_id.bytes && result.diagnostic_generation==1 && !allocation_fault_triggered,
          "hash candidate malformed vector lost exact diagnostic/offset or allocated");
  };
  for (std::size_t length=0;length<304;++length)
    fail({golden.begin(),golden.begin()+length},41,length);
  for (std::size_t i=0;i<304;++i) {
    auto bad=golden; bad[i]^=0x80;
    unsigned reason=0; std::size_t at=i;
    if (i<8) reason=42;
    else if (i<10) {reason=43;at=8;}
    else if (i<12) {reason=44;at=10;}
    else if (i<16) {reason=45;at=12;}
    else if (i<24) {reason=47;at=16;}
    else if (i==24) reason=49;
    else if (i==25) reason=57;
    else if (i<32 || i>=296) reason=46;
    else if (i<40) {if (i==39) {reason=55;at=32;}}
    else if (i<232) {
      reason=51;
      for (const auto start:{40,56,64,80,88,104,112,120,136,144,152,168,176,208,224})
        if (i>=static_cast<std::size_t>(start)) at=start;
    }
    if (reason) fail(bad,reason,at);
    else Check(dt::ValidateBlobHashCandidateNoAllocV3(f.profile,bad).ok(),
        "shape validation falsely authenticated projection/reference/range content");
  }
  for (const auto at:{264,280,288}) {
    auto bad=golden; std::fill_n(bad.begin()+at,at==264?16:8,0);
    fail(bad,at==288?59:58,at);
  }
  auto trailing=golden; trailing.push_back(1); fail(trailing,48,304);
  // Precedence is structural, not a convenient lowest offset or trailing check.
  trailing[24]=1; fail(trailing,49,24);
  trailing[8]=4; fail(trailing,43,8);
  Check(!dt::ValidateBlobHashCandidateNoAllocV3(f.profile,{static_cast<const byte*>(nullptr),1}).ok(),
        "null nonempty hash candidate span admitted");
  Check(!dt::ValidateBlobHashCandidateNoAllocV3(f.profile,
        {reinterpret_cast<const byte*>(UINTPTR_MAX-1),3}).ok(),"overflowing hash candidate span admitted");
  Clean(f);
}
void BlobRawIndexFamilyPolicy() {
  Fixture f;
  // Warm the actual immutable family registry before the allocation barrier.
  // These are engine-wide family identities, never BLOB compatibility rows.
  const auto& families=scratchbird::core::index::BuiltinIndexFamilyDescriptors();
  Check(!families.empty(),"global family registry is empty");
  for (const auto& family:families) {
    Check(family.family_generation==1 &&
        scratchbird::core::index::FindBuiltinIndexFamilyByIdentity(
            family.family_uuid.value,family.family_generation)==&family,
        "binary lookup did not return the actual immutable global family");
    auto copied=family;
    copied.id="not-the-registered-alias"; copied.canonical_name="forged-display-name";
    allocation_fault_triggered=false; allocation_budget=0;
    Check(scratchbird::core::index::FindBuiltinIndexFamilyByIdentity(
        copied.family_uuid.value,copied.family_generation)==&family,
        "display labels changed binary global-family lookup");
    Check(!allocation_fault_triggered,"hot binary family lookup allocated");
    allocation_budget=-1;
    for (auto generation:{std::uint64_t{0},std::uint64_t{2},UINT64_MAX})
      Check(!scratchbird::core::index::FindBuiltinIndexFamilyByIdentity(
          family.family_uuid.value,generation),"stale global-family generation resolved");
    for (std::size_t i=0;i<16;++i) {
      auto wrong=family.family_uuid.value; wrong.bytes[i]^=0x80;
      Check(!scratchbird::core::index::FindBuiltinIndexFamilyByIdentity(wrong,1),
            "mutated global-family UUID resolved");
    }
    for (const auto& compatibility:kBlobIndexCompatibilityGolden)
      Check(family.family_uuid.value.bytes!=compatibility.index_compatibility_uuid.bytes,
            "global family identity aliases a BLOB datatype compatibility identity");
    allocation_fault_triggered=false; allocation_budget=0;
    const auto refused=dt::SelectBlobRawIndexPolicyNoAllocV3(
        f.profile,family.family_uuid.value,1,0,0,0);
    allocation_budget=-1;
    Check(!allocation_fault_triggered &&
        refused.disposition==dt::BlobRawIndexPolicyDispositionV3::compatibility_binding_invalid &&
        refused.diagnostic_code.empty() && refused.diagnostic_uuid.is_nil(),
        "global family UUID was treated as BLOB compatibility authority");
  }
  for (const auto& compatibility:kBlobIndexCompatibilityGolden)
    Check(!scratchbird::core::index::FindBuiltinIndexFamilyByIdentity(
        compatibility.index_compatibility_uuid,1),
        "BLOB compatibility UUID was treated as global family authority");
  Check(!scratchbird::core::index::FindBuiltinIndexFamilyByIdentity({},1),
        "nil global-family UUID resolved");
  // Independent registered DATE/TIME/TIMESTAMP/INTERVAL HASH compatibility
  // rows must not select the BLOB profile, even though all classify as HASH.
  constexpr scratchbird::core::platform::Uuid foreign_compatibilities[]{
    {{0x01,0xa1,0x00,0x8e,0xc1,0x00,0x70,0x0b,0x80,0,0,0,0,0,0,0x0b}},
    {{0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x52}},
    {{0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,0x6e}},
    {{0x01,0xa1,0x06,0x64,0x77,0xd0,0x73,0xf4,0xa0,0xdd,0xb6,0x87,0x2f,0xd4,0x1a,0xc4}},
    {},
  };
  for (const auto& id:foreign_compatibilities) {
    allocation_fault_triggered=false; allocation_budget=0;
    const auto refused=dt::SelectBlobRawIndexPolicyNoAllocV3(f.profile,id,1,0,0,0);
    allocation_budget=-1;
    Check(!allocation_fault_triggered &&
        refused.disposition==dt::BlobRawIndexPolicyDispositionV3::compatibility_binding_invalid &&
        refused.diagnostic_code.empty(),"foreign or nil compatibility UUID selected BLOB policy");
  }
  unsigned hashes=0,refusals=0;
  for (const auto& row:kBlobIndexCompatibilityGolden) {
    allocation_fault_triggered=false; allocation_budget=0;
    const auto result=dt::SelectBlobRawIndexPolicyNoAllocV3(f.profile,row.index_compatibility_uuid,1,73,4096,8192);
    allocation_budget=-1;
    Check(!allocation_fault_triggered && result.diagnostic_code==row.diagnostic,
          "binary compatibility policy differs from exact specification disposition");
    if (row.diagnostic.empty()) {
      ++hashes;
      Check(result.disposition==dt::BlobRawIndexPolicyDispositionV3::hash_candidate_requires_recheck &&
          result.diagnostic_uuid.is_nil() && result.reason_enum==0 && result.ordering_operation_enum==0,
          "hash admission fabricated an equality/success diagnostic");
    } else {
      ++refusals;
      const scratchbird::core::platform::Uuid expected_id=row.ordering
          ? scratchbird::core::platform::Uuid{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0xef,0xb3,0xa1,0x1b,0xe3,0x36,0xb4,0x2a,0x20}}
          : row.diagnostic=="BLOB.MATERIALIZATION_REFUSED"
          ? scratchbird::core::platform::Uuid{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x70,0xa6,0x90,0x7e,0x2f,0x79,0xa7,0xd6,0x78,0x5e}}
          : scratchbird::core::platform::Uuid{{0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7a,0x4c,0x8e,0x41,0x52,0x7e,0x88,0x3a,0x2d,0xc4}};
      Check(result.disposition==dt::BlobRawIndexPolicyDispositionV3::refused &&
          result.diagnostic_uuid.bytes==expected_id.bytes && result.diagnostic_generation==1 &&
          result.ordering_operation_enum==row.ordering,"unsupported BLOB family did not explicitly refuse");
      if (row.ordering) Check(result.reason_enum==83,"ordering refusal missing natural_order_absent");
      else if (row.diagnostic=="BLOB.MATERIALIZATION_REFUSED")
        Check(result.logical_length_u64==73 && result.budget_octets_u64==4096 && result.target_limit_u64==8192,
              "included-payload refusal lost actual bounds");
      else Check(result.reason_enum==88 && result.descriptor_uuid.bytes==f.profile.identity.descriptor_uuid.bytes &&
          result.descriptor_generation==f.profile.identity.descriptor_generation,
          "nonordering refusal fabricated ordering or lost descriptor vector");
    }
    for (auto generation:{std::uint64_t{0},std::uint64_t{2},UINT64_MAX})
      Check(dt::SelectBlobRawIndexPolicyNoAllocV3(f.profile,row.index_compatibility_uuid,generation,0,0,0).disposition==
          dt::BlobRawIndexPolicyDispositionV3::compatibility_binding_invalid,"wrong datatype/index compatibility generation admitted");
    for (std::size_t i=0;i<16;++i) {
      auto wrong=row.index_compatibility_uuid; wrong.bytes[i]^=0x80;
      const auto refused=dt::SelectBlobRawIndexPolicyNoAllocV3(f.profile,wrong,1,0,0,0);
      Check(refused.disposition==dt::BlobRawIndexPolicyDispositionV3::compatibility_binding_invalid &&
          refused.diagnostic_code.empty(),"substituted compatibility identity fabricated policy authority");
    }
  }
  Check(hashes==1 && refusals==15,"raw BLOB family policy is not hash-only");
  auto bad=f.profile; bad.policy_bindings[static_cast<std::size_t>(dt::BlobPolicyKindV3::index)].generation++;
  Check(dt::SelectBlobRawIndexPolicyNoAllocV3(bad,kBlobIndexCompatibilityGolden[3].index_compatibility_uuid,1,0,0,0).disposition==
      dt::BlobRawIndexPolicyDispositionV3::profile_invalid,"unbound policy emitted a B-tree refusal");
  Check(dt::SelectBlobRawIndexPolicyNoAllocV3(f.profile,kBlobIndexCompatibilityGolden[10].index_compatibility_uuid,1,
      dt::kBlobMaximumLogicalBytesV3+1,0,0).disposition==dt::BlobRawIndexPolicyDispositionV3::input_invalid,
      "impossible policy input bound admitted");
  Clean(f);
}
