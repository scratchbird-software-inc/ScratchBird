// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_watermark.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E = NativePublicationWatermarkError;
constexpr std::size_t kUsed = 512, kStateSeal = 368, kImageSeal = 400;
constexpr std::array<byte,8> kPlanDomain{'S','B','P','A','G','S','T','3'};
constexpr std::array<byte,8> kIntentDomain{'S','B','P','A','G','S','T','2'};
constexpr std::array<byte,8> kResolutionDomain{'S','B','P','A','G','S','T','4'};

bool Zero(const byte* bytes, std::size_t size) {
  return std::all_of(bytes, bytes + size, [](byte value) { return value == 0; });
}
bool V7(const Uuid& id) {
  return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
}
Uuid GetUuid(const byte* bytes) {
  Uuid id;
  std::copy_n(bytes, 16, id.bytes.begin());
  return id;
}
void PutUuid(byte* bytes, const Uuid& id) {
  std::copy(id.bytes.begin(), id.bytes.end(), bytes);
}
disk::NativePageReference GetRef(const byte* bytes) {
  return {GetUuid(bytes), LoadLittle64(bytes + 16), LoadLittle64(bytes + 24),
          GetUuid(bytes + 32)};
}
void PutRef(byte* bytes, const disk::NativePageReference& ref) {
  PutUuid(bytes, ref.filespace_uuid);
  StoreLittle64(bytes + 16, ref.page_number);
  StoreLittle64(bytes + 24, ref.page_generation);
  PutUuid(bytes + 32, ref.page_size_profile_uuid);
}
bool ValidRef(const disk::NativePageReference& ref) {
  const auto* profile = disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
  return V7(ref.filespace_uuid) && profile && ref.page_number && ref.page_generation &&
         ref.page_number < std::numeric_limits<u64>::max() / profile->page_size_bytes &&
         disk::CheckFileDeviceExtent(ref.page_number * profile->page_size_bytes,
                                     profile->page_size_bytes).ok();
}
NativePublicationWatermarkImage Fail(E error) {
  NativePublicationWatermarkImage result;
  result.error = error;
  return result;
}
auto ImageDigest(const std::vector<byte>& bytes) {
  const std::array<byte, 32> zeros{};
  const core::hash::HashDigestSegment parts[] = {
      {bytes.data(), kImageSeal}, {zeros.data(), zeros.size()},
      {bytes.data() + kImageSeal + 32, bytes.size() - kImageSeal - 32}};
  return core::hash::ComputeSha256DigestParts(parts, 3);
}
E Validate(const NativePublicationWatermark& state) {
  const auto& header = state.header;
  if (!disk::EncodeNativeCommonPageHeader(header).ok() ||
      header.page_type != 0x0500 || header.flags) return E::invalid_header;
  for (const auto* id : {&state.object_uuid, &state.bootstrap_uuid, &state.timeline_uuid,
                        &state.operation_uuid, &state.base_checkpoint_object_uuid})
    if (!V7(*id)) return E::invalid_identity;
  if (state.object_uuid == state.bootstrap_uuid || header.page_uuid == state.object_uuid ||
      header.page_uuid == state.bootstrap_uuid) return E::invalid_identity;
  if (!state.watermark || !state.base_checkpoint_generation || !state.base_root_set_generation ||
      state.base_checkpoint_generation > state.watermark ||
      Zero(state.base_checkpoint_sha256.data(), 32)) return E::invalid_family;
  if (state.watermark == 1) {
    if (state.previous_watermark || !Zero(state.previous_state_sha256.data(), 32))
      return E::invalid_family;
  } else if (state.previous_watermark != state.watermark - 1 ||
             Zero(state.previous_state_sha256.data(), 32)) return E::invalid_family;
  if (!ValidRef(state.base_checkpoint)) return E::invalid_reference;
  if (state.base_checkpoint.filespace_uuid == header.filespace_uuid &&
      (state.base_checkpoint.page_size_profile_uuid != header.page_size_profile_uuid ||
       state.base_checkpoint.page_number == header.page_number)) return E::invalid_reference;
  if(state.intent){const auto& intent=*state.intent;
    if(!V7(intent.initiator_uuid)||!V7(intent.request_context_uuid)||!V7(intent.policy_snapshot_uuid))return E::invalid_identity;
    if(state.watermark==1||intent.initiator_kind<1||intent.initiator_kind>8||intent.recovery_profile>1||
       Zero(intent.normalized_request_sha256.data(),32))return E::invalid_family;
  }
  if(state.publication_plan){const auto& plan=*state.publication_plan;
    if(!state.intent||!V7(plan.object_uuid)||plan.object_uuid==state.object_uuid||plan.object_uuid==state.bootstrap_uuid||
       Zero(plan.sha256.data(),32)||Zero(plan.reservation_state_sha256.data(),32))return E::invalid_family;
    if(!ValidRef(plan.page)||plan.page.filespace_uuid!=header.filespace_uuid||
       plan.page.page_size_profile_uuid!=header.page_size_profile_uuid||plan.page.page_number==header.page_number||
       (plan.page.filespace_uuid==state.base_checkpoint.filespace_uuid&&plan.page.page_number==state.base_checkpoint.page_number))return E::invalid_reference;
  }
  if(state.abandonment){const auto& resolution=*state.abandonment;
    if(!state.intent||state.intent->recovery_profile!=1||!V7(resolution.resolution_uuid)||
       resolution.resolution_uuid==state.operation_uuid||Zero(resolution.pending_state_sha256.data(),32))return E::invalid_family;
  }
  return E::none;
}
auto StateDigest(const byte* family,u16 version) {
  if(version==1)return core::hash::ComputeSha256Digest(family,240);
  const auto& domain=version==4?kResolutionDomain:version==3?kPlanDomain:kIntentDomain;
  const core::hash::HashDigestSegment parts[]={{domain.data(),domain.size()},
    {family,240},{family+304,version>=3?336u:208u}};
  return core::hash::ComputeSha256DigestParts(parts,3);
}
auto OriginDigest(const byte* family,u16 version) {
  std::array<byte,640> original{};
  std::copy_n(family,240,original.begin());std::copy_n(family+304,84,original.begin()+304);
  if(version==4)std::copy_n(family+516,2,original.begin()+516);
  std::copy_n(version==4?"SBPAG004":"SBPAG002",8,original.begin());StoreLittle16(original.data()+8,version==4?4:2);
  StoreLittle16(original.data()+10,version==4?640:512);StoreLittle32(original.data()+12,version==4?768:640);
  return StateDigest(original.data(),version==4?4:2);
}
auto PendingDigest(const byte* family) {
  std::array<byte,640> pending{};std::copy_n(family,640,pending.begin());
  std::fill(pending.begin()+518,pending.begin()+568,0);
  return StateDigest(pending.data(),4);
}
bool SameBase(const NativePublicationWatermark& a, const NativePublicationWatermark& b) {
  return a.base_checkpoint == b.base_checkpoint &&
         a.base_checkpoint_object_uuid == b.base_checkpoint_object_uuid &&
         a.base_checkpoint_sha256 == b.base_checkpoint_sha256 &&
         a.base_root_set_generation == b.base_root_set_generation;
}
}  // namespace

