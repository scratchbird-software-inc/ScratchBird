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
  return E::none;
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
    std::copy_n("SBPAG001", 8, family);
    StoreLittle16(family + 8, 1);
    StoreLittle16(family + 10, 384);
    StoreLittle32(family + 12, kUsed);
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
    const auto state_hash = core::hash::ComputeSha256Digest(family, 240);
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
    if (std::string_view(reinterpret_cast<const char*>(family), 8) != "SBPAG001" ||
        LoadLittle16(family + 8) != 1 || LoadLittle16(family + 10) != 384 ||
        LoadLittle32(family + 12) != kUsed || !Zero(family + 304, 80) ||
        !Zero(bytes.data() + kUsed, bytes.size() - kUsed)) return Fail(E::invalid_family);
    const auto state_hash = core::hash::ComputeSha256Digest(family, 240);
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
      for (const auto* header : {&a.header, &b.header})
        if (state->base_checkpoint.filespace_uuid == header->filespace_uuid &&
            state->base_checkpoint.page_number == header->page_number) return Fail(E::invalid_pair);
    if (a.watermark == b.watermark) {
      if (!std::equal(first.begin() + 128, first.begin() + 400, second.begin() + 128))
        return Fail(E::invalid_pair);
      return left;
    }
    const auto& older = a.watermark < b.watermark ? left : right;
    const auto& newer = a.watermark > b.watermark ? left : right;
    const auto& old_state = *older.state;
    const auto& new_state = *newer.state;
    if (new_state.previous_watermark != old_state.watermark ||
        new_state.previous_state_sha256 != older.state_sha256 ||
        new_state.operation_uuid == old_state.operation_uuid ||
        new_state.base_checkpoint_generation < old_state.base_checkpoint_generation ||
        new_state.base_checkpoint_generation > old_state.watermark ||
        new_state.base_root_set_generation < old_state.base_root_set_generation ||
        (new_state.base_checkpoint_generation == old_state.base_checkpoint_generation &&
         !SameBase(new_state, old_state))) return Fail(E::invalid_pair);
    return Fail(E::repair_required);
  } catch (const std::bad_alloc&) { return Fail(E::resource_exhausted); }
    catch (const std::length_error&) { return Fail(E::resource_exhausted); }
    catch (...) { return Fail(E::invalid_pair); }
}
}  // namespace scratchbird::storage::database