NativePublicationWatermarkImage EncodeNativePublicationWatermark(
    const NativePublicationWatermark& state) noexcept {
  try {
    const auto valid = Validate(state);
    if (valid != E::none) return Fail(valid);
    const auto common = disk::EncodeNativeCommonPageHeader(state.header);
    if (!common.ok()) return Fail(E::invalid_header);
    std::vector<byte> bytes(state.header.page_size_bytes, 0);
    std::copy(common.bytes->begin(), common.bytes->end(), bytes.begin());
    auto* family = bytes.data() + 128;
    const bool intent=state.intent.has_value();const u16 version=intent&&state.intent->recovery_profile?4:state.publication_plan?3:intent?2:1;
    std::copy_n(version==4?"SBPAG004":version==3?"SBPAG003":intent?"SBPAG002":"SBPAG001", 8, family);
    StoreLittle16(family + 8, version);
    StoreLittle16(family + 10, version>=3?640:intent?512:384);
    StoreLittle32(family + 12, version>=3?768:intent?640:kUsed);
    PutUuid(family + 16, state.object_uuid);
    PutUuid(family + 32, state.bootstrap_uuid);
    PutUuid(family + 48, state.timeline_uuid);
    StoreLittle64(family + 64, state.watermark);
    StoreLittle64(family + 72, state.base_checkpoint_generation);
    StoreLittle64(family + 80, state.base_root_set_generation);
    StoreLittle64(family + 88, state.previous_watermark);
    PutUuid(family + 96, state.operation_uuid);
    PutRef(family + 112, state.base_checkpoint);
    PutUuid(family + 160, state.base_checkpoint_object_uuid);
    std::copy(state.base_checkpoint_sha256.begin(), state.base_checkpoint_sha256.end(), family + 176);
    std::copy(state.previous_state_sha256.begin(), state.previous_state_sha256.end(), family + 208);
    if(intent){const auto& request=*state.intent;
      PutUuid(family+304,request.initiator_uuid);PutUuid(family+320,request.request_context_uuid);
      PutUuid(family+336,request.policy_snapshot_uuid);
      std::copy(request.normalized_request_sha256.begin(),request.normalized_request_sha256.end(),family+352);
      StoreLittle16(family+384,request.initiator_kind);StoreLittle16(family+386,1);
    }
    if(version==4)StoreLittle16(family+516,state.intent->recovery_profile);
    if(state.abandonment){StoreLittle16(family+518,1);PutUuid(family+520,state.abandonment->resolution_uuid);
      std::copy(state.abandonment->pending_state_sha256.begin(),state.abandonment->pending_state_sha256.end(),family+536);
    }
    if(state.publication_plan){const auto& plan=*state.publication_plan;
      PutRef(family+388,plan.page);PutUuid(family+436,plan.object_uuid);
      std::copy(plan.sha256.begin(),plan.sha256.end(),family+452);
      std::copy(plan.reservation_state_sha256.begin(),plan.reservation_state_sha256.end(),family+484);
      const auto origin=OriginDigest(family,version);if(!origin.ok())return Fail(E::hash_failure);
      if(origin.digest!=plan.reservation_state_sha256)return Fail(E::invalid_integrity);
    }
    if(state.abandonment){const auto pending=PendingDigest(family);if(!pending.ok())return Fail(E::hash_failure);
      if(pending.digest!=state.abandonment->pending_state_sha256)return Fail(E::invalid_integrity);
    }
    const auto state_hash = StateDigest(family,version);
    if (!state_hash.ok()) return Fail(E::hash_failure);
    std::copy(state_hash.digest.begin(), state_hash.digest.end(), bytes.begin() + kStateSeal);
    const auto image_hash = ImageDigest(bytes);
    if (!image_hash.ok()) return Fail(E::hash_failure);
    std::copy(image_hash.digest.begin(), image_hash.digest.end(), bytes.begin() + kImageSeal);
    return {E::none, state, state_hash.digest, std::move(bytes)};
  } catch (const std::bad_alloc&) { return Fail(E::resource_exhausted); }
    catch (const std::length_error&) { return Fail(E::resource_exhausted); }
    catch (...) { return Fail(E::invalid_family); }
}

NativePublicationWatermarkImage DecodeNativePublicationWatermark(
    const std::vector<byte>& bytes) noexcept {
  try {
    if (bytes.size() < kUsed) return Fail(E::invalid_header);
    const auto common = disk::DecodeNativeCommonPageHeader(bytes.data(), 128);
    if (!common.ok() || common.header->page_type != 0x0500 || common.header->flags ||
        bytes.size() != common.header->page_size_bytes) return Fail(E::invalid_header);
    const auto image_hash = ImageDigest(bytes);
    if (!image_hash.ok()) return Fail(E::hash_failure);
    if (!std::equal(image_hash.digest.begin(), image_hash.digest.end(), bytes.begin() + kImageSeal))
      return Fail(E::invalid_integrity);
    const auto* family = bytes.data() + 128;
    const auto version=LoadLittle16(family+8);
    const bool intent=version>=2;const std::size_t used=version>=3?768:intent?640:kUsed;
    if (version<1||version>4||std::string_view(reinterpret_cast<const char*>(family),8)!=(version==4?"SBPAG004":version==3?"SBPAG003":intent?"SBPAG002":"SBPAG001")||
        LoadLittle16(family+10)!=(version>=3?640:intent?512:384)||LoadLittle32(family+12)!=used||
        !Zero(family+(version==4?568:version==3?516:intent?388:304),version==4?72:intent?124:80)||
        (intent&&LoadLittle16(family+386)!=1)||!Zero(bytes.data()+used,bytes.size()-used))return Fail(E::invalid_family);
    if(version==4&&(LoadLittle16(family+516)!=1||LoadLittle16(family+518)>1||
       (!LoadLittle16(family+518)&&!Zero(family+520,48))))return Fail(E::invalid_family);
    const bool anchor=version==3||(version==4&&!Zero(family+388,128));
    if(anchor){const auto origin=OriginDigest(family,version);if(!origin.ok())return Fail(E::hash_failure);
      if(!std::equal(origin.digest.begin(),origin.digest.end(),family+484))return Fail(E::invalid_integrity);
    }
    if(version==4&&LoadLittle16(family+518)){const auto pending=PendingDigest(family);if(!pending.ok())return Fail(E::hash_failure);
      if(!std::equal(pending.digest.begin(),pending.digest.end(),family+536))return Fail(E::invalid_integrity);
    }
    const auto state_hash = StateDigest(family,version);
    if (!state_hash.ok()) return Fail(E::hash_failure);
    if (!std::equal(state_hash.digest.begin(), state_hash.digest.end(), bytes.begin() + kStateSeal))
      return Fail(E::invalid_integrity);
    NativePublicationWatermark state;
    state.header = *common.header;
    state.object_uuid = GetUuid(family + 16);
    state.bootstrap_uuid = GetUuid(family + 32);
    state.timeline_uuid = GetUuid(family + 48);
    state.watermark = LoadLittle64(family + 64);
    state.base_checkpoint_generation = LoadLittle64(family + 72);
    state.base_root_set_generation = LoadLittle64(family + 80);
    state.previous_watermark = LoadLittle64(family + 88);
    state.operation_uuid = GetUuid(family + 96);
    state.base_checkpoint = GetRef(family + 112);
    state.base_checkpoint_object_uuid = GetUuid(family + 160);
    std::copy_n(family + 176, 32, state.base_checkpoint_sha256.begin());
    std::copy_n(family + 208, 32, state.previous_state_sha256.begin());
    if(intent){NativePublicationIntent request;
      request.initiator_uuid=GetUuid(family+304);request.request_context_uuid=GetUuid(family+320);
      request.policy_snapshot_uuid=GetUuid(family+336);request.initiator_kind=LoadLittle16(family+384);
      if(version==4)request.recovery_profile=LoadLittle16(family+516);
      std::copy_n(family+352,32,request.normalized_request_sha256.begin());state.intent=request;
    }
    if(anchor){NativePublicationWatermark::PlanAnchor plan;
      plan.page=GetRef(family+388);plan.object_uuid=GetUuid(family+436);
      std::copy_n(family+452,32,plan.sha256.begin());std::copy_n(family+484,32,plan.reservation_state_sha256.begin());
      state.publication_plan=plan;
    }
    if(version==4&&LoadLittle16(family+518)){NativePublicationWatermark::Abandonment resolution;
      resolution.resolution_uuid=GetUuid(family+520);std::copy_n(family+536,32,resolution.pending_state_sha256.begin());state.abandonment=resolution;
    }
    const auto valid = Validate(state);
    if (valid != E::none) return Fail(valid);
    return {E::none, std::move(state), state_hash.digest, bytes};
  } catch (const std::bad_alloc&) { return Fail(E::resource_exhausted); }
    catch (const std::length_error&) { return Fail(E::resource_exhausted); }
    catch (...) { return Fail(E::invalid_family); }
}

NativePublicationWatermarkImage ClassifyNativePublicationWatermarkPair(
    const std::vector<byte>& first, const std::vector<byte>& second) noexcept {
  try {
    auto left = DecodeNativePublicationWatermark(first);
    auto right = DecodeNativePublicationWatermark(second);
    for (const auto* result : {&left, &right})
      if (result->error == E::hash_failure || result->error == E::resource_exhausted)
        return Fail(result->error);
    if (!left.ok() || !right.ok())
      return Fail(left.ok() || right.ok() ? E::repair_required : E::invalid_pair);
    const auto& a = *left.state;
    const auto& b = *right.state;
    if (a.header.database_uuid != b.header.database_uuid ||
        a.header.filespace_uuid != b.header.filespace_uuid ||
        a.header.page_size_profile_uuid != b.header.page_size_profile_uuid ||
        a.header.page_number == b.header.page_number || a.header.page_uuid == b.header.page_uuid ||
        a.object_uuid != b.object_uuid || a.bootstrap_uuid != b.bootstrap_uuid ||
        a.timeline_uuid != b.timeline_uuid) return Fail(E::invalid_pair);
    for (const auto* state : {&a, &b})
      for (const auto* header : {&a.header, &b.header}) {
        if (state->base_checkpoint.filespace_uuid == header->filespace_uuid &&
            state->base_checkpoint.page_number == header->page_number) return Fail(E::invalid_pair);
        if(state->publication_plan&&state->publication_plan->page.filespace_uuid==header->filespace_uuid&&
           state->publication_plan->page.page_number==header->page_number)return Fail(E::invalid_pair);
      }
    if (a.watermark == b.watermark) {
      if(bool(a.abandonment)!=bool(b.abandonment)){
        const auto& resolved=a.abandonment?left:right;const auto& pending=a.abandonment?right:left;
        return Fail(pending.state->intent&&pending.state->intent->recovery_profile==1&&
          resolved.state->abandonment->pending_state_sha256==pending.state_sha256?E::repair_required:E::invalid_pair);
      }
      if(bool(a.publication_plan)!=bool(b.publication_plan)){
        if(a.abandonment||b.abandonment)return Fail(E::invalid_pair);
        const auto& attached=a.publication_plan?left:right;const auto& original=a.publication_plan?right:left;
        return Fail(original.state->intent&&attached.state->publication_plan->reservation_state_sha256==original.state_sha256?E::repair_required:E::invalid_pair);
      }
      if (a.publication_plan!=b.publication_plan||a.abandonment!=b.abandonment||a.intent!=b.intent||!std::equal(first.begin() + 128, first.begin() + 400, second.begin() + 128))
        return Fail(E::invalid_pair);
      return left;
    }
    const auto& older = a.watermark < b.watermark ? left : right;
    const auto& newer = a.watermark > b.watermark ? left : right;
    const auto& old_state = *older.state;
    const auto& new_state = *newer.state;
    if (new_state.publication_plan||new_state.abandonment||new_state.previous_watermark != old_state.watermark ||
        new_state.previous_state_sha256 != older.state_sha256 ||
        new_state.operation_uuid == old_state.operation_uuid ||
        new_state.base_checkpoint_generation < old_state.base_checkpoint_generation ||
        new_state.base_checkpoint_generation > old_state.watermark ||
        (old_state.intent&&!old_state.abandonment&&new_state.base_checkpoint_generation!=old_state.watermark)||
        new_state.base_root_set_generation < old_state.base_root_set_generation ||
        (new_state.base_checkpoint_generation == old_state.base_checkpoint_generation &&
         !SameBase(new_state, old_state))) return Fail(E::invalid_pair);
    return Fail(E::repair_required);
  } catch (const std::bad_alloc&) { return Fail(E::resource_exhausted); }
    catch (const std::length_error&) { return Fail(E::resource_exhausted); }
    catch (...) { return Fail(E::invalid_pair); }
}
}  // namespace scratchbird::storage::database
